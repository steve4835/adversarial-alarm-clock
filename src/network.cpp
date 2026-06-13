#include "globals.h"
#include "network.h"
#include "display.h"
#include "config.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>

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

bool syncNtp() {
  if (WiFi.status() != WL_CONNECTED) return false;
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
      // Store UTC so the RTC survives DST transitions without discontinuity
      rtc.adjust(DateTime((uint32_t)time(nullptr)));
      Serial.printf("RTC updated (UTC): %02d:%02d:%02d local\n",
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      // Mark RTC as UTC so subsequent boots can trust the seed
      prefs.begin("sys", false);
      prefs.putBool("rtc_utc", true);
      prefs.end();
    }
    ntpSynced     = true;
    rtcPowerLost  = false;
    lastNtpSync   = millis();
    if (firstSyncTime[0] == '\0') {
      strftime(firstSyncTime, sizeof(firstSyncTime), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    }
    return true;
  } else {
    Serial.println("NTP sync failed — using RTC.");
    return false;
  }
}

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
