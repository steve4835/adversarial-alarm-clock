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
 *   T+0min  buzzer starts (slow beeps → continuous over 3 min)
 *            strobe relay joins after first escalation phase
 *   Dismiss: buzzer + strobe off, today marked cancelled
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

bool          rtcAvailable    = false;
bool          ntpSynced       = false;
unsigned long lastNtpSync     = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastDisplayUpdate = 0;
bool          alarmArmed      = false;

AlarmState alarmState     = IDLE;
bool       todayCancelled = false;

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Adversarial Alarm Clock ===");

  pinMode(GPIO_RELAY,  OUTPUT);
  pinMode(GPIO_BUZZER, OUTPUT);
  digitalWrite(GPIO_RELAY,  LOW);
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

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
    syncNtp();    
    setupHttp();
    if (MQTT_ENABLED) setupMqtt();
  }

  // If booting after today's alarm time, don't fire immediately
  if (rtcAvailable) {
    DateTime now  = rtc.now();
    int today     = now.dayOfTheWeek();
    int curMins   = now.hour() * 60 + now.minute();
    int alarmMins = schedule[today].enabled
                    ? schedule[today].hour * 60 + schedule[today].minute : -1;
    if (alarmMins >= 0 && curMins >= alarmMins) {
      todayCancelled = true;
      Serial.println("Boot after alarm time — skipping today's alarm.");
    }
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

  // Display + alarm check at 1 Hz
  if (now - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = now;

    if(rtc.lostPower()) {
      if (ntpSynced) {
        ntpSynced = false;
        Serial.println("WARNING: RTC lost power — time wrong until NTP sync");
        syncNtp();
      }
    } else if (rtcAvailable) {
      DateTime rtcNow = rtc.now();
      int todayIdx    = rtcNow.dayOfTheWeek();
      int tomorrowIdx = (todayIdx + 1) % 7;
      int curMins     = rtcNow.hour() * 60 + rtcNow.minute();
      int todayMins   = schedule[todayIdx].enabled
                        ? schedule[todayIdx].hour * 60 + schedule[todayIdx].minute : -1;

      alarmArmed = (todayMins > curMins && !todayCancelled) || schedule[tomorrowIdx].enabled;
      updateDisplay(rtcNow.hour(), rtcNow.minute());

      // Midnight rollover: reuse rtcNow so no extra I2C read on every loop tick
      static int  lastDay        = -1;
      static bool ntpSyncedToday = false;
      if (lastDay != -1 && rtcNow.day() != (uint8_t)lastDay) {
        todayCancelled = false;
        ntpSyncedToday = false;
        Serial.println("Midnight rollover — todayCancelled reset.");
      }
      lastDay = rtcNow.day();

      if (!todayCancelled) {
        int alarmMins = schedule[todayIdx].enabled
                        ? schedule[todayIdx].hour * 60 + schedule[todayIdx].minute : -1;
        if (alarmMins >= 0 && alarmState == IDLE && curMins >= alarmMins)
          startBuzzer();
      }

      // Periodic NTP re-sync — flag prevents missing the narrow time window under load
      if (WiFi.status() == WL_CONNECTED &&
          rtcNow.hour() == NTP_SYNC_HOUR && !ntpSyncedToday) {
        syncNtp();
        ntpSyncedToday = true;
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
