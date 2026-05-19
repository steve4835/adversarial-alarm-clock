#pragma once

#include "types.h"

// Indexed 0=Sun … 6=Sat, matching struct tm .tm_wday and DateTime .dayOfTheWeek()
extern DayAlarm    schedule[7];
extern const char* DAY_KEYS[7];

void loadSchedule();
void saveDay(int d);
int  dayIndex(const char* name);

// Find the next enabled alarm from a given point.
// skipToday forces the search to start at offset 1 (use when todayCancelled).
bool nextAlarm(int fromWday, int fromHour, int fromMin, bool skipToday,
               int& outDay, uint8_t& outH, uint8_t& outM);

void logNextAlarm();
