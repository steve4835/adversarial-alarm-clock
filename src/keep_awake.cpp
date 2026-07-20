#include "globals.h"
#include "keep_awake.h"
#include "config.h"

enum KeepAwakeState { KA_IDLE, KA_WAITING, KA_CHIRPING };

static KeepAwakeState kaState           = KA_IDLE;
static unsigned long  kaWindowStartedAt = 0; // millis anchor; all timing is elapsed-since-this
static int            kaOccurrenceIndex = 0; // 0-based; occurrence N fires at (N+1)*INTERVAL
static bool           kaOccurrenceDismissed = false;
static unsigned long  kaChirpStartedAt  = 0;
static unsigned long  kaChirpLastToggle = 0;
static bool           kaChirpToneOn     = false;

static const int TOTAL_OCCURRENCES = KEEP_AWAKE_DURATION_MS / KEEP_AWAKE_INTERVAL_MS;

// elapsed time (ms) at which the occurrence at kaOccurrenceIndex is due
static unsigned long occurrenceDueAt() {
  return (unsigned long)(kaOccurrenceIndex + 1) * KEEP_AWAKE_INTERVAL_MS;
}

static void turnBuzzerOff() {
  digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
  kaChirpToneOn = false;
}

static void advanceOccurrence() {
  kaOccurrenceIndex++;
  if (kaOccurrenceIndex >= TOTAL_OCCURRENCES) {
    kaState = KA_IDLE;
    return;
  }
  kaState               = KA_WAITING;
  kaOccurrenceDismissed = false;
}

void startKeepAwake() {
  kaState               = KA_WAITING;
  kaWindowStartedAt     = millis();
  kaOccurrenceIndex     = 0;
  kaOccurrenceDismissed = false;
  Serial.println("Keep-awake sequence started.");
}

void cancelKeepAwake() {
  if (kaState == KA_CHIRPING) turnBuzzerOff();
  kaState = KA_IDLE;
}

void handleKeepAwake() {
  if (kaState == KA_IDLE) return;

  unsigned long now     = millis();
  unsigned long elapsed = now - kaWindowStartedAt;

  if (kaState == KA_WAITING) {
    if (elapsed < occurrenceDueAt()) return;
    if (kaOccurrenceDismissed) {
      advanceOccurrence();
      return;
    }
    kaState           = KA_CHIRPING;
    kaChirpStartedAt  = now;
    kaChirpLastToggle = now;
    kaChirpToneOn     = false;
    return;
  }

  // KA_CHIRPING
  if (now - kaChirpStartedAt >= KEEP_AWAKE_CHIRP_WINDOW_MS) {
    turnBuzzerOff();
    advanceOccurrence();
    return;
  }

  if (!kaChirpToneOn &&
      now - kaChirpLastToggle >= (KEEP_AWAKE_CHIRP_PERIOD_MS - KEEP_AWAKE_CHIRP_ON_MS)) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? LOW : HIGH);
    kaChirpToneOn     = true;
    kaChirpLastToggle = now;
  } else if (kaChirpToneOn && now - kaChirpLastToggle >= KEEP_AWAKE_CHIRP_ON_MS) {
    digitalWrite(GPIO_BUZZER, BUZZER_ACTIVE_LOW ? HIGH : LOW);
    kaChirpToneOn     = false;
    kaChirpLastToggle = now;
  }
}

bool dismissKeepAwake() {
  if (kaState == KA_IDLE) return false;

  if (kaState == KA_CHIRPING) {
    turnBuzzerOff();
    advanceOccurrence();
    Serial.println("Keep-awake chirp dismissed.");
    return true;
  }

  // KA_WAITING — only allowed within the 5-minute pre-window
  unsigned long elapsed = millis() - kaWindowStartedAt;
  unsigned long dueAt   = occurrenceDueAt();
  if (dueAt - elapsed <= KEEP_AWAKE_DISMISS_WINDOW_MS) {
    kaOccurrenceDismissed = true;
    Serial.println("Keep-awake next occurrence pre-dismissed.");
    return true;
  }
  return false;
}

bool isKeepAwakeActive() {
  return kaState != KA_IDLE;
}

bool keepAwakeDismissWindowOpen() {
  if (kaState == KA_CHIRPING) return true;
  if (kaState == KA_WAITING && !kaOccurrenceDismissed) {
    unsigned long elapsed = millis() - kaWindowStartedAt;
    unsigned long dueAt   = occurrenceDueAt();
    return (dueAt - elapsed) <= KEEP_AWAKE_DISMISS_WINDOW_MS;
  }
  return false;
}
