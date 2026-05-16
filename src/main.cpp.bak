/*
 * Adversarial Alarm Clock — Stage 1
 * ESP32 + HT16K33 1.2" 4-digit display + DS3231 RTC + NTP + OTA
 *
 * Both display and RTC share I2C bus (GPIO 21/22)
 *   HT16K33 default address: 0x70
 *   DS3231  default address: 0x68
 *
 * PlatformIO lib_deps (see platformio.ini):
 *   adafruit/Adafruit LED Backpack Library
 *   adafruit/Adafruit GFX Library
 *   adafruit/RTClib
 */

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_LEDBackpack.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Configuration — edit these
// ---------------------------------------------------------------------------
const char* WIFI_SSID     = "UDM";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
const char* OTA_HOSTNAME  = "alarm-clock";
const char* OTA_PASSWORD  = "YOUR_OTA_PASSWORD";  // change this

// POSIX timezone string — US Central with automatic DST
const char* TZ_STRING = "CST6CDT,M3.2.0,M11.1.0";

// NTP servers
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";

// How often to re-sync NTP → DS3231
const unsigned long NTP_SYNC_INTERVAL = 6UL * 60 * 60 * 1000; // 6 hours

// Display brightness 0 (dim) – 15 (max)
const uint8_t DISPLAY_BRIGHTNESS = 8;

// ---------------------------------------------------------------------------
// Pin assignments
// ---------------------------------------------------------------------------
#define STATUS_LED  2   // onboard LED
// SDA = GPIO 21, SCL = GPIO 22 (Wire defaults, no explicit define needed)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Adafruit_7segment display = Adafruit_7segment();
RTC_DS3231        rtc;
Preferences       prefs;

bool          rtcAvailable   = false;
bool          ntpSynced      = false;
unsigned long lastNtpSync    = 0;

// Colon blink
bool          colonOn        = true;
unsigned long lastColonToggle = 0;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
void updateDisplay(int hour, int minute) {
  // Left two digits = hour, right two = minute
  // print() on Adafruit_7segment takes an integer and right-justifies it.
  // We manually place digits so leading zero on hour works correctly.

  display.writeDigitNum(0, hour / 10, false);
  display.writeDigitNum(1, hour % 10, false);
  display.drawColon(colonOn);
  display.writeDigitNum(3, minute / 10, false);
  display.writeDigitNum(4, minute % 10, false);
  display.writeDisplay();
}

void showDashes() {
  // "----" while waiting for time sync
  display.writeDigitRaw(0, 0x40); // segment G = dash
  display.writeDigitRaw(1, 0x40);
  display.writeDigitRaw(3, 0x40);
  display.writeDigitRaw(4, 0x40);
  display.drawColon(false);
  display.writeDisplay();
}

void showOtaProgress(int pct) {
  // Show 0–99 during OTA flash, right-justified
  display.writeDigitRaw(0, 0x00);
  display.writeDigitRaw(1, 0x00);
  display.drawColon(false);
  display.writeDigitNum(3, pct / 10, false);
  display.writeDigitNum(4, pct % 10, false);
  display.writeDisplay();
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
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

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi failed — running on RTC only");
  }
}

// ---------------------------------------------------------------------------
// NTP → DS3231 sync
// ---------------------------------------------------------------------------
void syncNtpToRtc() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Syncing NTP...");
  configTzTime(TZ_STRING, NTP_SERVER1, NTP_SERVER2);

  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo, 1000) && retries < 10) {
    retries++;
    Serial.print(".");
  }
  Serial.println();

  if (retries < 10) {
    if (rtcAvailable) {
      time_t now = time(nullptr);
      rtc.adjust(DateTime(now));
      Serial.printf("RTC updated: %02d:%02d:%02d\n",
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    ntpSynced     = true;
    lastNtpSync   = millis();
  } else {
    Serial.println("NTP sync failed — using RTC");
  }
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: Starting...");
    showDashes();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("OTA: Done. Rebooting.");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = progress / (total / 100);
    Serial.printf("OTA: %d%%\r", pct);
    showOtaProgress(pct);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)     Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.printf("OTA ready — hostname: %s\n", OTA_HOSTNAME);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n=== Adversarial Alarm Clock — Stage 1 ===");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  // I2C + display
  Wire.begin();
  display.begin(0x70);
  display.setBrightness(DISPLAY_BRIGHTNESS);
  showDashes(); // "----" until we have time

  // RTC
  if (rtc.begin()) {
    rtcAvailable = true;
    Serial.println("DS3231 found");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power — waiting for NTP sync");
    }
  } else {
    Serial.println("DS3231 not found — will use NTP system time only");
  }

  // WiFi + OTA + NTP
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
    syncNtpToRtc();
  }

  Serial.println("Setup complete.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  ArduinoOTA.handle();

  unsigned long now = millis();

  // Update display at 1Hz (toggle colon every 500ms)
  if (now - lastColonToggle >= 500) {
    colonOn        = !colonOn;
    lastColonToggle = now;

    if (rtcAvailable) {
      DateTime t = rtc.now();
      updateDisplay(t.hour(), t.minute());
    } else if (ntpSynced) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        updateDisplay(timeinfo.tm_hour, timeinfo.tm_min);
      }
    } else {
      showDashes();
    }
  }

  // Periodic NTP re-sync
  if (WiFi.status() == WL_CONNECTED &&
      now - lastNtpSync > NTP_SYNC_INTERVAL) {
    syncNtpToRtc();
  }

  // WiFi reconnect watchdog
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (now - lastReconnectAttempt > 30000) {
      lastReconnectAttempt = now;
      Serial.println("WiFi dropped, reconnecting...");
      WiFi.reconnect();
    }
  }
}