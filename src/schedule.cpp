#include "globals.h"
#include "schedule.h"
#include <time.h>

const char* DAY_KEYS[] = { "sun","mon","tue","wed","thu","fri","sat" };
DayAlarm    schedule[7];

void loadSchedule() {
  prefs.begin("schedule", true); // read-only
  for (int d = 0; d < 7; d++) {
    char keyH[8], keyM[8], keyE[8];
    snprintf(keyH, sizeof(keyH), "%sh", DAY_KEYS[d]);
    snprintf(keyM, sizeof(keyM), "%sm", DAY_KEYS[d]);
    snprintf(keyE, sizeof(keyE), "%se", DAY_KEYS[d]);
    schedule[d].hour    = prefs.getUChar(keyH, 6);
    schedule[d].minute  = prefs.getUChar(keyM, 30);
    schedule[d].enabled = prefs.getBool(keyE, d >= 1 && d <= 5); // Mon–Fri default
  }
  prefs.end();
}

void saveDay(int d) {
  prefs.begin("schedule", false); // read-write
  char keyH[8], keyM[8], keyE[8];
  snprintf(keyH, sizeof(keyH), "%sh", DAY_KEYS[d]);
  snprintf(keyM, sizeof(keyM), "%sm", DAY_KEYS[d]);
  snprintf(keyE, sizeof(keyE), "%se", DAY_KEYS[d]);
  prefs.putUChar(keyH, schedule[d].hour);
  prefs.putUChar(keyM, schedule[d].minute);
  prefs.putBool(keyE,  schedule[d].enabled);
  prefs.end();
}

int dayIndex(const char* name) {
  for (int i = 0; i < 7; i++) {
    if (strcasecmp(name, DAY_KEYS[i]) == 0) return i;
  }
  return -1;
}

bool nextAlarm(int fromWday, int fromHour, int fromMin, bool skipToday,
               int& outDay, uint8_t& outH, uint8_t& outM) {
  for (int offset = 0; offset < 7; offset++) {
    int d = (fromWday + offset) % 7;
    if (!schedule[d].enabled) continue;

    if (offset == 0) {
      if (skipToday) continue;
      int alarmMins   = schedule[d].hour * 60 + schedule[d].minute;
      int currentMins = fromHour * 60 + fromMin;
      if (alarmMins < currentMins) continue;
    }

    outDay = d;
    outH   = schedule[d].hour;
    outM   = schedule[d].minute;
    return true;
  }
  return false;
}

void logNextAlarm() {
  struct tm tm;
  if (!getLocalTime(&tm)) return;
  int outDay; uint8_t outH, outM;
  if (!nextAlarm(tm.tm_wday, tm.tm_hour, tm.tm_min, todayCancelled,
                 outDay, outH, outM)) {
    Serial.println("No alarm days enabled.");
    return;
  }
  Serial.printf("Next alarm: %s %02d:%02d\n", DAY_KEYS[outDay], outH, outM);
}
