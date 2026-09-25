#pragma once

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <ebus/types.hpp>
#include <mutex>

#include "pwm_calibration_selector.hpp"

namespace ebus::detail {
class JsonWriter;
}

class PwmCalibrationManager {
 public:
  bool begin();
  bool registerHandlers(httpd_handle_t server);
  void toJson(ebus::detail::JsonWriter& writer) const;

 private:
  enum class State : uint8_t {
    unavailable,
    idle,
    preparing_observation,
    observation_settling,
    observing,
    observed,
    preparing_sweep,
    settling,
    measuring,
    confirmation_settling,
    confirmation_measuring,
    awaiting_validation,
    accepted,
    rolled_back,
    failed,
  };

  enum class AcceptResult : uint8_t { accepted, invalid_state, persist_failed };

  static void taskEntry(void* argument);
  void taskLoop();
  void service();
  void sampleInputLocked();
  void resetMeasurementLocked();
  bool finishMeasurementLocked();
  bool restoreOriginalLocked(State state, bool persist);
  bool startObservation();
  bool startSweep();
  AcceptResult accept();
  bool requestRollback();
  bool busyLocked() const;
  const char* stateNameLocked() const;

  static esp_err_t handleObserve(httpd_req_t* req);
  static esp_err_t handleStart(httpd_req_t* req);
  static esp_err_t handleAccept(httpd_req_t* req);
  static esp_err_t handleRollback(httpd_req_t* req);
  static bool authenticate(httpd_req_t* req);

  mutable std::mutex mutex_;
  TaskHandle_t task_handle_ = nullptr;
  State state_ = State::unavailable;
  PwmCalibrationSweep sweep_;
  uint8_t original_ = 130;
  uint8_t selected_ = 130;
  uint64_t stage_started_at_ = 0;
  uint64_t last_syn_at_ = 0;
  uint32_t start_symbols_ = 0;
  uint32_t start_syn_ = 0;
  uint32_t start_errors_ = 0;
  uint32_t last_symbols_ = 0;
  uint32_t last_syn_ = 0;
  uint32_t last_errors_ = 0;
  uint32_t samples_ = 0;
  uint32_t high_samples_ = 0;
  uint32_t transitions_ = 0;
  bool last_level_ = false;
  bool has_last_level_ = false;
  bool last_stable_ = false;
  bool isolation_requested_ = false;
  bool has_original_ = false;
  bool rollback_requested_ = false;
  bool persistence_error_ = false;
};

extern PwmCalibrationManager pwmCalibrationManager;
