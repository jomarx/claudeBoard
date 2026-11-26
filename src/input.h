#pragma once
#include <Arduino.h>

enum class InputKind { None, Tap, LongPress, ButtonShort, ButtonLong };

struct InputEvent {
  InputKind kind = InputKind::None;
  int x = 0, y = 0;  // screen coords for Tap / LongPress
};

void inputBegin();
InputEvent inputPoll();
