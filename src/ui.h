#pragma once
#include <Arduino.h>
#include "model.h"

enum class View : uint8_t {
  Main = 0,      // gauges
  Today = 1,     // daily detail
  Clock = 2,     // big clock in one of several styles (Settings::clock)
  Weather = 3,   // current weather for the place set on the companion
  Calendar = 4,  // month grid
  Launcher = 5,  // app tiles that open things on the PC
  PageCount = 6, // number of pageable screens
  Alert,         // limit alert overlay
  Back,          // "limit has reset" overlay
};

struct AlertInfo {
  int pct = 0;
  bool reached = false;   // pct >= 100
};

// Screen state that main.cpp owns and the renderer only reads.
struct UiState {
  bool playing = false;   // automatic page change running (play/pause button)
  int monthOffset = 0;    // calendar: months away from the current one
  int pressedApp = -1;    // launcher: tile drawn highlighted right after a tap
};
extern UiState g_ui;

void uiBegin();
void uiSetBrightness(uint8_t level);
// 0 = portrait 240x320, 1 = landscape 320x240, 2 = portrait flipped, 3 = landscape
// flipped. The next uiRender repaints the whole screen.
void uiSetRotation(uint8_t rot);
// fx animates the change to this view; Fx::None just redraws.
void uiRender(View v, const AlertInfo &alert, Fx fx = Fx::None);

// What a tap at screen (x, y) on view v lands on.
enum class Hit : uint8_t {
  None,
  Prev, Next, PlayPause,              // the nav bar on every pageable screen
  MonthPrev, MonthNext, MonthToday,   // calendar title row
  App,                                // launcher tile, index in HitResult
  Snooze, Dismiss,                    // alert buttons
};
struct HitResult {
  Hit kind = Hit::None;
  int index = -1;
};
HitResult uiHitTest(View v, int x, int y);
