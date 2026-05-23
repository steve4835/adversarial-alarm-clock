#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <RTClib.h>
#include <Preferences.h>
#include "types.h"

// Hardware objects (defined in main.cpp)
extern RTC_DS3231  rtc;
extern Preferences prefs;
extern WebServer   httpServer;
extern WiFiClient  wifiClient;
extern PubSubClient mqtt;

// Connection state
extern bool          rtcAvailable;
extern bool          ntpSynced;
extern bool          rtcPowerLost;
extern unsigned long lastNtpSync;
extern unsigned long lastMqttAttempt;

// Display
extern unsigned long lastDisplayUpdate;
extern bool          alarmArmed;

// Alarm state
extern AlarmState alarmState;
extern bool       todayCancelled;
