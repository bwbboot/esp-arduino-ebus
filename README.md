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

The web interface provides configuration, diagnostics, firmware upgrade and
maintenance actions. Configuration and state-changing maintenance endpoints
use HTTP Basic authentication with user `admin` and the configured AP-mode
password. Stored Wi-Fi, MQTT and administrator passwords are returned to the
browser as placeholders and are not included in configuration exports.

### Restarting the adapter

Use **Restart** to perform a controlled software restart. The action:

- accepts `POST` requests only;
- requires administrator authentication;
- returns `202 Accepted` before restarting;
- preserves the network, administrator and PWM configuration.

A `GET` request to `/restart` does not restart the adapter.

Basic authentication does not encrypt HTTP traffic. Keep the management
interface on a trusted network and do not expose it directly to the internet.
Cross-origin administration requests are rejected when an `Origin` header is
present.

### Upgrading older installations

On the first boot after an application-only upgrade from the earlier
IotWebConf-based firmware, the adapter imports its device name, administrator
password, Wi-Fi credentials and PWM value before starting Wi-Fi. Existing
values in the current configuration always take precedence. The legacy data is
kept for firmware rollback, while a separate marker prevents it from being
imported again after a configuration reset.

Only upload the regular firmware image through the web interface. A full-flash
image also replaces the partition table and is intended for a serial recovery
or installation procedure.

When the installed bootloader supports OTA rollback, a pending image is
confirmed only after the HTTP recovery service, a reachable Wi-Fi interface
and the selected eBUS runtime have started. `sdkconfig.defaults` enables this
protection for new full-flash installations. An application-only upgrade does
not replace an older bootloader, so installations created without rollback
support need one serial full-flash installation before they gain this safety
net.

The upgrade page, file uploads and URL-based upgrades require administrator
authentication. The normal firmware profiles do not open the Arduino ESPOTA
port. The `*-ota` build profiles enable ESPOTA for development compatibility;
that legacy protocol has no administrator authentication and must only be used
on a trusted, isolated network.

## Wi-Fi connection policy

The configuration page provides two Wi-Fi behavior options:

- **Enable WiFi power saving** preserves the ESP-IDF modem-sleep behavior.
  Disable it when lower network latency and reduced timing jitter are more
  important than power consumption.
- **Scan all channels and select the strongest access point** evaluates every
  channel before connecting. This is useful when multiple access points
  advertise the same SSID, at the cost of a slightly longer connection scan.
  A configured BSSID still pins the connection to that access point.

When a station password is configured, open and WEP access points are excluded
from automatic selection.

The status API reports the active power-saving mode, scan method, channel,
RSSI, SSID and selected BSSID.

Wi-Fi and IP events are handled by their event family as well as their numeric
event identifier. This prevents an unrelated Wi-Fi event with the same numeric
value from being mistaken for a successful DHCP assignment. Configured devices
start in station-only mode; after repeated connection failures they enable the
recovery access point without taking down the station interface again.

## Bridge UART timing

The ESP32-C3 bridge receives bytes in a dedicated task that blocks on the
hardware UART. Network scheduling therefore does not set the arbitration
clock. SYN start time comes from the four measured falling edges of its
2400-baud waveform. Missing edges, buffered traffic, a busy transmitter or a
missed deadline cause the adapter to wait for a subsequent arbitration round.
Both arbitration rounds use the same checks. Address bytes go directly to the
UART FIFO inside a checked transmit window; no software TX queue or legacy
SoftwareSerial delay compensation is involved.

The console uses USB Serial/JTAG because GPIO21 is the eBUS RX pin on this
hardware and must not also be driven by the UART0 console. Existing generated
`sdkconfig.*` files can override `sdkconfig.defaults`: check the effective
configuration for the USB console and 4 MB flash size when reusing a build.

Host regression tests cover SYN edge recognition, timer rollover, delayed
processing, transmit deadlines and both arbitration rounds. These tests do
not replace electrical timing measurements or representative active reads on
the actual adapter. Confirm those before treating a new bridge image as
validated for deployment.

Bridge builds also reserve normal ESP-IDF Wi-Fi buffer capacity at startup:
at least 10 static RX, 32 dynamic RX and 16 static TX buffers, with a receive
Block Ack window of at least 6. This avoids inheriting the internal variant's
reduced memory budget while serving continuous TCP streams and web requests.
Larger custom limits, unlimited dynamic RX and dynamic TX mode are preserved.
The internal variant keeps its existing buffer settings. This policy needs
on-device testing with simultaneous clients; a successful build alone does
not establish network stability.

Bridge socket input uses nonblocking reads and peeks, not the optional lwIP
`FIONREAD` ioctl. Enhanced commands are decoded as a TCP byte stream: a split
two-byte command is retained per connection until complete, without executing
a partial command or blocking other clients. Host socket tests cover all
command/data combinations, split and combined packets, malformed commands,
disconnects and independent client state. Active bus operation still requires
on-device validation; successful passive reception alone does not test sending.

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
