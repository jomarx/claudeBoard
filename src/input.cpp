#include "input.h"
#include "model.h"

static const int BOOT_BTN_PIN = 0;
static const uint32_t LONG_MS = 700;
static const uint32_t HOLD_MS = 5000;
static const uint32_t DEBOUNCE_MS = 30;

// ---------- raw touch ----------

#if defined(TOUCH_XPT2046)
#include <SPI.h>

// The XPT2046 sits on its own SPI bus (the display uses HSPI), so it gets VSPI.
static SPIClass touchSpi(VSPI);
static const SPISettings XPT_SPI(2000000, MSBFIRST, SPI_MODE0);

static uint16_t xptRead(uint8_t cmd) {
  touchSpi.transfer(cmd);
  uint16_t v = touchSpi.transfer16(0) >> 3;  // 12-bit result
  return v;
}

static void touchBegin() {
  pinMode(XPT_CS, OUTPUT);
  digitalWrite(XPT_CS, HIGH);
  pinMode(XPT_IRQ, INPUT);  // GPIO36 is input-only, the board has the pull-up
  touchSpi.begin(XPT_SCK, XPT_MISO, XPT_MOSI, XPT_CS);
}

static bool touchRaw(uint16_t &rx, uint16_t &ry, uint16_t &z) {
  if (digitalRead(XPT_IRQ) == HIGH) return false;  // IRQ goes low while pressed
  touchSpi.beginTransaction(XPT_SPI);
  digitalWrite(XPT_CS, LOW);
  uint16_t z1 = xptRead(0xB1);
  uint16_t z2 = xptRead(0xC1);
  z = z1 + 4095 - z2;
  // Take a few samples and average; the first X/Y read after a Z read is noisy.
  xptRead(0x91);
  uint32_t sx = 0, sy = 0;
  for (int i = 0; i < 4; i++) {
    sy += xptRead(0x91);
    sx += xptRead(0xD1);
  }
  xptRead(0xD0);  // power down, keep IRQ enabled
  digitalWrite(XPT_CS, HIGH);
  touchSpi.endTransaction();
  rx = sx / 4;
  ry = sy / 4;
  return z > 300;
}

static bool touchRead(int &x, int &y) {
  uint16_t rx, ry, z;
  if (!touchRaw(rx, ry, z)) return false;
#ifdef TOUCH_SWAP_XY
  uint16_t t = rx; rx = ry; ry = t;
#endif
  x = map(rx, TOUCH_X_MIN, TOUCH_X_MAX, 0, 239);
  y = map(ry, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 319);
#ifdef TOUCH_INVERT_X
  x = 239 - x;
#endif
#ifdef TOUCH_INVERT_Y
  y = 319 - y;
#endif
  // The calibration above gives portrait (rotation 0) coordinates; turn them into the
  // current rotation's, using the same mapping TFT_eSPI uses for drawing.
  int px = constrain(x, 0, 239), py = constrain(y, 0, 319);
  switch (g_settings.rot) {
    case 1: x = py;        y = 239 - px; break;  // landscape, 320x240
    case 2: x = 239 - px;  y = 319 - py; break;  // portrait flipped
    case 3: x = 319 - py;  y = px;       break;  // landscape flipped
    default: x = px;       y = py;       break;
  }
#ifdef TOUCH_DEBUG
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 250) {
    lastLog = millis();
    Serial.printf("{\"type\":\"touch\",\"rx\":%u,\"ry\":%u,\"z\":%u,\"x\":%d,\"y\":%d}\n", rx, ry, z, x, y);
  }
#endif
  return true;
}

#else
static void touchBegin() {}
static bool touchRead(int &, int &) { return false; }
#endif

// ---------- gesture state ----------

static bool tDown = false, tLongSent = false;
static uint32_t tStart = 0, tLastSeen = 0;
static int tx = 0, ty = 0;

static bool bDown = false, bHoldSent = false;
static uint32_t bStart = 0;

void inputBegin() {
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  touchBegin();
}

InputEvent inputPoll() {
  InputEvent ev;
  uint32_t now = millis();

  // BOOT button. Opening the serial port pulses GPIO0 through the auto-reset
  // circuit, so ignore it until it has been seen released after boot.
  static bool bArmed = false;
  bool pressed = digitalRead(BOOT_BTN_PIN) == LOW;
  if (!bArmed) {
    if (!pressed && now > 1500) bArmed = true;
    pressed = false;
  }
  // Short and long presses are decided on release, so a 5 s hold (Wi-Fi setup) never sends
  // the long press (brightness) on its way there; the hold fires while still pressed.
  if (pressed && !bDown) {
    bDown = true; bHoldSent = false; bStart = now;
  } else if (pressed && bDown && !bHoldSent && now - bStart > HOLD_MS) {
    bHoldSent = true;
    ev.kind = InputKind::ButtonHold;
    return ev;
  } else if (!pressed && bDown) {
    bDown = false;
    const uint32_t held = now - bStart;
    if (bHoldSent || held <= DEBOUNCE_MS) return ev;
    ev.kind = held > LONG_MS ? InputKind::ButtonLong : InputKind::ButtonShort;
    return ev;
  }

  // Touch. A finger is "released" after 60 ms without a reading, which
  // smooths over the dropouts resistive panels produce.
  int x, y;
  if (touchRead(x, y)) {
    tLastSeen = now;
    if (!tDown) {
      tDown = true; tLongSent = false; tStart = now; tx = x; ty = y;
    } else if (!tLongSent && now - tStart > LONG_MS) {
      tLongSent = true;
      ev.kind = InputKind::LongPress; ev.x = tx; ev.y = ty;
      return ev;
    }
  } else if (tDown && now - tLastSeen > 60) {
    tDown = false;
    if (!tLongSent && tLastSeen - tStart >= DEBOUNCE_MS) {
      ev.kind = InputKind::Tap; ev.x = tx; ev.y = ty;
      return ev;
    }
  }
  return ev;
}
