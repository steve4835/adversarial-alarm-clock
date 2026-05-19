#pragma once

#include <stdint.h>

struct DayAlarm {
  uint8_t hour;
  uint8_t minute;
  bool    enabled;
};

enum AlarmState { IDLE, ALARM };
