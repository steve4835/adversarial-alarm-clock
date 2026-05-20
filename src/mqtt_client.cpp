#include "globals.h"
#include "mqtt_client.h"
#include "schedule.h"
#include "alarm.h"
#include "config.h"
#include <WiFi.h>

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
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
    resetTodayCancelledIfSafe();
    logNextAlarm();
  }
}

void setupMqtt() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqttMaintain();
}

static void mqttConnect() {
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

void mqttMaintain() {
  if (!MQTT_ENABLED || WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) mqttConnect();
  mqtt.loop();
}
