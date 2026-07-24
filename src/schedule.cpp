#include "globals.h"
#include "schedule.h"
#include <time.h>

const char* DAY_KEYS[] = { "sun","mon","tue","wed","thu","fri","sat" };
DayAlarm    schedule[7];

void loadSchedule() {
  prefs.begin("schedule", true); // read-only
  for (int d = 0; d < 7; d++) {
    char keyH[8], keyM[8], keyE[8], keyKA[8];
    snprintf(keyH,  sizeof(keyH),  "%sh",  DAY_KEYS[d]);
    snprintf(keyM,  sizeof(keyM),  "%sm",  DAY_KEYS[d]);
    snprintf(keyE,  sizeof(keyE),  "%se",  DAY_KEYS[d]);
    snprintf(keyKA, sizeof(keyKA), "%ska", DAY_KEYS[d]);
    schedule[d].hour      = prefs.getUChar(keyH, 6);
    schedule[d].minute    = prefs.getUChar(keyM, 30);
    schedule[d].enabled   = prefs.getBool(keyE, d >= 1 && d <= 5); // Mon–Fri default
    schedule[d].keepAwake = (KeepAwakeMode)prefs.getUChar(keyKA, KA_15MIN);
  }
  prefs.end();
}

void saveDay(int d) {
  prefs.begin("schedule", false); // read-write
  char keyH[8], keyM[8], keyE[8], keyKA[8];
  snprintf(keyH,  sizeof(keyH),  "%sh",  DAY_KEYS[d]);
  snprintf(keyM,  sizeof(keyM),  "%sm",  DAY_KEYS[d]);
  snprintf(keyE,  sizeof(keyE),  "%se",  DAY_KEYS[d]);
  snprintf(keyKA, sizeof(keyKA), "%ska", DAY_KEYS[d]);
  prefs.putUChar(keyH,  schedule[d].hour);
  prefs.putUChar(keyM,  schedule[d].minute);
  prefs.putBool(keyE,   schedule[d].enabled);
  prefs.putUChar(keyKA, schedule[d].keepAwake);
  prefs.end();
}

int dayIndex(const char* name) {
  for (int i = 0; i < 7; i++) {
    if (strcasecmp(name, DAY_KEYS[i]) == 0) return i;
  }
  return -1;
}

bool keepAwakeModeFromString(const char* s, KeepAwakeMode& out) {
  if (strcasecmp(s, "off")   == 0) { out = KA_OFF;   return true; }
  if (strcasecmp(s, "15min") == 0) { out = KA_15MIN; return true; }
  if (strcasecmp(s, "2hr")   == 0) { out = KA_2HR;   return true; }
  return false;
}

const char* keepAwakeModeToString(KeepAwakeMode m) {
  switch (m) {
    case KA_OFF:   return "off";
    case KA_2HR:   return "2hr";
    case KA_15MIN:
    default:       return "15min";
  }
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

int alarmImminent(const struct tm& now) {
  int currentMins = now.tm_hour * 60 + now.tm_min;

  if (!todayCancelled && schedule[now.tm_wday].enabled) {
    int diff = schedule[now.tm_wday].hour * 60 + schedule[now.tm_wday].minute - currentMins;
    if (diff >= 0 && diff <= 60) return now.tm_wday;
  }

  int minsUntilMidnight = 1440 - currentMins;
  if (minsUntilMidnight < 60) {
    int tomorrow = (now.tm_wday + 1) % 7;
    if (schedule[tomorrow].enabled) {
      int alarmMins = schedule[tomorrow].hour * 60 + schedule[tomorrow].minute;
      if (minsUntilMidnight + alarmMins <= 60) return tomorrow;
    }
  }

  return -1;
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
