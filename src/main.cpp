/*
 * Adversarial Alarm Clock
 * ============================================================
 * Hardware:
 *   ESP32 DevKit
 *   Adafruit 1.2" HT16K33 4-digit 7-segment display (I2C 0x70)
 *   DS3231 RTC (I2C 0x68)
 *   Piezo buzzer         → GPIO_BUZZER (PWM via tone())
 *   Opto-isolated relay  → GPIO_RELAY  (strobe on/off)
 *   DS3231 INT/SQW       → GPIO_RTC_INT (hardware alarm interrupt)
 *   Buck converter: 12V → 5V → ESP32 VIN
 *
 * Alarm sequence:
 *   T-5min  strobe relay ON   (first DS3231 interrupt)
 *   T+0min  buzzer starts     (second DS3231 interrupt)
 *   Dismiss buzzer + strobe off, re-arm DS3231 for next scheduled day
 *
 * Dismiss / pre-empt (same endpoint, correct behaviour either way):
 *   HTTP POST /dismiss
 *   MQTT      alarm/dismiss   (any payload)
 *
 * Set per-day alarm:
 *   HTTP POST /alarm?day=mon&h=6&m=30
 *   MQTT      alarm/set/mon   payload "06:30"
 *
 * Disable a day:
 *   HTTP POST /alarm?day=sat&enabled=0
 *   MQTT      alarm/set/sat   payload "off"
 *
 * Web UI:  http://<clock-ip>/
 * Status:  http://<clock-ip>/status
 *
 * MQTT broker: Mosquitto on your Linux box, port 1883
 * ============================================================
 */

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_LEDBackpack.h>
#include <Preferences.h>

// ============================================================
// Configuration — edit these
// ============================================================
const char* WIFI_SSID      = "YOUR_SSID";
const char* WIFI_PASSWORD  = "YOUR_PASSWORD";
const char* OTA_HOSTNAME   = "alarm-clock";
const char* OTA_PASSWORD   = "YOUR_OTA_PASSWORD";

// POSIX timezone — US Central with auto DST
const char* TZ_STRING      = "CST6CDT,M3.2.0,M11.1.0";
const char* NTP_SERVER1    = "pool.ntp.org";
const char* NTP_SERVER2    = "time.nist.gov";

// MQTT — point at your Mosquitto broker's IP
const char* MQTT_BROKER    = "192.168.1.x";   // <-- edit this
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "alarm-clock";

// Timing
const unsigned long NTP_SYNC_INTERVAL = 6UL * 60 * 60 * 1000; // 6 hours
const unsigned long MQTT_RECONNECT_MS = 5000;
const int           STROBE_LEAD_MIN   = 5;  // strobe on N minutes before buzzer

// Display
const uint8_t DISPLAY_BRIGHTNESS = 8; // 0–15

// ============================================================
// Pins
// ============================================================
#define GPIO_RTC_INT   4   // DS3231 INT/SQW, active low when alarm fires
#define GPIO_RELAY     5   // opto-isolated relay → strobe (HIGH = on)
#define GPIO_BUZZER   18   // piezo buzzer via tone()
// I2C: SDA=21, SCL=22 (Wire library defaults)

// ============================================================
// Day schedule
// ============================================================
struct DayAlarm {
  uint8_t hour;
  uint8_t minute;
  bool    enabled;
};

// Indexed 0=Sun … 6=Sat, matching struct tm .tm_wday and DateTime .dayOfTheWeek()
const char* DAY_KEYS[]  = { "sun","mon","tue","wed","thu","fri","sat" };
DayAlarm    schedule[7];

// ============================================================
// Globals
// ============================================================
Adafruit_7segment display = Adafruit_7segment();
RTC_DS3231        rtc;
Preferences       prefs;
WebServer         httpServer(80);
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);

bool          rtcAvailable    = false;
bool          ntpSynced       = false;
unsigned long lastNtpSync     = 0;
unsigned long lastMqttAttempt = 0;

// Display
bool          colonOn         = true;
unsigned long lastColonToggle = 0;

// Alarm state machine
enum AlarmState { IDLE, STROBE_ONLY, FULL_ALARM };
AlarmState    alarmState      = IDLE;
bool          todayCancelled  = false;
unsigned long alarmStartedAt  = 0;  // millis() when buzzer began

volatile bool rtcAlarmFired   = false;  // written in ISR only

// Buzzer escalation state — reset explicitly in startBuzzer()
unsigned long buzzerLastToggle = 0;
bool          buzzerToneOn     = false;

// ============================================================
// ISR — IRAM_ATTR required, flag only, no I2C here
// ============================================================
void IRAM_ATTR onRtcAlarm() {
  rtcAlarmFired = true;
}

// ============================================================
// Forward declarations (needed because setupOTA references
// display helpers defined later in the file)
// ============================================================
void showDashes();
void showOtaProgress(int pct);
void updateDisplay(int hour, int minute);

// ============================================================
// Schedule — NVS persistence
// ============================================================
void loadSchedule() {
  prefs.begin("schedule", true); // read-only
  for (int d = 0; d < 7; d++) {
    char keyH[8], keyM[8], keyE[8];
    snprintf(keyH, sizeof(keyH), "%sh", DAY_KEYS[d]);
    snprintf(keyM, sizeof(keyM), "%sm", DAY_KEYS[d]);
    snprintf(keyE, sizeof(keyE), "%se", DAY_KEYS[d]);
    schedule[d].hour    = prefs.getUChar(keyH, 6);
    schedule[d].minute  = prefs.getUChar(keyM, 30);
    schedule[d].enabled = prefs.getBool(keyE, d >= 1 && d <= 5); // Mon–Fri default
  }
  prefs.end();
}

void saveDay(int d) {
  prefs.begin("schedule", false); // read-write
  char keyH[8], keyM[8], keyE[8];
  snprintf(keyH, sizeof(keyH), "%sh", DAY_KEYS[d]);
  snprintf(keyM, sizeof(keyM), "%sm", DAY_KEYS[d]);
  snprintf(keyE, sizeof(keyE), "%se", DAY_KEYS[d]);
  prefs.putUChar(keyH, schedule[d].hour);
  prefs.putUChar(keyM, schedule[d].minute);
  prefs.putBool(keyE,  schedule[d].enabled);
  prefs.end();
}

int dayIndex(const char* name) {
  for (int i = 0; i < 7; i++) {
    if (strcasecmp(name, DAY_KEYS[i]) == 0) return i;
  }
  return -1;
}

// Find the next enabled alarm day/time from a given point.
// Checks today first (only if alarm is still sufficiently in the future),
// then the next 6 days — 7 offsets total, one full week.
bool nextAlarm(int fromWday, int fromHour, int fromMin,
               int& outDay, uint8_t& outH, uint8_t& outM) {
  for (int offset = 0; offset < 7; offset++) {  // FIX: was 8, only 7 needed
    int d = (fromWday + offset) % 7;
    if (!schedule[d].enabled) continue;

    if (offset == 0) {
      // Same day: skip if strobe should already have started
      int alarmMins   = schedule[d].hour * 60 + schedule[d].minute;
      int currentMins = fromHour * 60 + fromMin;
      if (alarmMins - STROBE_LEAD_MIN <= currentMins) continue;
    }

    outDay = d;
    outH   = schedule[d].hour;
    outM   = schedule[d].minute;
    return true;
  }
  return false;
}

// ============================================================
// DS3231 alarm arming
// ============================================================
void armNextAlarm() {
  if (!rtcAvailable) return;

  DateTime now = rtc.now();
  int     outDay;
  uint8_t outH, outM;

  if (!nextAlarm(now.dayOfTheWeek(), now.hour(), now.minute(),
                 outDay, outH, outM)) {
    Serial.println("No alarm days enabled — RTC not armed.");
    return;
  }

  // Arm for strobe time = alarm time minus lead minutes
  int strobeMin = (int)outM - STROBE_LEAD_MIN;
  int strobeH   = (int)outH;
  if (strobeMin < 0) {
    strobeMin += 60;
    strobeH    = (strobeH - 1 + 24) % 24;
  }

  rtc.clearAlarm(1);
  rtc.setAlarm1(
    DateTime(0, 0, 0, (uint8_t)strobeH, (uint8_t)strobeMin, 0),
    DS3231_A1_Hour   // fires daily at HH:MM:00
  );
  rtc.writeSqwPinMode(DS3231_OFF); // INT/SQW acts as interrupt, not square wave

  Serial.printf("Next alarm: %s %02d:%02d (strobe fires at %02d:%02d)\n",
    DAY_KEYS[outDay], outH, outM, strobeH, strobeMin);
}

// ============================================================
// Alarm state machine
// ============================================================
void startStrobe() {
  alarmState = STROBE_ONLY;
  digitalWrite(GPIO_RELAY, HIGH);
  Serial.println("Strobe ON");
}

void startBuzzer() {
  alarmState     = FULL_ALARM;
  alarmStartedAt = millis();
  // Reset escalation state so every alarm starts from the beginning
  buzzerLastToggle = millis();
  buzzerToneOn     = false;
  Serial.println("Buzzer ON");
}

void dismiss() {
  noTone(GPIO_BUZZER);
  digitalWrite(GPIO_RELAY, LOW);
  alarmState    = IDLE;
  todayCancelled = true;
  rtcAlarmFired  = false;
  if (rtcAvailable) rtc.clearAlarm(1);
  Serial.println("Alarm dismissed / cancelled for today.");
  armNextAlarm();
}

// Non-blocking buzzer escalation — call every loop iteration
void handleBuzzerEscalation() {
  if (alarmState != FULL_ALARM) return;

  unsigned long elapsed = millis() - alarmStartedAt;
  unsigned long now     = millis();

  // Stage parameters
  int           freq;
  unsigned long period, onTime;

  if (elapsed < 60000UL) {         // 0–1 min: slow, low
    freq   = 880;
    period = 1000;
    onTime = 200;
  } else if (elapsed < 120000UL) { // 1–2 min: faster, higher
    freq   = 1200;
    period = 500;
    onTime = 150;
  } else if (elapsed < 180000UL) { // 2–3 min: rapid
    freq   = 1800;
    period = 250;
    onTime = 100;
  } else {                          // 3 min+: continuous screech
    if (!buzzerToneOn) {
      tone(GPIO_BUZZER, 2400);
      buzzerToneOn = true;
    }
    return;
  }

  // Toggle buzzer on/off based on period
  if (!buzzerToneOn && now - buzzerLastToggle >= (period - onTime)) {
    tone(GPIO_BUZZER, freq);
    buzzerToneOn     = true;
    buzzerLastToggle = now;
  } else if (buzzerToneOn && now - buzzerLastToggle >= onTime) {
    noTone(GPIO_BUZZER);
    buzzerToneOn     = false;
    buzzerLastToggle = now;
  }
}

// ============================================================
// Display helpers
// ============================================================
void updateDisplay(int hour, int minute) {
  display.writeDigitNum(0, hour / 10,   false);
  display.writeDigitNum(1, hour % 10,   false);
  display.drawColon(colonOn);
  display.writeDigitNum(3, minute / 10, false);
  display.writeDigitNum(4, minute % 10, false);
  display.writeDisplay();
}

void showDashes() {
  for (int i : {0, 1, 3, 4}) display.writeDigitRaw(i, 0x40); // segment G = dash
  display.drawColon(false);
  display.writeDisplay();
}

void showOtaProgress(int pct) {
  // FIX: clamp to 0–99 to avoid display glitch at 100%
  pct = pct > 99 ? 99 : (pct < 0 ? 0 : pct);
  display.writeDigitRaw(0, 0x00);
  display.writeDigitRaw(1, 0x00);
  display.drawColon(false);
  display.writeDigitNum(3, pct / 10, false);
  display.writeDigitNum(4, pct % 10, false);
  display.writeDisplay();
}

// ============================================================
// HTTP — web UI + API
// ============================================================
void setupHttp() {

  // Root — simple web UI
  httpServer.on("/", HTTP_GET, []() {
    String html = R"(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Alarm Clock</title>
<style>
  body{font-family:monospace;max-width:480px;margin:2em auto;padding:1em}
  h1{font-size:1.2em}
  table{width:100%;border-collapse:collapse}
  td,th{padding:0.4em;border:1px solid #ccc;text-align:center}
  input[type=time]{font-size:1em}
  input[type=checkbox]{transform:scale(1.3)}
  button{padding:0.5em 1.5em;font-size:1em;margin-top:1em;cursor:pointer}
  .dismiss{background:#c00;color:#fff;border:none;border-radius:4px}
</style></head><body>
<h1>Adversarial Alarm Clock</h1>
<form method='POST' action='/alarm/ui'>
<table>
<tr><th>Day</th><th>Time</th><th>On</th></tr>)";

    const char* dayNames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    for (int d = 0; d < 7; d++) {
      html += "<tr><td>" + String(dayNames[d]) + "</td><td>";
      html += "<input type='time' name='t" + String(d) + "' value='";
      char t[6];
      snprintf(t, sizeof(t), "%02d:%02d", schedule[d].hour, schedule[d].minute);
      html += String(t) + "'></td><td>";
      html += "<input type='checkbox' name='e" + String(d) + "'";
      if (schedule[d].enabled) html += " checked";
      html += "></td></tr>";
    }
    html += R"(</table>
<button type='submit'>Save Schedule</button>
</form>
<br>
<form method='POST' action='/dismiss'>
<button class='dismiss' type='submit'>Dismiss / Cancel Today</button>
</form>
</body></html>)";
    httpServer.send(200, "text/html", html);
  });

  // Web UI form POST — saves all days at once
  httpServer.on("/alarm/ui", HTTP_POST, []() {
    for (int d = 0; d < 7; d++) {
      String tKey = "t" + String(d);
      String eKey = "e" + String(d);
      if (httpServer.hasArg(tKey)) {
        String t = httpServer.arg(tKey); // "HH:MM"
        if (t.length() >= 5 && t[2] == ':') {
          schedule[d].hour   = t.substring(0, 2).toInt();
          schedule[d].minute = t.substring(3, 5).toInt();
        }
        schedule[d].enabled = httpServer.hasArg(eKey);
        saveDay(d);
      }
    }
    armNextAlarm();
    httpServer.sendHeader("Location", "/");
    httpServer.send(303);
  });

  // API: POST /alarm?day=mon&h=6&m=30[&enabled=0]
  httpServer.on("/alarm", HTTP_POST, []() {
    if (!httpServer.hasArg("day")) {
      httpServer.send(400, "text/plain", "missing ?day=");
      return;
    }
    int d = dayIndex(httpServer.arg("day").c_str());
    if (d < 0) {
      httpServer.send(400, "text/plain", "unknown day — use sun/mon/tue/wed/thu/fri/sat");
      return;
    }
    if (httpServer.hasArg("h")) schedule[d].hour   = httpServer.arg("h").toInt();
    if (httpServer.hasArg("m")) schedule[d].minute = httpServer.arg("m").toInt();
    // enabled defaults to true unless explicitly passed as 0
    schedule[d].enabled = httpServer.hasArg("enabled")
                          ? (httpServer.arg("enabled").toInt() != 0)
                          : true;
    saveDay(d);
    armNextAlarm();
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %02d:%02d enabled=%d",
      DAY_KEYS[d], schedule[d].hour, schedule[d].minute, schedule[d].enabled);
    httpServer.send(200, "text/plain", buf);
  });

  // Dismiss / pre-empt — GET for browser testing, POST for production
  auto handleDismiss = []() {
    httpServer.send(200, "text/plain", "ok");
    dismiss();
  };
  httpServer.on("/dismiss", HTTP_POST, handleDismiss);
  httpServer.on("/dismiss", HTTP_GET,  handleDismiss);

  // Status JSON
  httpServer.on("/status", HTTP_GET, []() {
    char timeStr[6] = "--:--";
    if (rtcAvailable) {
      DateTime t = rtc.now();
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d", t.hour(), t.minute());
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"time\":\"%s\",\"state\":\"%s\",\"cancelled\":%s,\"wifi_rssi\":%d}",
      timeStr,
      alarmState == IDLE        ? "idle"   :
      alarmState == STROBE_ONLY ? "strobe" : "alarm",
      todayCancelled ? "true" : "false",
      WiFi.RSSI());
    httpServer.send(200, "application/json", buf);
  });

  httpServer.begin();
  Serial.printf("HTTP server: http://%s/\n", WiFi.localIP().toString().c_str());
}

// ============================================================
// MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64] = {};
  strncpy(msg, (char*)payload, min((unsigned int)63, length));
  Serial.printf("MQTT [%s] \"%s\"\n", topic, msg);

  if (strcmp(topic, "alarm/dismiss") == 0) {
    dismiss();
    return;
  }

  // alarm/set/<day>  — payload "HH:MM" or "off"
  if (strncmp(topic, "alarm/set/", 10) == 0) {
    int d = dayIndex(topic + 10);
    if (d < 0) return;

    if (strcasecmp(msg, "off") == 0) {
      schedule[d].enabled = false;
    } else if (strlen(msg) >= 5 && msg[2] == ':') {
      schedule[d].hour    = atoi(msg);
      schedule[d].minute  = atoi(msg + 3);
      schedule[d].enabled = true;
    } else {
      Serial.println("MQTT: unrecognised alarm/set payload — use HH:MM or off");
      return;
    }
    saveDay(d);
    armNextAlarm();
  }
}

void mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RECONNECT_MS) return;
  lastMqttAttempt = now;

  Serial.printf("MQTT connecting to %s:%d ...", MQTT_BROKER, MQTT_PORT);
  if (mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.println(" connected.");
    mqtt.subscribe("alarm/dismiss");
    mqtt.subscribe("alarm/set/+");
  } else {
    Serial.printf(" failed (rc=%d), retry in %lus\n",
      mqtt.state(), MQTT_RECONNECT_MS / 1000);
  }
}

void setupMqtt() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqttConnect();
}

// ============================================================
// WiFi
// ============================================================
void connectWiFi() {
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\nWiFi failed — running on RTC only.");
}

// ============================================================
// NTP → DS3231 sync
// ============================================================
void syncNtp() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Syncing NTP...");
  configTzTime(TZ_STRING, NTP_SERVER1, NTP_SERVER2);
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo, 1000) && retries < 10) {
    Serial.print(".");
    retries++;
  }
  Serial.println();
  if (retries < 10) {
    if (rtcAvailable) {
      rtc.adjust(DateTime(time(nullptr)));
      Serial.printf("RTC updated: %02d:%02d:%02d\n",
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    ntpSynced   = true;
    lastNtpSync = millis();
  } else {
    Serial.println("NTP sync failed — using RTC.");
  }
}

// ============================================================
// OTA
// ============================================================
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: starting...");
    showDashes();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // FIX: guard against division by zero if total < 100
    int pct = (total > 0) ? (int)((float)progress / total * 100) : 0;
    Serial.printf("OTA: %d%%\r", pct);
    showOtaProgress(pct);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: done, rebooting.");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("auth failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("begin failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("connect failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("receive failed");
    else if (error == OTA_END_ERROR)     Serial.println("end failed");
  });

  ArduinoOTA.begin();
  Serial.printf("OTA ready — hostname: %s\n", OTA_HOSTNAME);
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Adversarial Alarm Clock ===");

  pinMode(GPIO_RTC_INT, INPUT_PULLUP);
  pinMode(GPIO_RELAY,   OUTPUT);
  digitalWrite(GPIO_RELAY, LOW);

  Wire.begin();
  display.begin(0x70);
  display.setBrightness(DISPLAY_BRIGHTNESS);
  showDashes();

  if (rtc.begin()) {
    rtcAvailable = true;
    Serial.println("DS3231 OK");
    if (rtc.lostPower())
      Serial.println("WARNING: RTC lost power — time wrong until NTP sync");
  } else {
    Serial.println("DS3231 not found — NTP only, hardware alarm unavailable");
  }

  loadSchedule();

  // Attach interrupt before arming — avoids a race if alarm time is imminent
  attachInterrupt(digitalPinToInterrupt(GPIO_RTC_INT), onRtcAlarm, FALLING);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
    syncNtp();
    setupHttp();
    setupMqtt();
  }

  armNextAlarm();
  Serial.println("Ready.");
}

// ============================================================
// Loop
// ============================================================
void loop() {
  ArduinoOTA.handle();
  httpServer.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) mqttConnect();
    mqtt.loop();
  }

  unsigned long now = millis();

  // --- DS3231 hardware interrupt ---
  if (rtcAlarmFired) {
    rtcAlarmFired = false;
    if (rtcAvailable) rtc.clearAlarm(1); // release INT pin immediately

    if (!todayCancelled) {
      if (alarmState == IDLE) {
        // First interrupt: start strobe, re-arm DS3231 for buzzer time
        startStrobe();
        if (rtcAvailable) {
          DateTime t = rtc.now();
          int d = t.dayOfTheWeek();
          if (schedule[d].enabled) {
            rtc.clearAlarm(1);
            rtc.setAlarm1(
              DateTime(0, 0, 0, schedule[d].hour, schedule[d].minute, 0),
              DS3231_A1_Hour
            );
            rtc.writeSqwPinMode(DS3231_OFF);
            Serial.printf("DS3231 re-armed for buzzer at %02d:%02d\n",
              schedule[d].hour, schedule[d].minute);
          }
        }
      } else if (alarmState == STROBE_ONLY) {
        // Second interrupt: start buzzer
        startBuzzer();
      }
    } else {
      // Pre-empted — both interrupts skipped, arm for next day
      Serial.println("Alarm skipped (cancelled for today).");
      todayCancelled = false;
      armNextAlarm();
    }
  }

  // --- Buzzer tone escalation (non-blocking) ---
  handleBuzzerEscalation();

  // --- Display update at 1Hz (colon toggles every 500ms) ---
  if (now - lastColonToggle >= 500) {
    colonOn         = !colonOn;
    lastColonToggle = now;
    if (rtcAvailable) {
      DateTime t = rtc.now();
      updateDisplay(t.hour(), t.minute());
    } else if (ntpSynced) {
      struct tm ti;
      if (getLocalTime(&ti)) updateDisplay(ti.tm_hour, ti.tm_min);
    } else {
      showDashes();
    }
  }

  // --- Reset todayCancelled at midnight ---
  if (rtcAvailable) {
    static int lastDay = -1;
    int today = rtc.now().day();
    if (lastDay != -1 && today != lastDay) {
      todayCancelled = false;
      Serial.println("Midnight rollover — todayCancelled reset.");
    }
    lastDay = today;
  }

  // --- Periodic NTP re-sync ---
  if (WiFi.status() == WL_CONNECTED && now - lastNtpSync > NTP_SYNC_INTERVAL)
    syncNtp();

  // --- WiFi watchdog ---
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect > 30000) {
      lastReconnect = now;
      Serial.println("WiFi dropped — reconnecting...");
      WiFi.reconnect();
    }
  }
}