#pragma once
#include <Arduino.h>
#include "model.h"

enum class View : uint8_t {
  Main = 0,      // gauges
  Today = 1,     // daily detail
  PageCount = 2, // number of pageable screens
  Alert,         // limit alert overlay
  Back,          // "limit has reset" overlay
};

struct AlertInfo {
  int pct = 0;
  bool reached = false;   // pct >= 100
};

void uiBegin();
void uiSetBrightness(uint8_t level);
// 0 = portrait 240x320, 1 = landscape 320x240, 2 = portrait flipped, 3 = landscape
// flipped. The next uiRender repaints the whole screen.
void uiSetRotation(uint8_t rot);
// fx animates the change to this view; Fx::None just redraws.
void uiRender(View v, const AlertInfo &alert, Fx fx = Fx::None);

// Hit-testing for the alert screen buttons. 0 = none, 1 = snooze, 2 = dismiss.
int uiAlertButtonAt(int x, int y);
