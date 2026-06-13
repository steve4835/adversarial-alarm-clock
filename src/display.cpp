#include "globals.h"
#include "display.h"
#include "config.h"
#include <Adafruit_LEDBackpack.h>

static Adafruit_7segment display;

void setupDisplay() {
  display.begin(DISPLAY_I2C_ADDR);
  display.setBrightness(DISPLAY_BRIGHTNESS);
  showDashes();
}

static constexpr uint8_t SEG_BLANK      = 0x00;
static constexpr uint8_t COLON_BIT      = 0x02;
static constexpr uint8_t ALARM_ARMED_BIT = 0x04;
static constexpr uint8_t PM_BIT         = 0x08;
static constexpr uint8_t SEG_DASH       = 0x40;

void updateDisplay(int hour, int minute) {
  int h = hour % 12;
  if (h == 0) h = 12;
  if (h >= 10)
    display.writeDigitNum(0, h / 10, false);
  else
    display.writeDigitRaw(0, SEG_BLANK); // blank leading digit for single-digit hours
  display.writeDigitNum(1, h % 10, false);
  uint8_t special = COLON_BIT;
  if (alarmArmed)  special |= ALARM_ARMED_BIT;
  if (hour >= 12)  special |= PM_BIT;
  display.writeDigitRaw(2, special);
  display.writeDigitNum(3, minute / 10, false);
  display.writeDigitNum(4, minute % 10, false);
  display.writeDisplay();
}

void showDashes() {
  for (int i : {0, 1, 3, 4}) display.writeDigitRaw(i, SEG_DASH);
  display.drawColon(false);
  display.writeDisplay();
}

void showOtaProgress(int pct) {
  pct = pct > 99 ? 99 : (pct < 0 ? 0 : pct); // display only has 2 digits
  display.writeDigitRaw(0, SEG_BLANK);
  display.writeDigitRaw(1, SEG_BLANK);
  display.drawColon(false);
  display.writeDigitNum(3, pct / 10, false);
  display.writeDigitNum(4, pct % 10, false);
  display.writeDisplay();
}
