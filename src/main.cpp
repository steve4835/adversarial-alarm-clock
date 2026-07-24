/*
 * Adversarial Alarm Clock
 * ============================================================
 * Hardware:
 *   ESP32 DevKit
 *   Adafruit 1.2" HT16K33 4-digit 7-segment display (I2C 0x70)
 *   DS3231 RTC (I2C 0x68, stores UTC)
 *   Active buzzer        → GPIO_BUZZER (digitalWrite, LOW = on)
 *   Buck converter: 12V → 5V → ESP32 VIN
 *
 * Alarm sequence (software polling at 1 Hz):
 *   T+0min  buzzer starts (slow beeps → continuous over 3 min)
 *   Dismiss: buzzer off, today marked cancelled
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

#include <Wire.h>
#include <ArduinoOTA.h>
#include <time.h>

#include "config.h"
#include "globals.h"
#include "schedule.h"
#include "alarm.h"
#include "keep_awake.h"
#include "display.h"
#include "network.h"
#include "http_server.h"
#include "mqtt_client.h"

// ============================================================
// Global definitions (declared extern in globals.h)
// ============================================================
RTC_DS3231  rtc;
Preferences prefs;
WebServer   httpServer(80);
WiFiClient  wifiClient;
PubSubClient mqtt(wifiClient);

bool          rtcAvailable      = false;
bool          ntpSynced         = false;
bool          rtcPowerLost      = false;
unsigned long lastNtpSync       = 0;
unsigned long lastMqttAttempt   = 0;
unsigned long lastDisplayUpdate = 0;
bool          alarmArmed        = false;
char          firstSyncTime[30] = "";

AlarmState alarmState     = IDLE;
bool       todayCancelled = false;

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Adversarial Alarm Clock ===");

  pinMode(GPIO_BUZZER, OUTPUT);
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);

  Wire.begin();
  setupDisplay();

  if (rtc.begin()) {
    rtcAvailable = true;
    Serial.println("DS3231 OK");
  } else {
    Serial.println("DS3231 not found — NTP only, no RTC fallback");
  }

  loadSchedule();

  // Set TZ unconditionally so getLocalTime() works for all subsequent calls
  setenv("TZ", TZ_STRING, 1);
  tzset();

  // Only seed from RTC once we know it holds UTC (written by syncNtp).
  // Before the first successful NTP sync the RTC may hold local time from
  // old firmware; seeding from it would double-apply the TZ offset and fool
  // the boot-time alarm guard into thinking it's already tomorrow.
  {
    prefs.begin("sys", true);
    bool rtcIsUtc = prefs.getBool("rtc_utc", false);
    prefs.end();
    if (rtcAvailable && !rtc.lostPower() && rtcIsUtc) {
      struct timeval tv = { .tv_sec = (time_t)rtc.now().unixtime(), .tv_usec = 0 };
      settimeofday(&tv, nullptr);
    }
  }

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
  mqttMaintain();

  unsigned long now = millis();

  // Non-blocking buzzer escalation
  handleBuzzerEscalation();
  handleKeepAwake();

  static int lastKnownHour = -1, lastKnownMinute = -1;

  // Display + alarm check at 1 Hz
  if (now - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = now;

    // Surface RTC power-loss and attempt recovery, but don't block on it
    if (rtcAvailable && rtc.lostPower()) {
      if (!rtcPowerLost) {
        Serial.println("WARNING: RTC lost power — attempting NTP sync");
        rtcPowerLost = true;
      }
      if (WiFi.status() == WL_CONNECTED) {
        ntpSynced = syncNtp();
        if (ntpSynced) rtcPowerLost = false;
      }
    }

    struct tm tm;
    if (getLocalTime(&tm)) {
      int todayIdx    = tm.tm_wday;
      int tomorrowIdx = (todayIdx + 1) % 7;
      int curMins     = tm.tm_hour * 60 + tm.tm_min;
      int alarmMins   = schedule[todayIdx].enabled
                        ? schedule[todayIdx].hour * 60 + schedule[todayIdx].minute : -1;

      // One-shot boot check: runs on the first valid time reading, regardless of
      // whether time came from RTC or NTP. Prevents firing a past alarm after reboot.
      static bool bootCheckDone = false;
      if (!bootCheckDone) {
        bootCheckDone = true;
        if (alarmMins >= 0 && curMins >= alarmMins) {
          todayCancelled = true;
          Serial.println("Boot after alarm time — skipping today's alarm.");
        }
      }

      alarmArmed      = (alarmMins > curMins && !todayCancelled) || schedule[tomorrowIdx].enabled;
      lastKnownHour   = tm.tm_hour;
      lastKnownMinute = tm.tm_min;
      updateDisplay(tm.tm_hour, tm.tm_min);

      static int  lastDay        = -1;
      static bool ntpSyncedToday = false;
      if (lastDay != -1 && tm.tm_mday != lastDay) {
        todayCancelled = false;
        ntpSyncedToday = false;
        Serial.println("Midnight rollover — todayCancelled reset.");
      }
      lastDay = tm.tm_mday;

      if (!todayCancelled) {
        if (alarmMins >= 0 && alarmState == IDLE && curMins >= alarmMins)
          startBuzzer(todayIdx);
      }

      // Periodic NTP re-sync at true wall-clock hour (DST-correct)
      if (WiFi.status() == WL_CONNECTED &&
          tm.tm_hour == (int)NTP_SYNC_HOUR && !ntpSyncedToday) {
        ntpSyncedToday = syncNtp();
      }
    } else {
      showDashes();
    }
  }

  // Faster refresh so the alarm/keep-awake indicator dot can blink at ~1 Hz
  // (the 1 Hz block above is too coarse to show a sub-second flash).
  static unsigned long lastIndicatorUpdate = 0;
  if (lastKnownHour >= 0 && now - lastIndicatorUpdate >= 250) {
    lastIndicatorUpdate = now;
    updateDisplay(lastKnownHour, lastKnownMinute);
  }

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect > 30000) {
      lastReconnect = now;
      Serial.println("WiFi dropped — reconnecting...");
      WiFi.reconnect();
    }
  }
}
