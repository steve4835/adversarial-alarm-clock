# Adversarial Alarm Clock

An ESP32-based alarm clock designed to be difficult to dismiss. The buzzer escalates over three minutes from slow intermittent beeps to continuous tone, and dismissal requires an authenticated HTTP or MQTT request rather than a physical button.  Crucially, there is also a battery failover as well so unplugging the alarm is ineffectual.  The apparatus is in an enclosure as well, precluding the possibility of easily unplugging the power from the board either.

## Hardware

### Main components

| Component                                                                      | Link/Notes                                       |
| ------------------------------------------------------------------------------ | ------------------------------------------------ |
| ESP32 DevKit                                                                   |                                                  |
| Adafruit 1.2" 4-digit 7-segment (HT16K33, I2C 0x70)                            | https://www.adafruit.com/product/1270            |
| DS3231 RTC Module (I2C 0x68, stores local time) ((see note in "Time keeping")) | https://www.amazon.com/gp/product/B07Q7NZTQS<br> |
| Active piezo buzzer                                                            |                                                  |
| Battery                                                                        | https://www.amazon.com/dp/B0FG2P7RGP             |
| Enclosure                                                                      | https://amazon.com/dp/B0B5QGM83Y                 |
| 5.5x2.1mm female barrel jack (panel mount)                                     | https://amazon.com/dp/B07Y8MFCJD                 |

### Misc components

| Component                 | Qty |
| ------------------------- | --- |
| IN5819 Schottky Diode     | 2   |
| 7805 regulator + heatsink | 1   |
| 1K resistor               | 1   |
| NPN transistor (2N3904)   | 1   |
| Protoboard/breadboard     | -   |
| 0.1" male/female headers  | -   |
| Dupont leads              | -   |

## Circuit
- Display and RTC share I2C bus to ESP32 standard SDA/SCL (21 & 22, respectively) pins, very straightforward.  Display VIO and VCC are both wired to 3.3V rail. RTC is wired to 5V rail.
- The power/battery failover circuit uses an ideal diode setup. 2 IN5819 Schottky diodes with the cathodes both going to the 7805 input, and the anodes going to the battery pack positive lead and the 12V barrel jack center pin respectively.  This way whichever voltage is higher "wins", and the diodes prevent backflow. The 7805 output is the 5V rail.  Battery charge lead (female barrel jack) is wired directly to the enclosure's barrel jack.
- The piezo buzzer is driven via a 2N3904 transistor to supply 5V driven from 3.3V GPIO. 1K resistor from GPIO to base, emitter to ground, collector to buzzer ground lead, 5V rail to buzzer positive lead.

## Alarm sequence

The buzzer escalates through six phases, keyed on milliseconds elapsed since the alarm started:

| Elapsed   | Period     | On-time |
| --------- | ---------- | ------- |
| 0–30 s    | 10 s       | 250 ms  |
| 30–60 s   | 5 s        | 250 ms  |
| 1–2 min   | 1 s        | 200 ms  |
| 2–2.5 min | 500 ms     | 150 ms  |
| 2.5–3 min | 200 ms     | 150 ms  |
| 3 min+    | continuous | —       |

Dismissal stops the buzzer and marks today's alarm cancelled. The alarm will not re-fire until the next scheduled day.

## Control interfaces

### HTTP

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Web UI — schedule editor with live status |
| GET | `/status` | JSON status (time, state, next alarm, hw info) |
| POST | `/alarm?day=mon&h=6&m=30` | Set a day's alarm time |
| POST | `/alarm?day=sat&enabled=0` | Disable a day |
| POST | `/dismiss` | Dismiss or pre-empt; body must contain the dismiss token |
| POST | `/alarm/ui` | Form handler for the web UI schedule save |

The dismiss token is set at build time via `CONF_DISMISS_TOKEN` (see Configuration). The request body is scanned for the token string; a simple `{"token":"<value>"}` payload works.

### MQTT (optional)

MQTT is disabled by default (`MQTT_ENABLED = false` in `config.h`). When enabled it connects to the configured broker and subscribes to two topic patterns:

| Topic | Payload | Effect |
|-------|---------|--------|
| `alarm/dismiss` | any | dismiss / cancel today |
| `alarm/set/<day>` | `HH:MM` | set alarm for that day |
| `alarm/set/<day>` | `off` | disable that day |

Day names are lowercase three-letter abbreviations: `sun`, `mon`, `tue`, `wed`, `thu`, `fri`, `sat`.

## Schedule persistence

The per-day schedule is stored in the ESP32's NVS (non-volatile storage) via the `Preferences` library under the namespace `schedule`. On first boot the default is 06:30 Mon–Fri, disabled Sat–Sun. Changes via HTTP or MQTT are written immediately.

## Time keeping

The DS3231 RTC is the primary time source and is set to local time. On boot and daily at `NTP_SYNC_HOUR` (default 04:00), the device syncs with the configured NTP servers and writes the result back to the RTC.

If the RTC is absent or loses power, the device falls back to the ESP32's internal `getLocalTime()` after a successful NTP sync. If neither source is available the display shows dashes.

If booting after today's alarm time has already passed, the alarm is automatically skipped for that day.

I realized after I finished the project that between the battery failover and the daily NTP sync, the RTC was probably wholly unnecessary.  It will never lose power, and NTP keeps the system clock accurate.  

## Display indicators

- Colon: always on while time is displayed
- Second decimal point (position 2): PM indicator
- First decimal point (position 2, bit 2): alarm-armed indicator — lit when today's alarm is still pending, or when tomorrow has an alarm scheduled

## Building and flashing

The project uses [PlatformIO](https://platformio.org/) targeting `espressif32 / esp32dev`.

```bash
# Initial flash via USB
pio run --target upload

# Subsequent updates over WiFi (OTA)
pio run --target upload   # uses upload_protocol = espota
```

OTA upload address and auth password are set in `platformio.ini`. Change `upload_port` to match your device's IP.

## Configuration

Copy `secrets.ini.example` to `secrets.ini` and fill in your values. This file is loaded by PlatformIO and injected as build-time `#define` macros.

```ini
[secrets]
build_flags =
  -DCONF_WIFI_SSID=\"your-ssid\"
  -DCONF_WIFI_PASSWORD=\"your-wifi-password\"
  -DCONF_OTA_HOSTNAME=\"alarm-clock\"
  -DCONF_OTA_PASSWORD=\"your-ota-password\"
  -DCONF_TZ_STRING=\"America/Chicago\"
  -DCONF_NTP_SERVER1=\"pool.ntp.org\"
  -DCONF_NTP_SERVER2=\"time.nist.gov\"
  -DCONF_MQTT_BROKER=\"192.168.1.x\"
  -DCONF_MQTT_PORT=1883
  -DCONF_MQTT_CLIENT_ID=\"alarm-clock\"
  -DCONF_DISMISS_TOKEN=\"change-me\"
```

`TZ_STRING` must be a POSIX timezone string (e.g. `CST6CDT,M3.2.0,M11.1.0`), not an IANA name.

Additional compile-time options in `config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `MQTT_ENABLED` | `false` | Enable MQTT client |
| `SHOW_DISMISS_ON_WEB` | `false` | Show dismiss button in the web UI |
| `NTP_SYNC_HOUR` | `4` | Hour of day for daily NTP re-sync |
| `DISPLAY_BRIGHTNESS` | `4` | HT16K33 brightness (0–15) |
| `BUZZER_ACTIVE_LOW` | `false` | Invert buzzer GPIO polarity |

`secrets.ini` is gitignored. Do not commit it.

## Dependencies

Managed by PlatformIO:

- `adafruit/Adafruit LED Backpack Library`
- `adafruit/Adafruit GFX Library`
- `adafruit/RTClib`
- `knolleary/PubSubClient`
