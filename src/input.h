#pragma once
#include <Arduino.h>

// ButtonHold = BOOT held 5 s (after its ButtonLong at 0.7 s): opens Wi-Fi setup on the board.
enum class InputKind { None, Tap, LongPress, ButtonShort, ButtonLong, ButtonHold };

struct InputEvent {
  InputKind kind = InputKind::None;
  int x = 0, y = 0;  // screen coords for Tap / LongPress
};

void inputBegin();
InputEvent inputPoll();
