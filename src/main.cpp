// Claude usage meter firmware.
// Data arrives as newline-delimited JSON over the USB serial port (see protocol.h);
// a companion script on the PC collects the numbers and pushes them down.

#include <Arduino.h>
#include "model.h"
#include "protocol.h"
#include "ui.h"
#include "input.h"

UsageData g_usage;
Settings g_settings;
Clock g_clock;
uint32_t g_lastFrameMs = 0;

static View page = View::Main;      // the pageable screen underneath any overlay
static View shown = View::Main;     // what is on the glass right now
static AlertInfo alert;
static bool dirty = true;

// ---------- alerts ----------

static uint32_t windowReset = 0;    // sessionReset of the window we are tracking
static int firedLevel = 0;          // highest threshold already alerted in this window
static bool limitHit = false;
static uint32_t snoozeUntilMs = 0;
static bool snoozed = false;
static uint32_t backShownMs = 0;

static void show(View v) {
  if (v != shown) { shown = v; dirty = true; }
}

static void showPage() { show(page); }

static int levelFor(int pct) {
  if (pct >= 100) return 100;
  if (pct >= g_settings.alert2) return g_settings.alert2;
  if (pct >= g_settings.alert1) return g_settings.alert1;
  return 0;
}

static void evaluateAlerts() {
  const UsageData &u = g_usage;
  if (u.sessionPct < 0) return;

  // A new 5-hour window: the reset time moved forward, or usage fell sharply.
  bool newWindow = (u.sessionReset && windowReset && u.sessionReset > windowReset + 60) ||
                   (firedLevel > 0 && u.sessionPct + 30 < firedLevel);
  if (newWindow) {
    bool wasHit = limitHit;
    firedLevel = 0;
    limitHit = false;
    snoozed = false;
    if (wasHit) {
      backShownMs = millis();
      show(View::Back);
    } else if (shown == View::Alert) {
      showPage();
    }
  }
  if (u.sessionReset) windowReset = u.sessionReset;

  int lvl = levelFor(u.sessionPct);
  if (u.sessionPct >= 100) limitHit = true;

  alert.pct = u.sessionPct;
  alert.reached = u.sessionPct >= 100;

  if (lvl > firedLevel) {
    // A higher threshold always breaks through a snooze.
    firedLevel = lvl;
    snoozed = false;
    show(View::Alert);
    dirty = true;
  } else if (shown == View::Alert) {
    dirty = true;  // keep the number on the alert screen current
  }
}

static void tickAlerts() {
  if (snoozed && (int32_t)(millis() - snoozeUntilMs) >= 0) {
    snoozed = false;
    if (firedLevel > 0 && levelFor(g_usage.sessionPct) >= firedLevel) show(View::Alert);
  }
  if (shown == View::Back && millis() - backShownMs > 15000) showPage();
}

// ---------- brightness / idle dim ----------

static const uint32_t DIM_AFTER_MS = 120000;
static uint32_t lastActivityMs = 0;
static bool dimmed = false;

static void wake() {
  lastActivityMs = millis();
  if (dimmed) {
    dimmed = false;
    uiSetBrightness(g_settings.brightness);
  }
}

static void tickDim() {
  if (shown == View::Alert || shown == View::Back) return;  // never dim an alert
  if (!dimmed && millis() - lastActivityMs > DIM_AFTER_MS) {
    dimmed = true;
    uiSetBrightness(max<uint8_t>(g_settings.brightness / 5, 4));
  }
}

static void cycleBrightness() {
  static const uint8_t steps[] = {40, 120, 200, 255};
  int i = 0;
  while (i < 4 && steps[i] <= g_settings.brightness) i++;
  g_settings.brightness = steps[i % 4];
  uiSetBrightness(g_settings.brightness);
  protocolSendEvent("brightness", g_settings.brightness);
}

// ---------- input ----------

static void nextPage() {
  page = (View)(((int)page + 1) % (int)View::PageCount);
  showPage();
  protocolSendEvent("screen", (int)page);
}

static void handleInput(const InputEvent &ev) {
  if (ev.kind == InputKind::None) return;
  bool wasDim = dimmed;
  wake();
  if (wasDim) return;  // the first touch only wakes the screen

  switch (shown) {
    case View::Alert:
      if (ev.kind == InputKind::Tap) {
        int b = uiAlertButtonAt(ev.x, ev.y);
        if (b == 1) {
          snoozed = true;
          snoozeUntilMs = millis() + 30UL * 60UL * 1000UL;
          protocolSendEvent("snooze", alert.pct);
          showPage();
        } else if (b == 2) {
          protocolSendEvent("dismiss", alert.pct);
          showPage();
        }
      } else if (ev.kind == InputKind::ButtonShort) {
        protocolSendEvent("dismiss", alert.pct);
        showPage();
      }
      return;
    case View::Back:
      showPage();
      return;
    default:
      break;
  }

  if (ev.kind == InputKind::Tap || ev.kind == InputKind::ButtonShort) nextPage();
  else if (ev.kind == InputKind::LongPress || ev.kind == InputKind::ButtonLong) cycleBrightness();
}

// ---------- main ----------

void setup() {
  // Board peripherals the meter doesn't use: RGB LED off (active low),
  // speaker amplifier off (enable is active low).
  for (int p : {22, 16, 17}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);

  protocolBegin();
  uiBegin();
  inputBegin();
  lastActivityMs = millis();
  uiRender(shown, alert);
  dirty = false;
  protocolSendHello();
}

// Things on screen that change with time alone: the minute, and live/stale/offline.
static uint32_t timeSignature() {
  uint32_t sig = g_clock.synced() ? g_clock.now() / 60 : 0;
  if (g_lastFrameMs) {
    uint32_t age = millis() - g_lastFrameMs;
    sig = sig * 4 + (age < STALE_AFTER_MS ? 1 : age < OFFLINE_AFTER_MS ? 2 : 3);
  }
  return sig;
}

void loop() {
  int screenReq = -1;
  switch (protocolPoll(screenReq)) {
    case FrameResult::Usage:
      evaluateAlerts();
      dirty = true;
      break;
    case FrameResult::Config:
      if (!dimmed) uiSetBrightness(g_settings.brightness);
      evaluateAlerts();
      dirty = true;
      break;
    case FrameResult::Screen:
      if (screenReq >= 0 && screenReq < (int)View::PageCount) {
        page = (View)screenReq;
        wake();
        showPage();
      }
      break;
    default:
      break;
  }

  handleInput(inputPoll());
  tickAlerts();
  tickDim();

  static uint32_t lastSig = 0;
  uint32_t sig = timeSignature();
  if (sig != lastSig) { lastSig = sig; dirty = true; }

  if (dirty) {
    dirty = false;
    uiRender(shown, alert);
  }
  delay(10);
}
