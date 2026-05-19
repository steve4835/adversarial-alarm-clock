#include "globals.h"
#include "alarm.h"
#include "config.h"
#include "schedule.h"

// Module-private state; not exposed through globals
static unsigned long alarmStartedAt  = 0;
static unsigned long buzzerLastToggle = 0;
static bool          buzzerToneOn     = false;

struct AlarmPhase {
  unsigned long minElapsed; // ms
  unsigned long maxElapsed; // ms; 0xFFFFFFFFUL = open-ended
  unsigned long period;     // ms; 0 = continuous on
  unsigned long onTime;     // ms
  bool          buzzerOn;
  bool          strobeOn;
};

// Each phase is active while elapsed is in [minElapsed, maxElapsed).
static const AlarmPhase ALARM_PHASES[] = {
  {      0,  30000, 10000, 250, true, false }, // 0–30 s: slow beeps, no strobe
  {  30000,  60000,  5000, 250, true, false }, // 30–60 s: medium beeps, no strobe
  {  60000, 120000,  1000, 200, true, false }, // 1–2 min: faster beeps
  { 120000, 150000,   500, 150, true, true  }, // 2–2.5 min: rapid beeps + strobe
  { 150000, 180000,   200,  75, true, true  }, // 2.5–3 min: very rapid + strobe
  { 180000, 0xFFFFFFFFUL, 0, 0, true, true  }, // 3 min+: continuous + strobe
};

void startBuzzer() {
  digitalWrite(GPIO_RELAY, LOW); // relay is escalation-controlled; start clean
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
  alarmState       = ALARM;
  alarmStartedAt   = millis();
  buzzerLastToggle = millis();
  buzzerToneOn     = true;
  Serial.println("Buzzer ON");
}

void dismiss() {
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(GPIO_RELAY, LOW);
  alarmState     = IDLE;
  todayCancelled = true;
  Serial.println("Alarm dismissed / cancelled for today.");
  logNextAlarm();
}

void handleBuzzerEscalation() {
  if (alarmState != ALARM) return;

  unsigned long elapsed = millis() - alarmStartedAt;
  unsigned long now     = millis();

  const AlarmPhase* phase = nullptr;
  for (const auto& p : ALARM_PHASES) {
    if (elapsed >= p.minElapsed && elapsed < p.maxElapsed) { phase = &p; break; }
  }
  if (!phase) return;

  digitalWrite(GPIO_RELAY, phase->strobeOn ? HIGH : LOW);

  if (phase->period == 0) {
    if (phase->buzzerOn && !buzzerToneOn) {
      digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
      buzzerToneOn = true;
    } else if (!phase->buzzerOn && buzzerToneOn) {
      digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
      buzzerToneOn = false;
    }
    return;
  }

  if (!buzzerToneOn && now - buzzerLastToggle >= (phase->period - phase->onTime)) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
    buzzerToneOn     = true;
    buzzerLastToggle = now;
  } else if (buzzerToneOn && now - buzzerLastToggle >= phase->onTime) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
    buzzerToneOn     = false;
    buzzerLastToggle = now;
  }
}
