#pragma once

#include "types.h"

// Indexed 0=Sun … 6=Sat, matching struct tm .tm_wday and DateTime .dayOfTheWeek()
extern DayAlarm    schedule[7];
extern const char* DAY_KEYS[7];

void loadSchedule();
void saveDay(int d);
int  dayIndex(const char* name);

// "off" / "15min" / "2hr" — used by both the API and the web UI form.
bool        keepAwakeModeFromString(const char* s, KeepAwakeMode& out);
const char* keepAwakeModeToString(KeepAwakeMode m);

// Find the next enabled alarm from a given point.
// skipToday forces the search to start at offset 1 (use when todayCancelled).
bool nextAlarm(int fromWday, int fromHour, int fromMin, bool skipToday,
               int& outDay, uint8_t& outH, uint8_t& outM);

void logNextAlarm();

// Returns the locked day index (0-6) if an alarm fires within 60 minutes, or -1 if none.
int alarmImminent(const struct tm& now);
