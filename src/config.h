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
constexpr uint8_t DISPLAY_BRIGHTNESS = 5; // 0–15

// GPIO
constexpr uint8_t GPIO_RELAY  = 5;  // opto-isolated relay (HIGH = on)
constexpr uint8_t GPIO_BUZZER = 18; // active buzzer

// LOW signal = buzzer on when true
constexpr bool BUZZER_ACTIVE_LOW = false;
