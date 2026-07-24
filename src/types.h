#pragma once

#include <stdint.h>

// Keep-awake duration after a day's alarm is dismissed. Chirps always happen
// on a fixed 15-min grid; the mode controls how long that grid runs for.
enum KeepAwakeMode : uint8_t { KA_OFF = 0, KA_15MIN = 1, KA_2HR = 2 };

struct DayAlarm {
  uint8_t       hour;
  uint8_t       minute;
  bool          enabled;
  KeepAwakeMode keepAwake;
};

enum AlarmState { IDLE, ALARM };
