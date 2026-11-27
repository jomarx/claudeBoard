#include "ui.h"
#include "model.h"
#include <TFT_eSPI.h>
#include <time.h>

TFT_eSPI tft;

// The full frame (240x320x16bit = 150 KB) does not fit in one allocation on a
// plain ESP32, so each frame is drawn twice into a half-height sprite (75 KB):
// once for the top half, once for the bottom half. That keeps redraws
// flicker-free without PSRAM. Portrait uses a 240x160 band, landscape 320x120;
// both are the same number of bytes, so switching reuses the same heap block.
static TFT_eSprite spr(&tft);
static int W = 240, H = 320, BAND = 160;
static bool land = false;   // rotation 1 or 3

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t C_BG = rgb(20, 20, 19);
static const uint16_t C_TRACK = rgb(44, 44, 42);
static const uint16_t C_TEXT = rgb(241, 239, 232);
static const uint16_t C_MUTED = rgb(180, 178, 169);
static const uint16_t C_DIM = rgb(136, 135, 128);
static const uint16_t C_CORAL = rgb(217, 119, 87);
static const uint16_t C_GREEN = rgb(93, 202, 165);
static const uint16_t C_AMBER = rgb(239, 159, 39);
static const uint16_t C_RED = rgb(226, 75, 74);
static const uint16_t C_ALERT_BG = rgb(74, 27, 12);
static const uint16_t C_ALERT_FG = rgb(240, 153, 123);
static const uint16_t C_ALERT_SUB = rgb(245, 196, 179);
static const uint16_t C_OK_BG = rgb(4, 52, 44);
static const uint16_t C_OK_SUB = rgb(159, 225, 203);

// Thin wrapper that shifts every y by the current band offset.
struct G {
  int y0 = 0;
  void fill(uint16_t c) { spr.fillSprite(c); }
  void rect(int x, int y, int w, int h, uint16_t c) { spr.fillRect(x, y - y0, w, h, c); }
  void rrect(int x, int y, int w, int h, int r, uint16_t c) { spr.fillRoundRect(x, y - y0, w, h, r, c); }
  void rframe(int x, int y, int w, int h, int r, uint16_t c) { spr.drawRoundRect(x, y - y0, w, h, r, c); }
  void circle(int x, int y, int r, uint16_t c) { spr.fillCircle(x, y - y0, r, c); }
  void arc(int x, int y, int r, int ir, int a0, int a1, uint16_t fg, uint16_t bg, bool round) {
    spr.drawSmoothArc(x, y - y0, r, ir, a0, a1, fg, bg, round);
  }
  // datum: TL_DATUM, TC_DATUM, TR_DATUM, ML_DATUM, MC_DATUM, ...
  int text(const char *s, int x, int y, int font, uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM) {
    spr.setTextColor(fg, bg);
    spr.setTextDatum(datum);
    return spr.drawString(s, x, y - y0, font);
  }
  int width(const char *s, int font) { return spr.textWidth(s, font); }
};
static G g;

// ---------- formatting ----------

static uint16_t levelColor(int pct) {
  if (pct >= 85) return C_CORAL;
  if (pct >= 60) return C_AMBER;
  return C_GREEN;
}

static void fmtDuration(char *out, size_t n, int32_t secs) {
  if (secs < 0) secs = 0;
  int32_t m = secs / 60, h = m / 60, d = h / 24;
  if (d > 0) snprintf(out, n, "%ldd %ldh", (long)d, (long)(h % 24));
  else if (h > 0) snprintf(out, n, "%ldh %ldm", (long)h, (long)(m % 60));
  else snprintf(out, n, "%ldm", (long)max<int32_t>(m, secs > 0 ? 1 : 0));
}

static void fmtClock(char *out, size_t n, uint32_t localEpoch) {
  time_t t = localEpoch;
  struct tm tm;
  gmtime_r(&t, &tm);
  if (g_settings.h24) {
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
  } else {
    int h = tm.tm_hour % 12;
    snprintf(out, n, "%d:%02d%s", h ? h : 12, tm.tm_min, tm.tm_hour < 12 ? "am" : "pm");
  }
}

static void fmtWeekdayClock(char *out, size_t n, uint32_t localEpoch) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  time_t t = localEpoch;
  struct tm tm;
  gmtime_r(&t, &tm);
  char c[10];
  fmtClock(c, sizeof c, localEpoch);
  snprintf(out, n, "%s %s", days[tm.tm_wday], c);
}

static void fmtDate(char *out, size_t n, uint32_t localEpoch) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t t = localEpoch;
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(out, n, "%s %d %s", days[tm.tm_wday], tm.tm_mday, mon[tm.tm_mon]);
}

static void fmtTokens(char *out, size_t n, uint64_t v) {
  if (v >= 1000000000ULL) snprintf(out, n, "%.1fB", v / 1e9);
  else if (v >= 1000000ULL) snprintf(out, n, "%.1fM", v / 1e6);
  else if (v >= 1000ULL) snprintf(out, n, "%.0fK", v / 1e3);
  else snprintf(out, n, "%u", (unsigned)v);
}

// ---------- shared pieces ----------

static void header(const char *left, uint16_t bg) {
  g.text(left, 12, 6, 2, C_MUTED, bg);

  uint16_t dot = C_DIM;
  const char *state = "no data";
  if (g_lastFrameMs) {
    uint32_t age = millis() - g_lastFrameMs;
    if (age < STALE_AFTER_MS) { dot = C_GREEN; state = "live"; }
    else if (age < OFFLINE_AFTER_MS) { dot = C_AMBER; state = "stale"; }
    else { dot = C_RED; state = "offline"; }
  }
  char right[24];
  if (g_clock.synced()) {
    char c[10];
    fmtClock(c, sizeof c, g_clock.localNow());
    snprintf(right, sizeof right, "%s  %s", state, c);
  } else {
    snprintf(right, sizeof right, "%s", state);
  }
  int w = g.width(right, 2);
  g.text(right, W - 12, 6, 2, C_TEXT, bg, TR_DATUM);
  g.circle(W - 12 - w - 8, 13, 3, dot);
}

static void pageDots(int active) {
  int n = (int)View::PageCount;
  int x = W / 2 - (n - 1) * 8;
  for (int i = 0; i < n; i++) {
    if (i == active) g.circle(x + i * 16, H - 12, 3, C_TEXT);
    else g.circle(x + i * 16, H - 12, 2, C_DIM);
  }
}

static void bar(int x, int y, int w, int h, int pct, uint16_t c) {
  g.rrect(x, y, w, h, h / 2, C_TRACK);
  int fw = constrain(pct, 0, 100) * w / 100;
  if (fw > 0) g.rrect(x, y, max(fw, h), h, h / 2, c);
}

// ---------- screens ----------

static void drawWaiting() {
  g.fill(C_BG);
  header("Claude meter", C_BG);
  int y = land ? 60 : 120;
  g.text("Waiting for", W / 2, y, 4, C_TEXT, C_BG, TC_DATUM);
  g.text("USB data", W / 2, y + 30, 4, C_TEXT, C_BG, TC_DATUM);
  g.text("Run the host script on", W / 2, y + 80, 2, C_MUTED, C_BG, TC_DATUM);
  g.text("your PC to start the feed.", W / 2, y + 98, 2, C_MUTED, C_BG, TC_DATUM);
  char v[24];
  snprintf(v, sizeof v, "fw %s", FW_VERSION);
  g.text(v, W / 2, H - 30, 2, C_DIM, C_BG, TC_DATUM);
}

// 270 degree gauge for the 5-hour window with the number inside.
static void gauge(int cx, int cy) {
  const UsageData &u = g_usage;
  // TFT_eSPI angles: 0 = 6 o'clock, clockwise; the gap is at the bottom.
  const int r = 64, ir = 51;
  const int a0 = 45, a1 = 315;
  g.arc(cx, cy, r, ir, a0, a1, C_TRACK, C_BG, true);
  if (u.sessionPct > 0) {
    int end = a0 + (a1 - a0) * min<int>(u.sessionPct, 100) / 100;
    if (end > a0) g.arc(cx, cy, r, ir, a0, end, levelColor(u.sessionPct), C_BG, true);
  }

  char buf[24];
  if (u.sessionPct >= 0) {
    snprintf(buf, sizeof buf, "%d", u.sessionPct);
    int nw = g.width(buf, 6), pw = g.width("%", 4);
    int x = cx - (nw + pw + 2) / 2;
    g.text(buf, x, cy - 30, 6, C_TEXT, C_BG);
    g.text("%", x + nw + 2, cy - 30 + 20, 4, C_TEXT, C_BG);
  } else {
    g.text("--", cx, cy - 30, 6, C_TEXT, C_BG, TC_DATUM);
  }
  g.text("5-hour session", cx, cy + 22, 2, C_MUTED, C_BG, TC_DATUM);
}

// Label and percentage on one line with a bar underneath, spanning x0..x1.
static void limitRow(const char *label, int pct, int x0, int x1, int y) {
  char buf[8];
  if (pct >= 0) snprintf(buf, sizeof buf, "%d%%", pct);
  else strcpy(buf, "--");
  g.text(label, x0, y, 2, C_MUTED, C_BG);
  g.text(buf, x1, y, 2, C_TEXT, C_BG, TR_DATUM);
  bar(x0, y + 20, x1 - x0, 8, max<int>(pct, 0), levelColor(pct));
}

static void drawMain() {
  const UsageData &u = g_usage;
  g.fill(C_BG);
  header(u.plan[0] ? u.plan : "Claude", C_BG);

  const int cx = W / 2, cy = 112;
  gauge(cx, cy);
  char buf[24];
  if (u.sessionReset && g_clock.synced()) {
    char d[16];
    int32_t left = (int32_t)(u.sessionReset - g_clock.now());
    fmtDuration(d, sizeof d, left);
    snprintf(buf, sizeof buf, "resets in %s", d);
    g.text(buf, cx, 184, 2, C_TEXT, C_BG, TC_DATUM);
  }

  int y = 210;
  limitRow("Weekly, all models", u.weekPct, 12, W - 12, y);

  y += 38;
  if (u.opusPct >= 0) {
    limitRow("Weekly, Opus", u.opusPct, 12, W - 12, y);
    y += 34;
  } else {
    y += 4;
  }

  if (u.weekReset && g_clock.synced()) {
    char when[20];
    fmtWeekdayClock(when, sizeof when, u.weekReset + g_clock.tzOffset);
    g.text("weekly resets", 12, y, 2, C_MUTED, C_BG);
    g.text(when, W - 12, y, 2, C_TEXT, C_BG, TR_DATUM);
  }

  pageDots(0);
}

// Landscape: gauge on the left, weekly limits in a column on the right.
static void drawMainLand() {
  const UsageData &u = g_usage;
  g.fill(C_BG);
  header(u.plan[0] ? u.plan : "Claude", C_BG);

  const int cx = 84, cy = 108;
  gauge(cx, cy);
  char buf[24];
  if (u.sessionReset && g_clock.synced()) {
    char d[16];
    fmtDuration(d, sizeof d, (int32_t)(u.sessionReset - g_clock.now()));
    snprintf(buf, sizeof buf, "resets in %s", d);
    g.text(buf, cx, 180, 2, C_TEXT, C_BG, TC_DATUM);
  }

  const int x0 = 172, x1 = W - 12;
  int y = 44;
  limitRow("Weekly, all", u.weekPct, x0, x1, y);
  y += 44;
  if (u.opusPct >= 0) {
    limitRow("Weekly, Opus", u.opusPct, x0, x1, y);
    y += 44;
  }
  if (u.weekReset && g_clock.synced()) {
    char when[20];
    fmtWeekdayClock(when, sizeof when, u.weekReset + g_clock.tzOffset);
    g.text("weekly resets", x0, y, 2, C_MUTED, C_BG);
    g.text(when, x1, y + 18, 2, C_TEXT, C_BG, TR_DATUM);
  }

  pageDots(0);
}

static void tile(int x, int y, const char *label, const char *value, const char *suffix, int w = 104) {
  g.rrect(x, y, w, 50, 4, C_TRACK);
  g.text(label, x + 8, y + 5, 2, C_MUTED, C_TRACK);
  int vw = g.text(value, x + 8, y + 22, 4, C_TEXT, C_TRACK);
  if (suffix && 8 + vw + 4 + g.width(suffix, 2) <= w)
    g.text(suffix, x + 8 + vw + 4, y + 30, 2, C_GREEN, C_TRACK);
}

// The four stat tiles in a 2x2 grid; w is one tile's width.
static void tiles(int x, int y, int w, int gap) {
  const UsageData &u = g_usage;
  char a[16], b[16], c[16], d[16];
  fmtTokens(a, sizeof a, u.tokens);
  snprintf(b, sizeof b, "$%.2f", u.cost);
  snprintf(c, sizeof c, "%u", u.sessions);
  if (u.cachePct >= 0) snprintf(d, sizeof d, "%d%%", u.cachePct);
  else strcpy(d, "--");
  tile(x, y, "tokens", a, nullptr, w);
  tile(x + w + gap, y, "API equiv", b, nullptr, w);
  tile(x, y + 56, "sessions", c, u.sessions ? "active" : nullptr, w);
  tile(x + w + gap, y + 56, "cache hit", d, nullptr, w);
}

// Tokens per local hour as 24 bars from bx; the label sits at y, the bars below it.
static void hourly(int bx, int y, int bw, int step) {
  const UsageData &u = g_usage;
  g.text("Tokens per hour", bx, y, 2, C_MUTED, C_BG);
  uint32_t peak = 1;
  for (int i = 0; i < 24; i++) peak = max(peak, u.hourly[i]);
  int curHour = -1;
  if (g_clock.synced()) {
    time_t t = g_clock.localNow();
    struct tm tm;
    gmtime_r(&t, &tm);
    curHour = tm.tm_hour;
  }
  const int by = y + 20, bh = 48;
  for (int i = 0; i < 24; i++) {
    int h = (int)((uint64_t)u.hourly[i] * bh / peak);
    int x = bx + i * step;
    if (curHour >= 0 && i > curHour) {
      g.rect(x, by + bh - 3, bw, 3, C_TRACK);
    } else {
      if (h < 2) h = u.hourly[i] ? 2 : 1;
      g.rect(x, by + bh - h, bw, h, u.hourly[i] ? C_CORAL : C_TRACK);
    }
  }
  g.text("0", bx, by + bh + 2, 1, C_DIM, C_BG);
  g.text("12", bx + 12 * step, by + bh + 2, 1, C_DIM, C_BG);
  g.text("23", bx + 23 * step, by + bh + 2, 1, C_DIM, C_BG);
}

static void drawToday() {
  const UsageData &u = g_usage;
  g.fill(C_BG);
  char date[20] = "Today";
  if (g_clock.synced()) fmtDate(date, sizeof date, g_clock.localNow());
  header(date, C_BG);

  tiles(12, 30, 104, 8);
  hourly(12, 146, 8, 9);

  int y = 232;
  for (int i = 0; i < u.modelCount; i++) {
    char p[8];
    snprintf(p, sizeof p, "%u%%", u.models[i].pct);
    g.text(u.models[i].name, 12, y, 2, C_MUTED, C_BG);
    g.text(p, W - 12, y, 2, C_TEXT, C_BG, TR_DATUM);
    y += 18;
  }

  pageDots(1);
}

// Landscape: tiles top left, model shares top right, the hourly chart across the bottom.
static void drawTodayLand() {
  const UsageData &u = g_usage;
  g.fill(C_BG);
  char date[20] = "Today";
  if (g_clock.synced()) fmtDate(date, sizeof date, g_clock.localNow());
  header(date, C_BG);

  tiles(12, 30, 96, 6);

  // The column right of the tiles is narrow, so long model names are shortened to fit.
  const int x0 = 214, x1 = W - 12;
  int y = 34;
  for (int i = 0; i < u.modelCount; i++) {
    char p[8], n[sizeof u.models[i].name];
    snprintf(p, sizeof p, "%u%%", u.models[i].pct);
    strlcpy(n, u.models[i].name, sizeof n);
    int room = x1 - x0 - g.width(p, 2) - 6;
    for (int len = strlen(n); len > 0 && g.width(n, 2) > room;) n[--len] = 0;
    g.text(n, x0, y, 2, C_MUTED, C_BG);
    g.text(p, x1, y, 2, C_TEXT, C_BG, TR_DATUM);
    y += 26;
  }

  hourly(12, 144, 10, 12);
  pageDots(1);
}

static const int BTN_H = 36;
static int btnY() { return H - 50; }  // 270 portrait, 190 landscape

static void drawAlert(const AlertInfo &al) {
  g.fill(C_ALERT_BG);
  g.text("alert", 12, 6, 2, C_ALERT_SUB, C_ALERT_BG);
  if (g_clock.synced()) {
    char c[10];
    fmtClock(c, sizeof c, g_clock.localNow());
    g.text(c, W - 12, 6, 2, C_TEXT, C_ALERT_BG, TR_DATUM);
  }

  char buf[24];
  snprintf(buf, sizeof buf, "%d", al.pct);
  int nw = g.width(buf, 6), pw = g.width("%", 4);
  int x = W / 2 - (nw + pw + 2) / 2;
  // Same stack in both orientations; landscape packs it 80 px tighter.
  const int top = land ? 30 : 52, msg = land ? 86 : 116, rs = land ? 110 : 156;
  g.text(buf, x, top, 6, C_ALERT_FG, C_ALERT_BG);
  g.text("%", x + nw + 2, top + 20, 4, C_ALERT_FG, C_ALERT_BG);

  g.text(al.reached ? "5-hour limit reached" : "5-hour limit almost used",
         W / 2, msg, 2, C_TEXT, C_ALERT_BG, TC_DATUM);

  if (g_usage.sessionReset && g_clock.synced()) {
    g.text("resets at", W / 2, rs, 2, C_ALERT_SUB, C_ALERT_BG, TC_DATUM);
    fmtClock(buf, sizeof buf, g_usage.sessionReset + g_clock.tzOffset);
    g.text(buf, W / 2, rs + 18, 4, C_TEXT, C_ALERT_BG, TC_DATUM);
    char d[16];
    fmtDuration(d, sizeof d, (int32_t)(g_usage.sessionReset - g_clock.now()));
    snprintf(buf, sizeof buf, "%s left", d);
    g.text(buf, W / 2, rs + 48, 2, C_ALERT_SUB, C_ALERT_BG, TC_DATUM);
  }

  // Two buttons splitting the width: 104 px each in portrait, 144 in landscape.
  const int by = btnY(), bw = (W - 32) / 2, x2 = 20 + bw;
  g.rframe(12, by, bw, BTN_H, 4, C_ALERT_FG);
  g.rframe(x2, by, bw, BTN_H, 4, C_ALERT_FG);
  g.text("snooze 30m", 12 + bw / 2, by + BTN_H / 2, 2, C_TEXT, C_ALERT_BG, MC_DATUM);
  g.text("dismiss", x2 + bw / 2, by + BTN_H / 2, 2, C_TEXT, C_ALERT_BG, MC_DATUM);
}

static void drawBack() {
  g.fill(C_OK_BG);
  int y = land ? 76 : 120;
  g.text("You're back", W / 2, y, 4, C_TEXT, C_OK_BG, TC_DATUM);
  g.text("Your 5-hour limit has reset.", W / 2, y + 38, 2, C_OK_SUB, C_OK_BG, TC_DATUM);
  g.text("Tap to continue", W / 2, H - 30, 2, C_OK_SUB, C_OK_BG, TC_DATUM);
}

// ---------- public ----------

void uiBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
#endif
  uiSetBrightness(g_settings.brightness);

  tft.init();
  uiSetRotation(g_settings.rot);  // set from the companion; also allocates the sprite

#ifdef BOOT_SELFTEST
  // Direct draws, no sprite: tells a panel-init problem apart from a sprite problem.
  tft.fillScreen(TFT_RED);   delay(1000);
  tft.fillScreen(TFT_GREEN); delay(1000);
  tft.fillScreen(TFT_BLUE);  delay(1000);
#endif
  tft.fillScreen(C_BG);
}

// Allocates the band sprite for the current W x BAND, replacing one of another shape.
// Both shapes are 76800 bytes at 16 bit, so the freed block fits the new one.
static void makeSprite() {
  if (spr.created()) {
    if (spr.width() == W && spr.height() == BAND) return;
    spr.deleteSprite();
  }
  size_t freeBefore = ESP.getFreeHeap(), largest = ESP.getMaxAllocHeap();
  spr.setColorDepth(16);
  void *buf = spr.createSprite(W, BAND);
  int depth = 16;
  if (!buf) {
    // Not enough heap: fall back to a smaller depth rather than a blank screen.
    spr.setColorDepth(8);
    buf = spr.createSprite(W, BAND);
    depth = buf ? 8 : 0;
  }
  Serial.printf("{\"type\":\"diag\",\"heap\":%u,\"largest\":%u,\"sprite_depth\":%d,\"w\":%d,\"h\":%d}\n",
                (unsigned)freeBefore, (unsigned)largest, depth, W, BAND);
}

static uint8_t level = 0;   // backlight level the rest of the firmware asked for

static void backlight(uint8_t l) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, l);
#else
  ledcWrite(0, l);
#endif
}

void uiSetBrightness(uint8_t l) {
  level = l;
  backlight(l);
}

void uiSetRotation(uint8_t rot) {
  rot &= 3;
  tft.setRotation(rot);
  land = rot & 1;
  W = land ? 320 : 240;
  H = land ? 240 : 320;
  BAND = H / 2;
  makeSprite();
}

static void drawBand(View v, const AlertInfo &alert, int band) {
  g.y0 = band;
  switch (v) {
    case View::Alert: drawAlert(alert); break;
    case View::Back: drawBack(); break;
    case View::Today:
      if (!g_usage.valid) drawWaiting();
      else if (land) drawTodayLand();
      else drawToday();
      break;
    default:
      if (!g_usage.valid) drawWaiting();
      else if (land) drawMainLand();
      else drawMain();
      break;
  }
}

static void fadeBacklight(bool down) {
  const int STEPS = 8;
  for (int i = 1; i <= STEPS; i++) {
    int k = down ? STEPS - i : i;
    backlight(level * k / STEPS);
    delay(14);
  }
}

void uiRender(View v, const AlertInfo &alert, Fx fx) {
  if (fx == Fx::Fade) fadeBacklight(true);

  for (int band = 0; band < H; band += BAND) {
    drawBand(v, alert, band);
    if (fx == Fx::Wipe) {
      // Reveal the new screen top to bottom in strips from the already drawn band.
      const int STRIP = 16;
      for (int sy = 0; sy < BAND; sy += STRIP) {
        // 160 is a multiple of 16 but 120 is not: the last landscape strip is shorter.
        spr.pushSprite(0, band + sy, 0, sy, W, min(STRIP, BAND - sy));
        delay(9);
      }
    } else {
      spr.pushSprite(0, band);
    }
  }

  if (fx == Fx::Fade) fadeBacklight(false);
}

int uiAlertButtonAt(int x, int y) {
  if (y < btnY() - 10 || y > btnY() + BTN_H + 10) return 0;
  return x < W / 2 ? 1 : 2;
}
