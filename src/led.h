#pragma once
#include <Arduino.h>

// The RGB LED on the back of the board (GPIO 22 red, 16 green, 17 blue, low = on), driven
// by PWM as a status light. What it shows depends on Settings::led; see led.cpp.

// What the rest of the firmware knows that the LED reflects; main.cpp fills it every loop.
struct LedInput {
  int pct = -1;          // 5-hour session %, -1 = unknown
  bool alert = false;    // the limit alert is on screen
  bool back = false;     // the "you're back" screen is up
  bool dimmed = false;   // the screen is dimmed for idle
  bool offline = false;  // no usage data from the PC (never, or not for a while)
};

void ledBegin();
void ledTick(const LedInput &in);
// A short blink over whatever the LED is showing, e.g. when a launcher tile is tapped.
void ledFlash(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);
