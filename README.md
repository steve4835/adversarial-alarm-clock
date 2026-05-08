# Adversarial Alarm Clock — Stage 1

ESP32 + Adafruit 1.2" HT16K33 7-segment display + DS3231 RTC + NTP + OTA

## Wiring

| ESP32 GPIO | Connected to |
|------------|--------------|
| 21 (SDA)   | HT16K33 SDA + DS3231 SDA (shared I2C bus) |
| 22 (SCL)   | HT16K33 SCL + DS3231 SCL (shared I2C bus) |
| 2          | Onboard LED (status) |
| 3.3V       | HT16K33 VCC + DS3231 VCC |
| GND        | HT16K33 GND + DS3231 GND |

I2C addresses (no conflict):
- HT16K33 display: 0x70 (default)
- DS3231 RTC:      0x68 (fixed)

## Project structure

```
alarm_clock/
├── platformio.ini
├── README.md
└── src/
    └── main.cpp
```

## First flash (USB)

```bash
pio run --target upload
pio device monitor
```

## OTA flash (all subsequent updates)

1. Note the clock's IP from serial monitor on first boot
2. Edit `platformio.ini` — comment out USB section, uncomment OTA section, fill in IP
3. Upload normally

## Configuration

Edit the top of `src/main.cpp`:

| Setting | Description |
|---|---|
| `WIFI_SSID` | Your network name |
| `WIFI_PASSWORD` | Your network password |
| `OTA_PASSWORD` | OTA auth — change this |
| `TZ_STRING` | POSIX timezone string |
| `DISPLAY_BRIGHTNESS` | 0 (min) to 15 (max) |

### Common timezone strings
| Location | String |
|---|---|
| US Central (CST/CDT) | `CST6CDT,M3.2.0,M11.1.0` |
| US Eastern (EST/EDT) | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific (PST/PDT) | `PST8PDT,M3.2.0,M11.1.0` |
| UTC | `UTC0` |

## Coming in Stage 2

- Alarm time stored in NVS (survives reboot and power loss)
- Piezo buzzer with escalating tone
- 12V lamp via MOSFET + PWM (sunrise fade, 20min pre-alarm)
- Wired dismiss button (GPIO 4, kitchen-mounted)
- HTTP server for NFC dismiss (`POST /dismiss` via iOS Shortcuts)