#include "led.h"
#include "model.h"

// Common anode: a pin driven low lights its colour, so PWM duty is inverted.
static const uint8_t PIN_R = 22, PIN_G = 16, PIN_B = 17;
static const uint32_t PWM_HZ = 5000;

static uint32_t flashUntil = 0;
static uint8_t flashRgb[3] = {0, 0, 0};

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void pwmAttach(uint8_t pin, uint8_t) { ledcAttach(pin, PWM_HZ, 8); }
static void pwmWrite(uint8_t pin, uint8_t, uint8_t duty) { ledcWrite(pin, duty); }
#else
// Channel 0 is the backlight (ui.cpp); the LED uses 1-3.
static void pwmAttach(uint8_t pin, uint8_t ch) { ledcSetup(ch, PWM_HZ, 8); ledcAttachPin(pin, ch); }
static void pwmWrite(uint8_t, uint8_t ch, uint8_t duty) { ledcWrite(ch, duty); }
#endif

// The eye sees PWM brightness far from linearly; squaring evens the steps out.
static uint8_t gamma8(uint16_t v) { return (uint8_t)((v * v) / 255); }

static void output(uint8_t r, uint8_t g, uint8_t b) {
  static int last = -1;
  const int key = (r << 16) | (g << 8) | b;
  if (key == last) return;
  last = key;
  pwmWrite(PIN_R, 1, 255 - gamma8(r));
  pwmWrite(PIN_G, 2, 255 - gamma8(g));
  pwmWrite(PIN_B, 3, 255 - gamma8(b));
}

void ledBegin() {
  // Off before PWM takes over, so it doesn't flash at boot.
  for (uint8_t p : {PIN_R, PIN_G, PIN_B}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  pwmAttach(PIN_R, 1);
  pwmAttach(PIN_G, 2);
  pwmAttach(PIN_B, 3);
  output(0, 0, 0);
}

void ledFlash(uint8_t r, uint8_t g, uint8_t b, uint16_t ms) {
  flashRgb[0] = r; flashRgb[1] = g; flashRgb[2] = b;
  flashUntil = millis() + ms;
}

// 0.15 .. 1 and back over period ms: a slow "breathing" pulse.
static float breathe(uint32_t period) {
  const float ph = (millis() % period) / (float)period;
  return 0.15f + 0.85f * (0.5f - 0.5f * cosf(ph * TWO_PI));
}

void ledTick(const LedInput &in) {
  const Settings &s = g_settings;
  if ((int32_t)(flashUntil - millis()) > 0) {
    output(flashRgb[0], flashRgb[1], flashRgb[2]);
    return;
  }
  if (s.led == LedMode::Off || s.ledLevel == 0) { output(0, 0, 0); return; }

  // Pick a colour and how strongly it pulses (k = 1 is steady).
  uint8_t r = 0, g = 0, b = 0;
  float k = 1;
  if (in.alert) { r = 255; k = breathe(1600); }                       // limit alert: red pulse
  else if (in.back) { g = 255; b = 40; k = breathe(2400); }           // limit reset: green pulse
  else if (s.led == LedMode::Alerts) { output(0, 0, 0); return; }     // nothing to report
  else if (in.offline || in.pct < 0) { g = 40; b = 255; k = 0.35f * breathe(4000); }  // no data: dim blue
  // Same thresholds as the gauge colours (levelColor in ui.cpp).
  else if (in.pct >= 85) { r = 255; }
  else if (in.pct >= 60) { r = 255; g = 110; }
  else { g = 255; b = 30; }

  // The level setting, a third of it while the screen is dimmed; alerts ignore the dim.
  float level = s.ledLevel / 255.0f * k;
  if (in.dimmed && !in.alert && !in.back) level /= 3;
  output(r * level, g * level, b * level);
}
