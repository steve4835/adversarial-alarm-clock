#pragma once

// WiFi / OTA
static const char* const WIFI_SSID     = CONF_WIFI_SSID;
static const char* const WIFI_PASSWORD = CONF_WIFI_PASSWORD;
static const char* const OTA_HOSTNAME  = CONF_OTA_HOSTNAME;
static const char* const OTA_PASSWORD  = CONF_OTA_PASSWORD;

// NTP / timezone
static const char* const TZ_STRING   = CONF_TZ_STRING;
static const char* const NTP_SERVER1 = CONF_NTP_SERVER1;
static const char* const NTP_SERVER2 = CONF_NTP_SERVER2;

// MQTT
static const char* const MQTT_BROKER    = CONF_MQTT_BROKER;
static const char* const MQTT_CLIENT_ID = CONF_MQTT_CLIENT_ID;
constexpr int            MQTT_PORT      = CONF_MQTT_PORT;
constexpr bool           MQTT_ENABLED   = false;

// Timing
constexpr unsigned int  NTP_SYNC_HOUR     = 4;
constexpr unsigned long MQTT_RECONNECT_MS = 5000;

// Display
constexpr uint8_t DISPLAY_I2C_ADDR   = 0x70;
constexpr uint8_t DISPLAY_BRIGHTNESS = 4; // 0–15

// GPIO
constexpr uint8_t GPIO_BUZZER = 25; // active buzzer

// I2C (RTC and Display)
// For my own sanity because I forget every time
// SDA: GPIO pin 21
// SCL: GPIO pin 22

// LOW signal = buzzer on when true
constexpr bool BUZZER_ACTIVE_LOW = false;

constexpr bool SHOW_DISMISS_ON_WEB = false;

// Dismiss token — must match the "token" field in the POST /dismiss body
static const char* const DISMISS_TOKEN = CONF_DISMISS_TOKEN;

// Keep-awake — periodic chirps after the main alarm is dismissed, to catch
// falling back asleep. Fixed 15-min grid anchored to when it started.
constexpr unsigned long KEEP_AWAKE_DURATION_MS      = 2UL * 60 * 60 * 1000;
constexpr unsigned long KEEP_AWAKE_INTERVAL_MS      = 15UL * 60 * 1000;
constexpr unsigned long KEEP_AWAKE_CHIRP_WINDOW_MS  = 90UL * 1000;
constexpr unsigned long KEEP_AWAKE_CHIRP_ON_MS       = 100;
constexpr unsigned long KEEP_AWAKE_CHIRP_PERIOD_MS   = 2000;
constexpr unsigned long KEEP_AWAKE_DISMISS_WINDOW_MS = 5UL * 60 * 1000;
