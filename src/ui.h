#pragma once
#include <Arduino.h>

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
void uiRender(View v, const AlertInfo &alert);

// Hit-testing for the alert screen buttons. 0 = none, 1 = snooze, 2 = dismiss.
int uiAlertButtonAt(int x, int y);
