# esp-arduino-ebus

ESP-based Wi-Fi firmware for eBUS adapter hardware

**Warning: Do not power your adapter from a power supply on eBus terminals - you will burn the transmit circuit (receive may still work)!**

To get more info navigate to [wiki](https://github.com/danielkucera/esp-arduino-ebus/wiki)

`esp-arduino-ebus` is an open-source firmware for [EBUS to WiFi Adapter Module](https://danman.eu/ebus-adapter).  
It turns the adapter into a **network-connected eBUS interface** with TCP, MQTT, HTTP, and Home Assistant support — suitable for monitoring and controlling eBUS-based heating systems.

> ⚠️ **This firmware is designed to run only on supported eBUS adapter boards.**  
> It is **not** intended for bare ESP modules without the required eBUS interface circuitry.

---

## What This Project Does

- 🔌 Connects to **eBUS heating systems** (Vaillant and other eBUS-compatible HVAC equipment)
- 📡 Bridges the physical eBUS line to **Wi-Fi / Ethernet**
- 🌍 Exposes eBUS traffic over **TCP sockets** compatible with tools like `ebusd`
- 📊 Publishes data to **MQTT** for smart home integration
- 🏠 Supports **Home Assistant autodiscovery**
- ⚙️ Provides a **web interface** for configuration and diagnostics

---

## Required Hardware

This firmware **requires a compatible eBUS adapter board**, which provides:

- Proper **eBUS level shifting and electrical protection**
- Safe **bus power handling**
- Signal conditioning (PWM / comparator circuitry)
- Reliable physical connection to the eBUS line

Supported hardware revisions include multiple ESP32-based eBUS adapter boards maintained alongside this project.

> ❌ Flashing this firmware onto a generic ESP8266/ESP32 module **will not work**.

---

## 🧠 INTERNAL Firmware Mode

In addition to acting as a network bridge, `esp-arduino-ebus` offers an advanced **INTERNAL firmware mode**, allowing the adapter to behave as an **active, autonomous eBUS participant**.

### Key INTERNAL Features

- 🧾 **Internal command store** for eBUS messages
- 💾 **Persistent storage** in flash, restored after reboot
- 🔍 **Automatic eBUS device scanning**
- 🔄 **Active and passive operation** on the bus
- 📡 **Full remote control via MQTT or HTTP**

### INTERNAL Control Capabilities

- Insert or remove stored eBUS commands
- Send single or periodic commands
- Publish stored values on demand
- Enable filtering and forwarding rules
- Trigger bus scans and internal resets

This mode enables **standalone operation** without requiring external software such as `ebusd`.

---

## Smart Home Integration

- Native **MQTT support**
- **Home Assistant autodiscovery**
- Seamless integration into existing automation setups
- Suitable for dashboards, logging, and energy optimization

---

## Why Choose esp-arduino-ebus?

- ✅ Designed for **real eBUS adapter hardware**
- 🔓 Fully **open source**
- ⚡ Low-power, always-on operation
- 🔧 Flexible: bridge mode or INTERNAL standalone mode
- 🧩 Compatible with existing eBUS tools and ecosystems

---

## Web administration

Use the web interface's **Restart** button for an authenticated POST request;
GET requests no longer restart the adapter. The username is `admin` and the
password is the configured `apModePassword`. Change the factory default before
using administration on a shared network. HTTP Basic authentication is not
transport encryption: keep administration on a trusted LAN, never the public
Internet.

Configuration reads/writes/resets, restart and HTTP firmware upload/URL updates
require authentication. Configuration JSON masks stored Wi-Fi, MQTT and admin
passwords as `********`; saving that placeholder preserves the existing secret.
An explicit empty value is not a placeholder. Administration rejects mismatched
Origin/Host headers when Origin is present; this is additional browser protection,
not a replacement for credentials or a firewall.

Normal builds disable the unauthenticated legacy ESPOTA service. Explicit
`esp32-c3-ota` and `esp32-c3-internal-ota` development environments opt back in
for isolated trusted networks. Authenticated HTTP OTA remains available.

Validation checklist: missing/wrong credentials must reject restart, config and
upload; a rejected request must not restart or modify configuration; authenticated
restart must return202 and recover; masked secrets must survive a config save;
a cross-origin mutation must be rejected. Live combined-firmware checks verified
unauthenticated restart/upload rejection and an authenticated OTA/restart, not
every possible browser or endpoint combination.
## Passive PWM candidate selection (library groundwork)

The host-testable selector scans odd values 1 through 255, requires at least three
adjacent stable candidates and selects a tested midpoint in the widest stable
band. Ties prefer the band closer to the original setting. A failed sweep retains
the original value. This helper does not change PWM at runtime or add calibration
buttons; the authenticated, explicitly started workflow is a separate follow-up.
Receiver stability is a heuristic, not CRC or application-level read validation.

Run `c++ -std=c++17 -Wall -Wextra -Werror -Iinclude test_host/pwm_calibration_tests.cpp -o /tmp/pwm_calibration_tests && /tmp/pwm_calibration_tests`.


## Assisted PWM calibration

Assisted calibration is available in network-bridge firmware builds. The PWM
value controls the receiver threshold used to distinguish eBUS signal levels.
The suitable value can depend on adapter hardware, bus topology, cable length,
connected devices and electrical conditions.

Calibration is always started explicitly by an operator. It never adjusts PWM
continuously during normal operation.

### Observe

**Observe current PWM** monitors the signal at the current value for 30 seconds.
It does not change or persist PWM. Writable eBUS clients are temporarily
disconnected so the observation remains passive; read-only monitoring remains
available.

### Calibrate

**Start passive calibration** evaluates the supported PWM range:

1. Writable clients are isolated and new writable connections are rejected.
2. Transmission is disabled where the adapter hardware provides a TX-disable
   control.
3. Odd PWM values from 1 through 255 are allowed to settle and then measured.
4. Receiver stability is estimated from symbol flow, recurring SYN symbols,
   input transitions and both logic levels. At least three adjacent candidates
   must pass.
5. A tested value near the midpoint of the widest continuous stable range is
   measured once more and then applied temporarily.
6. Writable access resumes so the candidate can be validated with
   representative active reads under normal traffic.

A complete sweep takes approximately five minutes. Applications using writable
eBUS access may report missing data during that period.

### Validate, accept or roll back

Test representative reads after the sweep. Use **Accept validated candidate**
to persist a satisfactory result. Use **Restore original PWM** to immediately
restore and persist the value that was active before calibration.

The temporary candidate is restored automatically when no SYN activity is
detected for five seconds or when it is not accepted within ten minutes. The
original value is also restored if no stable range is found or the selected
candidate fails its confirmation measurement.

PWM calibration can improve the receive threshold, but it cannot correct every
wiring, power, topology, timing or protocol problem. A value found on one
installation must not be assumed to be suitable for another. The passive
heuristic does not validate CRCs, responses or application-level reads, so an
operator must validate representative active reads before accepting a result.

### Calibration status

`GET /api/v1/status` includes the `pwm_calibration` object with:

- state, original value, current candidate and selected value;
- stable range boundaries;
- symbol, SYN and input-transition counts plus the arbitration-error snapshot;
- the stability result for the latest measurement.

Possible states include `unavailable`, `idle`, `preparing_observation`,
`observation_settling`, `observing`, `observed`, `preparing_sweep`, `settling`,
`measuring`, `confirmation_settling`, `confirmation_measuring`,
`awaiting_validation`, `accepted`, `rolled_back` and `failed`. A
`persistence_error` flag reports a failed attempt to save a result.

### Maintenance endpoints

| Endpoint | Method | Authentication | Purpose |
| --- | --- | --- | --- |
| `/api/v1/status` | GET | No | Adapter and calibration status |
| `/restart` | POST | Admin | Controlled software restart |
| `/api/v1/pwm-calibration/observe` | POST | Admin | Observe the current PWM |
| `/api/v1/pwm-calibration/start` | POST | Admin | Start a passive PWM sweep |
| `/api/v1/pwm-calibration/accept` | POST | Admin | Persist the temporary candidate |
| `/api/v1/pwm-calibration/rollback` | POST | Admin | Restore the original value |
