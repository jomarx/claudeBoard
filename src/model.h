#pragma once
#include <Arduino.h>

#define FW_VERSION "0.1.0"

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

struct Settings {
  uint8_t brightness = 200;
  uint8_t alert1 = 80;
  uint8_t alert2 = 90;
  bool h24 = true;
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
