/*
 * Adversarial Alarm Clock
 * ============================================================
 * Hardware:
 *   ESP32 DevKit
 *   Adafruit 1.2" HT16K33 4-digit 7-segment display (I2C 0x70)
 *   DS3231 RTC (I2C 0x68, stores local time)
 *   Active buzzer        → GPIO_BUZZER (digitalWrite, LOW = on)
 *   Opto-isolated relay  → GPIO_RELAY  (strobe, HIGH = on)
 *   Buck converter: 12V → 5V → ESP32 VIN
 *
 * Alarm sequence (software polling at 1 Hz):
 *   T−1min  strobe relay ON
 *   T+0min  buzzer starts (slow beeps → continuous over 3 min)
 *   Dismiss buzzer + strobe off, today marked cancelled
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
 * Status:  http://<clock-ip>/status  (JSON, live on web UI)
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
// Configuration — set in secrets.ini (see secrets.ini.example)
// ============================================================
const char* WIFI_SSID      = CONF_WIFI_SSID;
const char* WIFI_PASSWORD  = CONF_WIFI_PASSWORD;
const char* OTA_HOSTNAME   = CONF_OTA_HOSTNAME;
const char* OTA_PASSWORD   = CONF_OTA_PASSWORD;

// POSIX timezone — US Central with auto DST
const char* TZ_STRING      = CONF_TZ_STRING;
const char* NTP_SERVER1    = CONF_NTP_SERVER1;
const char* NTP_SERVER2    = CONF_NTP_SERVER2;

// MQTT — point at your Mosquitto broker's IP
const char* MQTT_BROKER    = CONF_MQTT_BROKER;
const int   MQTT_PORT      = CONF_MQTT_PORT;
const char* MQTT_CLIENT_ID = CONF_MQTT_CLIENT_ID;
const bool MQTT_ENABLED    = false;

// Timing
const unsigned int NTP_SYNC_HOUR = 4; // 4am
const unsigned long MQTT_RECONNECT_MS = 5000;
const int           STROBE_LEAD_MIN   = 1;  // strobe on N minutes before buzzer

// Display
const uint8_t DISPLAY_BRIGHTNESS = 8; // 0–15

// ============================================================
// Pins
// ============================================================
#define GPIO_RELAY     5   // opto-isolated relay → strobe (HIGH = on)
#define GPIO_BUZZER   18   // active buzzer
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
unsigned long lastDisplayUpdate = 0;

// Alarm state machine
enum AlarmState { IDLE, STROBE_ONLY, FULL_ALARM };
AlarmState    alarmState      = IDLE;
bool          todayCancelled  = false;
unsigned long alarmStartedAt  = 0;  // millis() when buzzer began

bool          alarmArmed      = false;

// Buzzer escalation state — reset explicitly in startBuzzer()
unsigned long        buzzerLastToggle = 0;
bool                 buzzerToneOn     = false;
constexpr bool BUZZER_ACTIVE_LOW      = false; // LOW signal = buzzer on

// ============================================================
// Forward declarations (needed because setupOTA references
// display helpers defined later in the file)
// ============================================================
void showDashes();
void showOtaProgress(int pct);
void updateDisplay(int hour, int minute);
void startStrobe();

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
// skipToday forces offset to start at 1 (used when todayCancelled=true).
bool nextAlarm(int fromWday, int fromHour, int fromMin, bool skipToday,
               int& outDay, uint8_t& outH, uint8_t& outM) {
  for (int offset = 0; offset < 7; offset++) {
    int d = (fromWday + offset) % 7;
    if (!schedule[d].enabled) continue;

    if (offset == 0) {
      if (skipToday) continue;
      int alarmMins   = schedule[d].hour * 60 + schedule[d].minute;
      int currentMins = fromHour * 60 + fromMin;
      if (alarmMins < currentMins) continue;
    }

    outDay = d;
    outH   = schedule[d].hour;
    outM   = schedule[d].minute;
    return true;
  }
  return false;
}

// ============================================================
// Alarm logging
// ============================================================
void logNextAlarm() {
  if (!rtcAvailable) return;
  DateTime now = rtc.now();
  int outDay; uint8_t outH, outM;
  if (!nextAlarm(now.dayOfTheWeek(), now.hour(), now.minute(), todayCancelled,
                 outDay, outH, outM)) {
    Serial.println("No alarm days enabled.");
    return;
  }
  int sm = (int)outM - STROBE_LEAD_MIN, sh = (int)outH;
  if (sm < 0) { sm += 60; sh = (sh - 1 + 24) % 24; }
  Serial.printf("Next alarm: %s %02d:%02d (strobe at %02d:%02d)\n",
    DAY_KEYS[outDay], outH, outM, sh, sm);
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
  alarmState       = FULL_ALARM;
  alarmStartedAt   = millis();
  buzzerLastToggle = millis() - 1000; // ensure first beep fires on next loop tick
  buzzerToneOn     = false;
  Serial.println("Buzzer ON");
}

void dismiss() {
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(GPIO_RELAY, LOW);
  alarmState     = IDLE;
  todayCancelled = true;
  Serial.println("Alarm dismissed / cancelled for today.");
  logNextAlarm();
}

struct BuzzerPhase {
  unsigned long minElapsed; // ms
  unsigned long maxElapsed; // ms; 0xFFFFFFFFUL = open-ended
  unsigned long period;     // ms; 0 = continuous on
  unsigned long onTime;     // ms
};

// Each phase is active while alarmStartedAt elapsed is in [minElapsed, maxElapsed).
// period=0 means the buzzer stays on continuously for the rest of the alarm.
static const BuzzerPhase BUZZER_PHASES[] = {
  {      0,  60000, 1000, 100 }, // 0–1 min:  slow beeps
  {  60000, 120000,  400, 150 }, // 1–2 min:  faster beeps
  { 120000, 180000,  200, 100 }, // 2–3 min:  rapid beeps
  { 180000, 0xFFFFFFFFUL, 0, 0 }, // 3 min+: continuous
};

// Non-blocking buzzer escalation — call every loop iteration
// Active buzzer: fixed frequency, vary on/off cadence only
void handleBuzzerEscalation() {
  if (alarmState != FULL_ALARM) return;

  unsigned long elapsed = millis() - alarmStartedAt;
  unsigned long now     = millis();

  const BuzzerPhase* phase = nullptr;
  for (const auto& p : BUZZER_PHASES) {
    if (elapsed >= p.minElapsed && elapsed < p.maxElapsed) { phase = &p; break; }
  }
  if (!phase) return;

  if (phase->period == 0) {
    if (!buzzerToneOn) {
      digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
      buzzerToneOn = true;
    }
    return;
  }

  if (!buzzerToneOn && now - buzzerLastToggle >= (phase->period - phase->onTime)) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
    buzzerToneOn     = true;
    buzzerLastToggle = now;
  } else if (buzzerToneOn && now - buzzerLastToggle >= phase->onTime) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
    buzzerToneOn     = false;
    buzzerLastToggle = now;
  }
}

// ============================================================
// Display helpers
// ============================================================
void updateDisplay(int hour, int minute) {
  int h = hour % 12;
  if (h == 0) h = 12;
  if (h >= 10)
    display.writeDigitNum(0, h / 10, false);
  else
    display.writeDigitRaw(0, 0x00); // blank leading digit for single-digit hours
  display.writeDigitNum(1, h % 10,        false);
  uint8_t special = 0x02; // colon
  if (alarmArmed)         special |= 0x04; // alarm-armed dot
  if (hour >= 12)         special |= 0x08; // PM dot
  display.writeDigitRaw(2, special);
  display.writeDigitNum(3, minute / 10,   false);
  display.writeDigitNum(4, minute % 10,   false);
  display.writeDisplay();
}

void showDashes() {
  for (int i : {0, 1, 3, 4}) display.writeDigitRaw(i, 0x40); // segment G = dash
  display.drawColon(false);
  display.writeDisplay();
}

void showOtaProgress(int pct) {
  pct = pct > 99 ? 99 : (pct < 0 ? 0 : pct); // display only has 2 digits
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
<hr>
<div id='diag' style='font-size:0.85em;color:#555'>Loading status...</div>
<script>
function refresh(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('diag').innerHTML=
      'Time: <b>'+d.time+'</b> &nbsp; State: <b>'+d.state+'</b><br>'+
      'Cancelled today: <b>'+d.cancelled+'</b> &nbsp; RTC ok: <b>'+d.rtc+'</b><br>'+
      'Next: <b>'+d.next_day+' '+d.next_alarm+'</b> (strobe <b>'+d.next_strobe+'</b>)<br>'+
      'NTP sync: <b>'+d.ntp_sync+'</b> &nbsp; WiFi: '+d.wifi_rssi+' dBm &nbsp; <small>(refreshes every 1s)</small>';
  }).catch(()=>{ document.getElementById('diag').innerHTML='(status unavailable)'; });
}
refresh(); setInterval(refresh,1000);
</script>
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
    todayCancelled = false;
    logNextAlarm();
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
    if (!rtcAvailable || d != (int)rtc.now().dayOfTheWeek()) todayCancelled = false;
    logNextAlarm();
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
    char timeStr[9]   = "--:--:--";
    char nextDay[4]   = "---";
    char nextTime[6]  = "--:--";
    char strobeT[6]   = "--:--";

    if (rtcAvailable) {
      DateTime now = rtc.now();
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
      int outDay; uint8_t outH, outM;
      if (nextAlarm(now.dayOfTheWeek(), now.hour(), now.minute(), todayCancelled,
                    outDay, outH, outM)) {
        strncpy(nextDay, DAY_KEYS[outDay], sizeof(nextDay) - 1);
        snprintf(nextTime, sizeof(nextTime), "%02d:%02d", outH, outM);
        int sm = (int)outM - STROBE_LEAD_MIN;
        int sh = (int)outH;
        if (sm < 0) { sm += 60; sh = (sh - 1 + 24) % 24; }
        snprintf(strobeT, sizeof(strobeT), "%02d:%02d", sh, sm);
      }
    }

    char ntpSyncStr[32] = "never";
    if (lastNtpSync > 0) {
      unsigned long elapsed = millis() - lastNtpSync;
      unsigned long secs = elapsed / 1000;
      unsigned long mins = secs / 60;
      unsigned long hrs  = mins / 60;
      if (hrs > 0)
        snprintf(ntpSyncStr, sizeof(ntpSyncStr), "%luh %02lum ago", hrs, mins % 60);
      else if (mins > 0)
        snprintf(ntpSyncStr, sizeof(ntpSyncStr), "%lum ago", mins);
      else
        snprintf(ntpSyncStr, sizeof(ntpSyncStr), "%lus ago", secs);
    }

    char buf[448];
    snprintf(buf, sizeof(buf),
      "{\"time\":\"%s\",\"state\":\"%s\",\"cancelled\":%s,"
      "\"rtc\":%s,\"wifi_rssi\":%d,"
      "\"next_day\":\"%s\",\"next_alarm\":\"%s\",\"next_strobe\":\"%s\","
      "\"ntp_sync\":\"%s\"}",
      timeStr,
      alarmState == IDLE ? "idle" : alarmState == STROBE_ONLY ? "strobe" : "alarm",
      todayCancelled ? "true" : "false",
      rtcAvailable   ? "true" : "false",
      WiFi.RSSI(),
      nextDay, nextTime, strobeT,
      ntpSyncStr);
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
    if (!rtcAvailable || d != (int)rtc.now().dayOfTheWeek()) todayCancelled = false;
    logNextAlarm();
  }
}

void mqttConnect() {
  if (!MQTT_ENABLED) return;
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
      // Store local time so alarm hour comparisons stay consistent
      rtc.adjust(DateTime(
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
      Serial.printf("RTC updated (local): %02d:%02d:%02d\n",
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

  pinMode(GPIO_RELAY,   OUTPUT);
  pinMode(GPIO_BUZZER,  OUTPUT);
  digitalWrite(GPIO_RELAY,  LOW);
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);

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
    Serial.println("DS3231 not found — NTP only, no RTC fallback");
  }

  loadSchedule();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
    syncNtp();
    setupHttp();
    if (MQTT_ENABLED) setupMqtt();
  }

  logNextAlarm();
  Serial.println("Ready.");
}

// ============================================================
// Loop
// ============================================================
void loop() {
  ArduinoOTA.handle();
  httpServer.handleClient();

  if (MQTT_ENABLED && WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) mqttConnect();
    mqtt.loop();
  }

  unsigned long now = millis();

  // --- Buzzer tone escalation (non-blocking) ---
  handleBuzzerEscalation();

  // --- Display update at 1Hz ---
  if (now - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = now;
    if (rtcAvailable) {
      DateTime rtcNow = rtc.now();
      int todayIdx    = rtcNow.dayOfTheWeek();
      int tomorrowIdx = (todayIdx + 1) % 7;
      int curMins     = rtcNow.hour() * 60 + rtcNow.minute();
      int todayMins   = schedule[todayIdx].enabled
                        ? schedule[todayIdx].hour * 60 + schedule[todayIdx].minute : -1;

      bool alarmLaterToday = todayMins > curMins && !todayCancelled;
      bool alarmTomorrow   = schedule[tomorrowIdx].enabled;

      alarmArmed = alarmLaterToday || alarmTomorrow;
      updateDisplay(rtcNow.hour(), rtcNow.minute());

      // 1 Hz alarm check — fire strobe/buzzer when schedule matches
      if (!todayCancelled) {
        int today      = rtcNow.dayOfTheWeek();
        int curMins    = rtcNow.hour() * 60 + rtcNow.minute();
        int alarmMins  = schedule[today].enabled
                         ? schedule[today].hour * 60 + schedule[today].minute : -1;
        if (alarmMins >= 0) {
          int strobeMins = alarmMins - STROBE_LEAD_MIN;
          if (strobeMins < 0) strobeMins = 0;
          if (alarmState == IDLE && curMins >= strobeMins && curMins < alarmMins) {
            startStrobe();
          } else if (alarmState == STROBE_ONLY && curMins >= alarmMins) {
            startBuzzer();
          }
        }
      }
    } else if (ntpSynced) {
      struct tm ti;
      if (getLocalTime(&ti)) {
        alarmArmed = schedule[(ti.tm_wday + 1) % 7].enabled;
        updateDisplay(ti.tm_hour, ti.tm_min);
      }
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
  if (WiFi.status() == WL_CONNECTED && rtcAvailable) {
    DateTime rtcNow = rtc.now();
    if(rtcNow.hour() == NTP_SYNC_HOUR && rtcNow.minute() == 0 && rtcNow.second() < 2)
      syncNtp();
  }

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