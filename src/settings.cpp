#include "settings.h"
#include <Preferences.h>

static Preferences prefs;
static Settings saved;

static const uint8_t LAYOUT = 1;   // bump when the stored fields change meaning

void settingsLoad() {
  prefs.begin("meter", true);
  if (prefs.getUChar("layout", 0) == LAYOUT) {
    Settings s;
    s.brightness = prefs.getUChar("bright", s.brightness);
    s.alert1 = prefs.getUChar("alert1", s.alert1);
    s.alert2 = prefs.getUChar("alert2", s.alert2);
    s.h24 = prefs.getBool("h24", s.h24);
    s.rot = prefs.getUChar("rot", s.rot) & 3;
    s.cycle = prefs.getUShort("cycle", s.cycle);
    uint8_t fx = prefs.getUChar("fx", 0);
    s.fx = fx <= 2 ? (Fx)fx : Fx::None;
    uint8_t clock = prefs.getUChar("clock", 0);
    s.clock = clock < CLOCK_STYLES ? (ClockStyle)clock : ClockStyle::Digital;
    uint8_t led = prefs.getUChar("led", (uint8_t)s.led);
    s.led = led < LED_MODES ? (LedMode)led : LedMode::Usage;
    s.ledLevel = prefs.getUChar("ledlvl", s.ledLevel);
    s.dim = prefs.getUShort("dim", s.dim);
    g_settings = s;
  }
  prefs.end();
  saved = g_settings;
}

void settingsSave() {
  if (g_settings == saved) return;
  const Settings &s = g_settings;
  prefs.begin("meter", false);
  prefs.putUChar("layout", LAYOUT);
  prefs.putUChar("bright", s.brightness);
  prefs.putUChar("alert1", s.alert1);
  prefs.putUChar("alert2", s.alert2);
  prefs.putBool("h24", s.h24);
  prefs.putUChar("rot", s.rot);
  prefs.putUShort("cycle", s.cycle);
  prefs.putUChar("fx", (uint8_t)s.fx);
  prefs.putUChar("clock", (uint8_t)s.clock);
  prefs.putUChar("led", (uint8_t)s.led);
  prefs.putUChar("ledlvl", s.ledLevel);
  prefs.putUShort("dim", s.dim);
  prefs.end();
  saved = s;
}
