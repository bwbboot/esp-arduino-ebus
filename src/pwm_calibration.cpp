#include "pwm_calibration.hpp"

#include <driver/gpio.h>
#include <esp_timer.h>

#include <algorithm>
#include <ebus/detail/json_writer.hpp>
#include <string>

#include "app_limits.hpp"
#include "bus_type.hpp"
#include "client.hpp"
#include "config_manager.hpp"
#include "http.hpp"
#include "http_utils.hpp"
#include "main.hpp"

PwmCalibrationManager pwmCalibrationManager;

namespace {
constexpr uint64_t observe_ms = 30000;
constexpr uint64_t settle_ms = 250;
constexpr uint64_t measure_ms = 2000;
constexpr uint64_t prepare_timeout_ms = 5000;
constexpr uint64_t validation_ms = 10 * 60 * 1000;
constexpr uint64_t validation_signal_timeout_ms = 5000;

uint64_t nowMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

uint8_t currentPwm() {
  return static_cast<uint8_t>(std::min<uint32_t>(get_pwm(), 255));
}
}  // namespace

bool PwmCalibrationManager::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_handle_ != nullptr) return true;
  task_handle_ = nullptr;
  const BaseType_t result = xTaskCreate(
      taskEntry, "pwm_calibration", app::limits::Task::pwm_calibration_stack,
      this, app::limits::Task::pwm_calibration_priority, &task_handle_);
  state_ = result == pdPASS ? State::idle : State::unavailable;
  return result == pdPASS;
}

bool PwmCalibrationManager::registerHandlers(httpd_handle_t server) {
  if (server == nullptr) return false;
  return RegisterUri("/api/v1/pwm-calibration/observe", HTTP_POST,
                     handleObserve) &&
         RegisterUri("/api/v1/pwm-calibration/start", HTTP_POST,
                     handleStart) &&
         RegisterUri("/api/v1/pwm-calibration/accept", HTTP_POST,
                     handleAccept) &&
         RegisterUri("/api/v1/pwm-calibration/rollback", HTTP_POST,
                     handleRollback);
}

bool PwmCalibrationManager::authenticate(httpd_req_t* req) {
  return HttpUtils::requireAdminAuth(req);
}

esp_err_t PwmCalibrationManager::handleObserve(httpd_req_t* req) {
  if (!authenticate(req)) return ESP_OK;
  if (!pwmCalibrationManager.startObservation()) {
    HttpUtils::sendErrorResponse(req, "409 Conflict", "pwm_observe",
                                 "PWM calibration is unavailable or already active");
    return ESP_OK;
  }
  HttpUtils::sendResponse(req, "202 Accepted", "text/plain",
                          "Passive PWM observation started for 30 seconds\n");
  return ESP_OK;
}

esp_err_t PwmCalibrationManager::handleStart(httpd_req_t* req) {
  if (!authenticate(req)) return ESP_OK;
  if (!pwmCalibrationManager.startSweep()) {
    HttpUtils::sendErrorResponse(req, "409 Conflict", "pwm_calibration",
                                 "PWM calibration is unavailable or already active");
    return ESP_OK;
  }
  HttpUtils::sendResponse(
      req, "202 Accepted", "text/plain",
      "Passive PWM sweep started; validate the temporary candidate before accepting it\n");
  return ESP_OK;
}

esp_err_t PwmCalibrationManager::handleAccept(httpd_req_t* req) {
  if (!authenticate(req)) return ESP_OK;
  const AcceptResult result = pwmCalibrationManager.accept();
  if (result == AcceptResult::persist_failed) {
    HttpUtils::sendErrorResponse(req, "500 Internal Server Error", "pwm_accept",
                                 "Failed to persist the PWM candidate");
    return ESP_OK;
  }
  if (result != AcceptResult::accepted) {
    HttpUtils::sendErrorResponse(req, "409 Conflict", "pwm_accept",
                                 "No PWM candidate is awaiting validation");
    return ESP_OK;
  }
  HttpUtils::sendResponse(req, "200 OK", "text/plain",
                          "PWM candidate accepted and persisted\n");
  return ESP_OK;
}

esp_err_t PwmCalibrationManager::handleRollback(httpd_req_t* req) {
  if (!authenticate(req)) return ESP_OK;
  if (!pwmCalibrationManager.requestRollback()) {
    HttpUtils::sendErrorResponse(req, "409 Conflict", "pwm_rollback",
                                 "No active PWM calibration can be rolled back");
    return ESP_OK;
  }
  HttpUtils::sendResponse(req, "202 Accepted", "text/plain",
                          "PWM rollback requested\n");
  return ESP_OK;
}

bool PwmCalibrationManager::busyLocked() const {
  return state_ == State::preparing_observation ||
         state_ == State::observation_settling || state_ == State::observing ||
         state_ == State::preparing_sweep || state_ == State::settling ||
         state_ == State::measuring ||
         state_ == State::confirmation_settling ||
         state_ == State::confirmation_measuring ||
         state_ == State::awaiting_validation;
}

bool PwmCalibrationManager::startObservation() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_handle_ == nullptr || busyLocked()) return false;
  original_ = currentPwm();
  selected_ = original_;
  isolation_requested_ = false;
  has_original_ = true;
  rollback_requested_ = false;
  persistence_error_ = false;
  stage_started_at_ = nowMs();
  state_ = State::preparing_observation;
  return true;
}

bool PwmCalibrationManager::startSweep() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_handle_ == nullptr || busyLocked()) return false;
  original_ = currentPwm();
  selected_ = original_;
  sweep_.begin(original_);
  isolation_requested_ = false;
  has_original_ = true;
  rollback_requested_ = false;
  persistence_error_ = false;
  stage_started_at_ = nowMs();
  state_ = State::preparing_sweep;
  return true;
}

PwmCalibrationManager::AcceptResult PwmCalibrationManager::accept() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::awaiting_validation) return AcceptResult::invalid_state;
  if (!configManager.writeString("pwmValue", std::to_string(selected_))) {
    persistence_error_ = true;
    return AcceptResult::persist_failed;
  }
  persistence_error_ = false;
  state_ = State::accepted;
  stage_started_at_ = nowMs();
  has_original_ = false;
  return AcceptResult::accepted;
}

bool PwmCalibrationManager::requestRollback() {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool retry_persistence =
      state_ == State::failed && persistence_error_;
  if (!has_original_ ||
      (!busyLocked() && state_ != State::observed && !retry_persistence)) {
    return false;
  }
  rollback_requested_ = true;
  return true;
}

void PwmCalibrationManager::taskEntry(void* argument) {
  static_cast<PwmCalibrationManager*>(argument)->taskLoop();
}

void PwmCalibrationManager::taskLoop() {
  for (;;) {
    service();
    State state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state = state_;
    }
    if (state == State::observing || state == State::measuring ||
        state == State::confirmation_measuring) {
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void PwmCalibrationManager::resetMeasurementLocked() {
  stage_started_at_ = nowMs();
  start_symbols_ = Bus.nbr_symbols_.load();
  start_syn_ = Bus.nbr_syn_.load();
  start_errors_ = static_cast<uint32_t>(Bus.nbr_errors_.load());
  samples_ = 0;
  high_samples_ = 0;
  transitions_ = 0;
  has_last_level_ = false;
}

void PwmCalibrationManager::sampleInputLocked() {
  const bool level = gpio_get_level(static_cast<gpio_num_t>(UART_RX)) != 0;
  ++samples_;
  if (level) ++high_samples_;
  if (has_last_level_ && level != last_level_) ++transitions_;
  last_level_ = level;
  has_last_level_ = true;
}

bool PwmCalibrationManager::finishMeasurementLocked() {
  last_symbols_ = Bus.nbr_symbols_.load() - start_symbols_;
  last_syn_ = Bus.nbr_syn_.load() - start_syn_;
  last_errors_ =
      static_cast<uint32_t>(Bus.nbr_errors_.load()) - start_errors_;
  last_stable_ = pwmSignalIsStable(
      {last_symbols_, last_syn_, samples_, high_samples_, transitions_});
  return last_stable_;
}

bool PwmCalibrationManager::restoreOriginalLocked(State state, bool persist) {
  set_pwm(original_);
  selected_ = original_;
  stage_started_at_ = nowMs();
  releaseWritableClientIsolation();
  enableTX();
  isolation_requested_ = false;
  rollback_requested_ = false;
  bool persisted = true;
  if (persist) {
    persisted = configManager.writeString("pwmValue", std::to_string(original_));
  }
  persistence_error_ = !persisted;
  state_ = persisted ? state : State::failed;
  has_original_ = persist && !persisted;
  return persisted;
}

void PwmCalibrationManager::service() {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = nowMs();

  if (rollback_requested_) {
    if (isolation_requested_ && !writableClientIsolationReady()) {
      if (now - stage_started_at_ >= prepare_timeout_ms) {
        restoreOriginalLocked(State::failed, false);
      }
      return;
    }
    restoreOriginalLocked(State::rolled_back, true);
    return;
  }

  if (state_ == State::preparing_observation ||
      state_ == State::preparing_sweep) {
    if (now - stage_started_at_ >= prepare_timeout_ms) {
      restoreOriginalLocked(State::failed, false);
      return;
    }
    if (!isolation_requested_) {
      disableTX();
      requestWritableClientIsolation();
      isolation_requested_ = true;
      return;
    }
    if (!writableClientIsolationReady()) return;
    if (state_ == State::preparing_observation) {
      state_ = State::observation_settling;
      stage_started_at_ = now;
    } else {
      set_pwm(sweep_.candidate());
      state_ = State::settling;
      stage_started_at_ = now;
    }
    return;
  }

  if (state_ == State::observation_settling &&
      now - stage_started_at_ >= settle_ms) {
    state_ = State::observing;
    resetMeasurementLocked();
    return;
  }

  if (state_ == State::observing || state_ == State::measuring ||
      state_ == State::confirmation_measuring) {
    sampleInputLocked();
  }

  const uint64_t elapsed = now - stage_started_at_;
  if (state_ == State::observing && elapsed >= observe_ms) {
    finishMeasurementLocked();
    state_ = State::observed;
    stage_started_at_ = now;
    releaseWritableClientIsolation();
    enableTX();
    isolation_requested_ = false;
  } else if (state_ == State::settling && elapsed >= settle_ms) {
    state_ = State::measuring;
    resetMeasurementLocked();
  } else if (state_ == State::measuring && elapsed >= measure_ms) {
    const bool stable = finishMeasurementLocked();
    if (sweep_.record(stable)) {
      if (!sweep_.foundStableBand()) {
        restoreOriginalLocked(State::failed, false);
      } else {
        selected_ = sweep_.selected();
        set_pwm(selected_);
        state_ = State::confirmation_settling;
        stage_started_at_ = now;
      }
    } else {
      set_pwm(sweep_.candidate());
      state_ = State::settling;
      stage_started_at_ = now;
    }
  } else if (state_ == State::confirmation_settling &&
             elapsed >= settle_ms) {
    state_ = State::confirmation_measuring;
    resetMeasurementLocked();
  } else if (state_ == State::confirmation_measuring &&
             elapsed >= measure_ms) {
    if (!finishMeasurementLocked()) {
      restoreOriginalLocked(State::failed, false);
    } else {
      state_ = State::awaiting_validation;
      stage_started_at_ = now;
      last_syn_at_ = now;
      start_syn_ = Bus.nbr_syn_.load();
      releaseWritableClientIsolation();
      enableTX();
      isolation_requested_ = false;
    }
  } else if (state_ == State::awaiting_validation) {
    const uint32_t syn = Bus.nbr_syn_.load();
    if (syn != start_syn_) {
      start_syn_ = syn;
      last_syn_at_ = now;
    }
    if (now - last_syn_at_ >= validation_signal_timeout_ms ||
        elapsed >= validation_ms) {
      restoreOriginalLocked(State::rolled_back, false);
    }
  }
}

const char* PwmCalibrationManager::stateNameLocked() const {
  switch (state_) {
    case State::unavailable:
      return "unavailable";
    case State::idle:
      return "idle";
    case State::preparing_observation:
      return "preparing_observation";
    case State::observation_settling:
      return "observation_settling";
    case State::observing:
      return "observing";
    case State::observed:
      return "observed";
    case State::preparing_sweep:
      return "preparing_sweep";
    case State::settling:
      return "settling";
    case State::measuring:
      return "measuring";
    case State::confirmation_settling:
      return "confirmation_settling";
    case State::confirmation_measuring:
      return "confirmation_measuring";
    case State::awaiting_validation:
      return "awaiting_validation";
    case State::accepted:
      return "accepted";
    case State::rolled_back:
      return "rolled_back";
    case State::failed:
      return "failed";
  }
  return "unknown";
}

void PwmCalibrationManager::toJson(ebus::detail::JsonWriter& writer) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto scope = writer.objectScope();
  writer.writeField("state", stateNameLocked());
  writer.writeField("original", original_);
  writer.writeField("candidate", sweep_.candidate());
  writer.writeField("selected", selected_);
  writer.writeField("stable_min", sweep_.stableStart());
  writer.writeField("stable_max", sweep_.stableEnd());
  writer.writeField("last_symbols", last_symbols_);
  writer.writeField("last_syn", last_syn_);
  writer.writeField("last_arbitration_errors", last_errors_);
  writer.writeField("last_transitions", transitions_);
  writer.writeField("last_stable", last_stable_);
  writer.writeField("persistence_error", persistence_error_);
}
