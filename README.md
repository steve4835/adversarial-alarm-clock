# Adversarial Alarm Clock

An ESP32-based alarm clock designed to be difficult to dismiss. The buzzer escalates over three minutes from slow intermittent beeps to continuous tone, and dismissal requires an authenticated HTTP or MQTT request rather than a physical button.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32 DevKit |
| Display | Adafruit 1.2" 4-digit 7-segment (HT16K33, I2C 0x70) |
| RTC | DS3231 (I2C 0x68, stores local time) |
| Buzzer | Active buzzer on GPIO 18 |
| Relay | Opto-isolated relay on GPIO 5 (strobe output) |
| Power | 12V input → buck converter → 5V → ESP32 VIN |

The relay output is intended to drive a lamp or other load as a strobe. It activates at the start of the alarm and stays on until dismissal.

## Alarm sequence

The buzzer escalates through six phases, keyed on milliseconds elapsed since the alarm started:

| Elapsed | Period | On-time | Strobe |
|---------|--------|---------|--------|
| 0–30 s | 10 s | 250 ms | yes |
| 30–60 s | 5 s | 250 ms | yes |
| 1–2 min | 1 s | 200 ms | yes |
| 2–2.5 min | 500 ms | 150 ms | yes |
| 2.5–3 min | 200 ms | 150 ms | yes |
| 3 min+ | continuous | — | yes |

Dismissal stops both the buzzer and relay and marks today's alarm cancelled. The alarm will not re-fire until the next scheduled day.

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
