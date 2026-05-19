#pragma once

void setupMqtt();
void mqttMaintain(); // reconnect if needed + mqtt.loop(); call every loop iteration
