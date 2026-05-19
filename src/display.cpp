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

void updateDisplay(int hour, int minute) {
  int h = hour % 12;
  if (h == 0) h = 12;
  if (h >= 10)
    display.writeDigitNum(0, h / 10, false);
  else
    display.writeDigitRaw(0, 0x00); // blank leading digit for single-digit hours
  display.writeDigitNum(1, h % 10, false);
  uint8_t special = 0x02;   // colon
  if (alarmArmed)  special |= 0x04; // alarm-armed dot
  if (hour >= 12)  special |= 0x08; // PM dot
  display.writeDigitRaw(2, special);
  display.writeDigitNum(3, minute / 10, false);
  display.writeDigitNum(4, minute % 10, false);
  display.writeDisplay();
}

void showDashes() {
  for (int i : {0, 1, 3, 4}) display.writeDigitRaw(i, 0x40); // segment G = dash
  display.drawColon(false);
  display.writeDisplay();
}

void showOtaProgress(int pct) {
  pct = pct > 99 ? 99 : (pct < 0 ? 0 : pct); // display only has 2 digits
  display.writeDigitRaw(0, 0x00);
  display.writeDigitRaw(1, 0x00);
  display.drawColon(false);
  display.writeDigitNum(3, pct / 10, false);
  display.writeDigitNum(4, pct % 10, false);
  display.writeDisplay();
}
