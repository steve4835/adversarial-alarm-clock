# Adversarial Alarm Clock

An alarm clock for people who are not hard to wake up — just hard to get out of bed.

Built on an ESP32. No snooze button. No mercy.

---

## Philosophy

Most alarm clocks fail because dismissing them requires no effort. Roll over, tap a button, go back to sleep. This clock is designed so that the only way to silence it is to get up and go somewhere else. By the time you've done that, you're up.

- **No snooze.** Ever.
- **Strobe light** fires 5 minutes before the buzzer, so you can't claim you didn't know it was coming.
- **Dismiss button is in the kitchen.** Or anywhere that isn't your bed.
- **NFC tag** on the coffee maker as a secondary dismiss method — tap your phone to it, alarm stops, coffee starts. You're already there.
- **Escalating buzzer** — slow beeps become rapid screech over 3 minutes if you somehow ignore everything else.

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| MCU | ESP32-WROOM-32 DevKit | DOIT DevKit V1 recommended |
| Display | Adafruit 1.2" 4-digit 7-segment w/ HT16K33 backpack | I2C, addr 0x70 |
| RTC | DS3231 module | I2C, addr 0x68, shared bus with display |
| Strobe | 12VDC xenon strobe beacon, 60 FPM | Switched via relay |
| Relay | Opto-isolated relay module, 5V coil | Controls strobe power |
| Buzzer | Passive piezo buzzer | Driven via `tone()`, GPIO 18 |
| Power | 3S LiPo + 12V BMS + buck converter (12V→5V) | 12V rail for strobe, 5V for ESP32 |
| Power input | Panel-mount 5.5mm barrel jack | 12VDC in |
| Enclosure | 230×150×85mm ABS project box | |
| Dismiss button | Panel-mount momentary switch | Mounted in kitchen, long cable |
| NFC tag | NTAG215 sticker | On coffee maker, dismiss via iOS Shortcuts |

### Wiring

```
ESP32 GPIO  │ Connected to
────────────┼─────────────────────────────────────────
4           │ DS3231 INT/SQW (active low, INPUT_PULLUP)
5           │ Relay module IN (HIGH = strobe on)
18          │ Piezo buzzer (+)
21 (SDA)    │ HT16K33 SDA + DS3231 SDA
22 (SCL)    │ HT16K33 SCL + DS3231 SCL
3.3V        │ HT16K33 VCC, DS3231 VCC
GND         │ HT16K33 GND, DS3231 GND, buzzer (-)

12V rail    │ Strobe (+), relay COM/NO
5V rail     │ ESP32 VIN, relay coil VCC
```

> **Note:** The DS3231 INT/SQW pin is used as a hardware alarm interrupt, not a square wave output. `DS3231_OFF` mode is set in firmware to enable this.

---

## Alarm Sequence

```
T - 5 min   DS3231 hardware interrupt fires
            → Strobe relay switches ON
            → DS3231 re-armed for alarm time

T + 0 min   DS3231 hardware interrupt fires again
            → Buzzer starts (slow beeps, 880Hz)

T + 1 min   Buzzer cadence increases (1200Hz, faster)
T + 2 min   Rapid beeps (1800Hz)
T + 3 min   Continuous screech (2400Hz, no gaps)

Dismiss     Buzzer off, strobe off
            DS3231 re-armed for next scheduled alarm day
```

---

## Dismiss / Pre-empt

The same endpoint handles both cases correctly:

- **Before alarm fires** — cancels today's alarm, re-arms for the next scheduled day
- **During alarm** — silences buzzer and strobe immediately, re-arms for next day

| Method | Command |
|---|---|
| Kitchen button | Press the wired momentary switch |
| HTTP | `curl -X POST http://<clock-ip>/dismiss` |
| MQTT | `mosquitto_pub -h <broker-ip> -t alarm/dismiss -m 1` |
| NFC | Tap phone to tag → iOS Shortcut POSTs to `/dismiss` |

---

## Per-Day Schedule

Each day of the week has its own alarm time and enabled state. Defaults to Mon–Fri 06:30, weekends off.

### Web UI

Browse to `http://<clock-ip>/` — a simple table with time pickers and checkboxes for each day. Mobile-friendly.

### HTTP API

```bash
# Set Monday to 06:30
curl -X POST "http://<clock-ip>/alarm?day=mon&h=6&m=30"

# Disable Saturday
curl -X POST "http://<clock-ip>/alarm?day=sat&enabled=0"

# Status
curl http://<clock-ip>/status
```

### MQTT

```bash
# Set Wednesday to 07:00
mosquitto_pub -h <broker-ip> -t alarm/set/wed -m "07:00"

# Disable Sunday
mosquitto_pub -h <broker-ip> -t alarm/set/sun -m "off"

# Dismiss
mosquitto_pub -h <broker-ip> -t alarm/dismiss -m 1
```

MQTT topics:

| Topic | Payload | Action |
|---|---|---|
| `alarm/dismiss` | any | Dismiss or pre-empt today |
| `alarm/set/<day>` | `HH:MM` | Set alarm time for day, enable it |
| `alarm/set/<day>` | `off` | Disable alarm for that day |

Days: `sun` `mon` `tue` `wed` `thu` `fri` `sat`

Schedule is persisted to NVS — survives reboots and power loss.

---

## Time Sync

- **NTP** sync on boot via `pool.ntp.org` and `time.nist.gov`
- **DS3231 RTC** updated from NTP on boot and every 6 hours
- If WiFi is unavailable, DS3231 holds time independently (coin cell on module)
- Timezone configured via POSIX TZ string — DST handled automatically

---

## iOS NFC Dismiss Setup

1. Stick an NTAG215 sticker on the coffee maker (or wherever you want the dismiss point)
2. On iPhone: **Shortcuts → Automation → New Automation → NFC**
3. Scan the tag, name it
4. Add action: **Get Contents of URL** → Method: POST → URL: `http://<clock-ip>/dismiss`
5. Disable "Ask Before Running" and "Notify When Run"

Tapping the phone to the sticker silently POSTs to the clock. No app needed.

---

## Setup & Flashing

### Prerequisites

- [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- Mosquitto MQTT broker on your network:
  ```bash
  sudo apt install mosquitto mosquitto-clients
  sudo systemctl enable --now mosquitto
  ```

### Configuration

Edit the top of `src/main.cpp`:

```cpp
const char* WIFI_SSID      = "YOUR_SSID";
const char* WIFI_PASSWORD  = "YOUR_PASSWORD";
const char* OTA_PASSWORD   = "YOUR_OTA_PASSWORD";
const char* MQTT_BROKER    = "192.168.1.x";      // your broker's IP
const char* TZ_STRING      = "CST6CDT,M3.2.0,M11.1.0"; // your timezone
```

Common timezone strings:

| Location | String |
|---|---|
| US Central (CST/CDT) | `CST6CDT,M3.2.0,M11.1.0` |
| US Eastern (EST/EDT) | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific (PST/PDT) | `PST8PDT,M3.2.0,M11.1.0` |
| UTC | `UTC0` |

### First Flash (USB)

```bash
# In PlatformIO terminal, or click Upload in VS Code
pio run --target upload

# Open serial monitor to see IP address and sync status
pio device monitor
```

If you get permission denied on `/dev/ttyUSB0`:
```bash
sudo usermod -aG uucp $USER   # or dialout, check: ls -la /dev/ttyUSB0
# log out and back in
```

### OTA (All Subsequent Flashes)

Once running, note the IP from the serial monitor. Edit `platformio.ini`:

```ini
; Comment out default USB upload, uncomment OTA:
upload_protocol = espota
upload_port     = 192.168.x.x
upload_flags    = --auth=YOUR_OTA_PASSWORD
```

Then flash normally — PlatformIO uploads over WiFi.

---

## Project Structure

```
adversarial-alarm-clock/
├── platformio.ini
├── README.md
└── src/
    └── main.cpp
```

---

## Dependencies

Managed automatically by PlatformIO via `platformio.ini`:

| Library | Purpose |
|---|---|
| `adafruit/Adafruit LED Backpack Library` | HT16K33 display driver |
| `adafruit/Adafruit GFX Library` | Required by LED Backpack |
| `adafruit/RTClib` | DS3231 RTC driver |
| `knolleary/PubSubClient` | MQTT client |

---

## Power Backup

A 3S LiPo with a 12V BMS provides hours of backup power. This is not designed to survive multi-day outages — it's designed to survive someone yanking the power cord in a half-asleep attempt to get more sleep. Power is supplied via a panel-mount barrel jack. Unplugging it just switches to LiPo — the clock keeps going. The enclosure uses security Torx screws. Good luck.

---

## License

MIT