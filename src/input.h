#pragma once
#include <Arduino.h>

// BOOT: ButtonShort / ButtonLong (held over 0.7 s) on release; ButtonHold when held 5 s,
// while still pressed, with no ButtonLong after it: opens Wi-Fi setup on the board.
enum class InputKind { None, Tap, LongPress, ButtonShort, ButtonLong, ButtonHold };

struct InputEvent {
  InputKind kind = InputKind::None;
  int x = 0, y = 0;  // screen coords for Tap / LongPress
};

void inputBegin();
InputEvent inputPoll();
