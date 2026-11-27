#pragma once
#include <Arduino.h>

#define FW_VERSION "0.3.0"

// How long without a usage frame before the status dot changes.
#define STALE_AFTER_MS   (120UL * 1000UL)
#define OFFLINE_AFTER_MS (600UL * 1000UL)

struct ModelShare {
  char name[16];
  uint8_t pct;
};

struct UsageData {
  bool valid = false;           // at least one usage frame received
  char plan[16] = "";

  int16_t sessionPct = -1;      // 5-hour window, -1 = unknown
  uint32_t sessionReset = 0;    // unix seconds, 0 = unknown
  int16_t weekPct = -1;
  uint32_t weekReset = 0;
  int16_t opusPct = -1;         // weekly Opus-only limit, -1 = hide

  // today
  uint64_t tokens = 0;
  float cost = 0;               // API-equivalent USD
  uint8_t sessions = 0;         // active Claude Code sessions
  int16_t cachePct = -1;
  uint32_t hourly[24] = {0};    // tokens per local hour 0..23
  ModelShare models[4];
  uint8_t modelCount = 0;
};

enum class Fx : uint8_t { None = 0, Wipe = 1, Fade = 2 };
enum class ClockStyle : uint8_t { Digital = 0, Analog = 1, Rings = 2, Flip = 3, Words = 4, Binary = 5 };
#define CLOCK_STYLES 6
// The RGB LED on the back: usage colour + alert pulses, alert pulses only, or dark.
enum class LedMode : uint8_t { Off = 0, Usage = 1, Alerts = 2 };
#define LED_MODES 3

// Saved in NVS (settings.cpp); the companion can change every field with a cfg frame.
struct Settings {
  uint8_t brightness = 200;
  uint8_t alert1 = 80;
  uint8_t alert2 = 90;
  bool h24 = true;
  uint8_t rot = 0;          // display rotation, 0 portrait, 1 landscape, 2/3 flipped
  uint16_t cycle = 0;       // seconds between automatic page changes, 0 = off
  Fx fx = Fx::None;         // effect when the page changes
  ClockStyle clock = ClockStyle::Digital;   // the clock screen's face
  LedMode led = LedMode::Usage;             // what the back LED shows (led.cpp)
  uint8_t ledLevel = 60;                    // its brightness, 0-255
  uint16_t dim = 120;       // seconds idle before dimming, 0 = never

  bool operator==(const Settings &o) const {
    return brightness == o.brightness && alert1 == o.alert1 && alert2 == o.alert2 && h24 == o.h24 &&
           rot == o.rot && cycle == o.cycle && fx == o.fx && dim == o.dim &&
           clock == o.clock && led == o.led && ledLevel == o.ledLevel;
  }
  bool operator!=(const Settings &o) const { return !(*this == o); }
};

// Wall clock, synced from the host on every frame.
struct Clock {
  uint32_t epochAtSync = 0;     // unix seconds (UTC)
  uint32_t millisAtSync = 0;
  int32_t tzOffset = 0;         // seconds to add for local time

  bool synced() const { return epochAtSync != 0; }
  uint32_t now() const { return epochAtSync + (millis() - millisAtSync) / 1000; }
  uint32_t localNow() const { return now() + tzOffset; }
};

extern UsageData g_usage;
extern Settings g_settings;
extern Clock g_clock;
extern uint32_t g_lastFrameMs;   // millis() of the last usage frame, 0 = never

// Launcher tiles, sent by the companion in an "apps" frame. Only the names live on the
// board; tapping tile i sends a "launch" event with value i and the PC opens it.
#define MAX_APPS 8
struct AppEntry {
  char name[16];
};

extern AppEntry g_apps[MAX_APPS];
extern uint8_t g_appCount;

// Current weather, sent by the companion (it fetches Open-Meteo for the place set on its
// settings page). Temperatures in degrees C, wind in km/h.
struct WeatherData {
  bool valid = false;
  char place[24] = "";
  int16_t temp = 0, feels = 0, hi = 0, lo = 0;
  uint8_t hum = 0;              // relative humidity %
  uint16_t wind = 0;
  int8_t rain = -1;             // today's chance of rain %, -1 = unknown
  uint8_t code = 0;             // WMO weather code
  bool day = true;
  uint32_t at = 0;              // unix seconds of the observation, 0 = unknown
};

extern WeatherData g_weather;
