#include "globals.h"
#include "alarm.h"
#include "config.h"
#include "schedule.h"
#include "keep_awake.h"
#include <time.h>

// Module-private state; not exposed through globals
static unsigned long alarmStartedAt  = 0;
static unsigned long buzzerLastToggle = 0;
static bool          buzzerToneOn     = false;

struct AlarmPhase {
  unsigned long minElapsed; // ms
  unsigned long maxElapsed; // ms; 0xFFFFFFFFUL = open-ended
  unsigned long buzzerPeriod;     // ms; 0 = continuous on
  unsigned long buzzerOnTime;     // ms
};

// Each phase is active while elapsed is in [minElapsed, maxElapsed).
static const AlarmPhase ALARM_PHASES[] = {
  {      0,  30000, 10000, 250},
  {  30000,  60000,  5000, 250},
  {  60000, 120000,  1000, 200},
  { 120000, 150000,   500, 150},
  { 150000, 180000,   200, 150},
  { 180000, 0xFFFFFFFFUL, 0, 0}
};

void startBuzzer() {
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
  alarmState       = ALARM;
  alarmStartedAt = buzzerLastToggle = millis();
  buzzerToneOn     = true;
  Serial.println("Buzzer ON");
}

void resetTodayCancelledIfSafe() {
  struct tm tm;
  if (!getLocalTime(&tm)) return;
  int curMins   = tm.tm_hour * 60 + tm.tm_min;
  int alarmMins = schedule[tm.tm_wday].enabled
                  ? schedule[tm.tm_wday].hour * 60 + schedule[tm.tm_wday].minute
                  : -1;
  todayCancelled = (alarmMins >= 0 && curMins >= alarmMins);
}

void dismiss() {
  bool wasRinging = (alarmState == ALARM);
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
  alarmState     = IDLE;
  todayCancelled = true;
  Serial.println("Alarm dismissed / cancelled for today.");
  cancelKeepAwake();
  if (wasRinging) startKeepAwake();
  logNextAlarm();
}

void handleBuzzerEscalation() {
  if (alarmState != ALARM) return;

  unsigned long now     = millis();
  unsigned long elapsed = now - alarmStartedAt;

  const AlarmPhase* phase = nullptr;
  for (const auto& p : ALARM_PHASES) {
    if (elapsed >= p.minElapsed && elapsed < p.maxElapsed) { phase = &p; break; }
  }
  if (!phase) return;

  if (phase->buzzerPeriod == 0) {
    if (phase->buzzerOnTime == 0 && !buzzerToneOn) {
      digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
      buzzerToneOn = true;
    } else if (phase->buzzerOnTime != 0 && buzzerToneOn) {
      digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
      buzzerToneOn = false;
    }
    return;
  }

  if (!buzzerToneOn && now - buzzerLastToggle >= (phase->buzzerPeriod - phase->buzzerOnTime)) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
    buzzerToneOn     = true;
    buzzerLastToggle = now;
  } else if (buzzerToneOn && now - buzzerLastToggle >= phase->buzzerOnTime) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
    buzzerToneOn     = false;
    buzzerLastToggle = now;
  }
}
