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
  // Anti-aliased line with round ends, blended against bg.
  void wline(float xa, float ya, float xb, float yb, float w, uint16_t c, uint16_t bg) {
    spr.drawWideLine(xa, ya - y0, xb, yb - y0, w, c, bg);
  }
  void tri(int x0, int y0_, int x1, int y1, int x2, int y2, uint16_t c) {
    spr.fillTriangle(x0, y0_ - y0, x1, y1 - y0, x2, y2 - y0, c);
  }
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

// ---------- nav bar: [<] [>]  page dots  [play/pause] ----------
//
// Along the bottom of every pageable screen. Content must stay above navTop().

static const int NAV_H = 28, NAV_BW = 44;
static int navTop() { return H - NAV_H - 2; }       // 290 portrait, 210 landscape
static int navPrevX() { return 8; }
static int navNextX() { return 8 + NAV_BW + 6; }
static int navPlayX() { return W - 8 - NAV_BW; }

static void navButton(int x, uint16_t bg) {
  g.rrect(x, navTop(), NAV_BW, NAV_H, 6, bg);
}

static void navBar(int active) {
  const int y = navTop(), cy = y + NAV_H / 2;
  int x = navPrevX();
  navButton(x, C_TRACK);
  g.tri(x + 27, cy - 7, x + 27, cy + 7, x + 16, cy, C_TEXT);           // <
  x = navNextX();
  navButton(x, C_TRACK);
  g.tri(x + 17, cy - 7, x + 17, cy + 7, x + 28, cy, C_TEXT);           // >
  x = navPlayX();
  navButton(x, C_TRACK);
  if (g_ui.playing) {                                                  // || = running, tap pauses
    g.rect(x + 16, cy - 7, 4, 14, C_TEXT);
    g.rect(x + 24, cy - 7, 4, 14, C_TEXT);
  } else {                                                             // > = paused, tap plays
    g.tri(x + 17, cy - 8, x + 17, cy + 8, x + 30, cy, C_GREEN);
  }

  // Page dots centered in the gap between the left pair and the play button.
  const int n = (int)View::PageCount;
  const int mid = (navNextX() + NAV_BW + navPlayX()) / 2;
  const int dx = mid - (n - 1) * 7;
  for (int i = 0; i < n; i++) {
    if (i == active) g.circle(dx + i * 14, cy, 3, C_TEXT);
    else g.circle(dx + i * 14, cy, 2, C_DIM);
  }
}

static Hit navHit(int x, int y) {
  if (y < navTop() - 6) return Hit::None;          // a little slack above the buttons
  if (x >= navPrevX() - 6 && x < navPrevX() + NAV_BW + 3) return Hit::Prev;
  if (x >= navNextX() - 3 && x < navNextX() + NAV_BW + 6) return Hit::Next;
  if (x >= navPlayX() - 6) return Hit::PlayPause;
  return Hit::None;
}

static void bar(int x, int y, int w, int h, int pct, uint16_t c) {
  g.rrect(x, y, w, h, h / 2, C_TRACK);
  int fw = constrain(pct, 0, 100) * w / 100;
  if (fw > 0) g.rrect(x, y, max(fw, h), h, h / 2, c);
}

// ---------- screens ----------

static void drawWaiting(int page) {
  g.fill(C_BG);
  header("Claude meter", C_BG);
  int y = land ? 50 : 100;
  g.text("Waiting for", W / 2, y, 4, C_TEXT, C_BG, TC_DATUM);
  g.text("USB data", W / 2, y + 30, 4, C_TEXT, C_BG, TC_DATUM);
  g.text("Run the host script on", W / 2, y + 80, 2, C_MUTED, C_BG, TC_DATUM);
  g.text("your PC to start the feed.", W / 2, y + 98, 2, C_MUTED, C_BG, TC_DATUM);
  char v[24];
  snprintf(v, sizeof v, "fw %s", FW_VERSION);
  g.text(v, W / 2, navTop() - 24, 2, C_DIM, C_BG, TC_DATUM);
  navBar(page);
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
  char buf[12];
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

  const int cx = W / 2, cy = 104;
  gauge(cx, cy);
  char buf[24];
  if (u.sessionReset && g_clock.synced()) {
    char d[16];
    int32_t left = (int32_t)(u.sessionReset - g_clock.now());
    fmtDuration(d, sizeof d, left);
    snprintf(buf, sizeof buf, "resets in %s", d);
    g.text(buf, cx, 172, 2, C_TEXT, C_BG, TC_DATUM);
  }

  int y = 196;
  limitRow("Weekly, all models", u.weekPct, 12, W - 12, y);

  y += 34;
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

  navBar(0);
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

  navBar(0);
}

static void tile(int x, int y, const char *label, const char *value, const char *suffix, int w = 104) {
  g.rrect(x, y, w, 50, 4, C_TRACK);
  g.text(label, x + 8, y + 5, 2, C_MUTED, C_TRACK);
  // Narrow landscape tiles can't fit a long value like "$123.45" in font 4.
  const bool big = g.width(value, 4) <= w - 16;
  int vw = g.text(value, x + 8, y + (big ? 22 : 28), big ? 4 : 2, C_TEXT, C_TRACK);
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

// Tokens per local hour as 24 bars from bx; the label sits at y, bh-tall bars below it.
static void hourly(int bx, int y, int bw, int step, int bh) {
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
  const int by = y + 20;
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
  hourly(12, 142, 8, 9, 44);

  int y = 222;
  for (int i = 0; i < u.modelCount; i++) {
    char p[8];
    snprintf(p, sizeof p, "%u%%", u.models[i].pct);
    g.text(u.models[i].name, 12, y, 2, C_MUTED, C_BG);
    g.text(p, W - 12, y, 2, C_TEXT, C_BG, TR_DATUM);
    y += 16;
  }

  navBar(1);
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

  hourly(12, 140, 10, 12, 36);
  navBar(1);
}

// ---------- clock ----------

// Seconds through the minute as a bar, centered at y.
static void secondsBar(int y, int w, int sec) {
  const int x = (W - w) / 2;
  g.rrect(x, y, w, 6, 3, C_TRACK);
  int fw = w * sec / 59;
  if (fw > 0) g.rrect(x, y, max(fw, 6), 6, 3, C_CORAL);
}

// Font 8 has only digits and ":-.", so am/pm goes in font 4 below.
static void drawClockDigital(const struct tm &tm) {
  int hour = tm.tm_hour;
  const char *ampm = nullptr;
  if (!g_settings.h24) {
    ampm = hour < 12 ? "am" : "pm";
    hour = hour % 12 ? hour % 12 : 12;
  }
  char hh[4], mm[4];
  snprintf(hh, sizeof hh, g_settings.h24 ? "%02d" : "%d", hour);
  snprintf(mm, sizeof mm, "%02d", tm.tm_min);

  if (land) {
    // One line, HH:MM, with the seconds bar and am/pm underneath.
    char t[8];
    snprintf(t, sizeof t, "%s:%s", hh, mm);
    g.text(t, W / 2, 44, 8, C_TEXT, C_BG, TC_DATUM);
    secondsBar(138, 220, tm.tm_sec);
    if (ampm) g.text(ampm, W / 2, 158, 4, C_MUTED, C_BG, TC_DATUM);
  } else {
    // Portrait is too narrow for HH:MM at this size: hours over minutes.
    g.text(hh, W / 2, 40, 8, C_TEXT, C_BG, TC_DATUM);
    g.text(mm, W / 2, 128, 8, C_CORAL, C_BG, TC_DATUM);
    secondsBar(222, 160, tm.tm_sec);
    if (ampm) g.text(ampm, W / 2, 240, 4, C_MUTED, C_BG, TC_DATUM);
  }
}

static void drawClockAnalog(const struct tm &tm) {
  const int cx = W / 2, cy = land ? 116 : 152, r = land ? 88 : 100;
  g.arc(cx, cy, r, r - 3, 0, 360, C_TRACK, C_BG, false);

  // Ticks: long at the hours, dots between.
  for (int i = 0; i < 60; i++) {
    const float a = i * 6 * DEG_TO_RAD, sx = sinf(a), cy_ = -cosf(a);
    if (i % 5 == 0) {
      const float w = i % 15 == 0 ? 4 : 2.5f, in = r - (i % 15 == 0 ? 18 : 14);
      g.wline(cx + sx * in, cy + cy_ * in, cx + sx * (r - 8), cy + cy_ * (r - 8), w, C_MUTED, C_BG);
    } else {
      g.circle(cx + (int)roundf(sx * (r - 9)), cy + (int)roundf(cy_ * (r - 9)), 1, C_DIM);
    }
  }

  // Hands move smoothly: the hour hand with the minutes, the minute hand with the seconds.
  const float sec = tm.tm_sec, min = tm.tm_min + sec / 60.0f, hr = (tm.tm_hour % 12) + min / 60.0f;
  auto hand = [&](float turns, float len, float tail, float w, uint16_t c) {
    const float a = turns * TWO_PI, sx = sinf(a), sy = -cosf(a);
    g.wline(cx - sx * tail, cy - sy * tail, cx + sx * len, cy + sy * len, w, c, C_BG);
  };
  hand(hr / 12.0f, r * 0.50f, 0, 7, C_TEXT);
  hand(min / 60.0f, r * 0.78f, 0, 4, C_TEXT);
  hand(sec / 60.0f, r * 0.86f, 16, 2, C_CORAL);
  g.circle(cx, cy, 5, C_CORAL);
  g.circle(cx, cy, 2, C_BG);
}

// The displayed hour (12-hour when that's set), and "am"/"pm" or null.
static int clockHour(const struct tm &tm, const char **ampm) {
  *ampm = nullptr;
  if (g_settings.h24) return tm.tm_hour;
  *ampm = tm.tm_hour < 12 ? "am" : "pm";
  return tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
}

// Arc from 12 o'clock clockwise over frac of a turn. TFT_eSPI's 0 is 6 o'clock, so
// 12 o'clock is 180 and an arc past 6 o'clock again is drawn in two pieces.
static void arcFrom12(int cx, int cy, int r, int ir, float frac, uint16_t c) {
  const int span = (int)(constrain(frac, 0.0f, 1.0f) * 360);
  if (span < 1) return;
  const int end = 180 + span;
  g.arc(cx, cy, r, ir, 180, min(end, 360), c, C_BG, false);
  if (end > 360) g.arc(cx, cy, r, ir, 0, end - 360, c, C_BG, false);
}

// Rings: hours, minutes, seconds as concentric arcs filling clockwise, time in the middle.
static void drawClockRings(const struct tm &tm) {
  const int cx = land ? 104 : W / 2, cy = land ? 116 : 150, r = land ? 88 : 104;
  const float mins = tm.tm_min + tm.tm_sec / 60.0f;
  const float hours = g_settings.h24 ? (tm.tm_hour + mins / 60) / 24 : ((tm.tm_hour % 12) + mins / 60) / 12;
  struct Ring { float frac; uint16_t c; int w; const char *label; int value; };
  const char *ampm;
  const int hour = clockHour(tm, &ampm);
  const Ring rings[3] = {{hours, C_CORAL, 10, "hours", hour},
                         {mins / 60, C_GREEN, 8, "minutes", tm.tm_min},
                         {tm.tm_sec / 60.0f, C_AMBER, 6, "seconds", tm.tm_sec}};
  int rr = r;
  for (const Ring &ring : rings) {
    g.arc(cx, cy, rr, rr - ring.w, 0, 360, C_TRACK, C_BG, false);
    arcFrom12(cx, cy, rr, rr - ring.w, ring.frac, ring.c);
    rr -= ring.w + 5;
  }

  char t[8];
  snprintf(t, sizeof t, g_settings.h24 ? "%02d:%02d" : "%d:%02d", hour, tm.tm_min);
  if (land) {
    // The middle is too small for font 6 here; a legend on the right carries the detail.
    g.text(t, cx, cy, 4, C_TEXT, C_BG, MC_DATUM);
    if (ampm) g.text(ampm, cx, cy + 20, 2, C_MUTED, C_BG, TC_DATUM);
    int y = 48;
    for (const Ring &ring : rings) {
      char v[4];
      snprintf(v, sizeof v, "%02d", ring.value);
      g.circle(222, y + 8, 5, ring.c);
      g.text(ring.label, 234, y, 2, C_MUTED, C_BG);
      g.text(v, 234, y + 18, 4, C_TEXT, C_BG);
      y += 52;
    }
  } else {
    g.text(t, cx, cy, 6, C_TEXT, C_BG, MC_DATUM);
    if (ampm) g.text(ampm, cx, cy + 30, 2, C_MUTED, C_BG, TC_DATUM);
  }
}

// One flip-clock card: the digits, a split across the middle, hinge notches at the sides.
static void flipCard(int x, int y, int w, int h, const char *s) {
  g.rrect(x, y, w, h, 10, C_TRACK);
  g.text(s, x + w / 2, y + h / 2 + 2, 8, C_TEXT, C_TRACK, MC_DATUM);
  g.rect(x, y + h / 2 - 1, w, 2, C_BG);
  g.circle(x, y + h / 2, 4, C_BG);
  g.circle(x + w - 1, y + h / 2, 4, C_BG);
}

static void drawClockFlip(const struct tm &tm) {
  const char *ampm;
  const int hour = clockHour(tm, &ampm);
  char hh[4], mm[4];
  snprintf(hh, sizeof hh, g_settings.h24 ? "%02d" : "%d", hour);
  snprintf(mm, sizeof mm, "%02d", tm.tm_min);
  int x, y, w, h;
  if (land) {
    x = 14; y = 40; w = 140; h = 112;
    flipCard(x, y, w, h, hh);
    flipCard(W - 14 - w, y, w, h, mm);
    secondsBar(172, W - 28, tm.tm_sec);
  } else {
    w = 170; h = 100; x = (W - w) / 2; y = 34;
    flipCard(x, y, w, h, hh);
    flipCard(x, y + h + 10, w, h, mm);
    secondsBar(258, w, tm.tm_sec);
  }
  if (ampm) g.text(ampm, x + w - 12, y + 8, 2, C_MUTED, C_TRACK, TR_DATUM);
}

// Words: a word-clock grid, the phrase for the time lit and the rest faint. The four dots
// underneath count the minutes past the last five.
enum : uint8_t {
  WD_IT, WD_IS, WD_HALF, WD_TEN_M, WD_QUARTER, WD_TWENTY, WD_FIVE_M, WD_MINUTES, WD_TO, WD_PAST,
  WD_H1, WD_H2, WD_H3, WD_H4, WD_H5, WD_H6, WD_H7, WD_H8, WD_H9, WD_H10, WD_H11, WD_H12,
  WD_OCLOCK, WD_COUNT, WD_END = 0xFF
};
static const char *const WORD_TEXT[WD_COUNT] = {
  "it", "is", "half", "ten", "quarter", "twenty", "five", "minutes", "to", "past",
  "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
  "o'clock"};
// Lines of word ids for each orientation, WD_END-terminated.
static const uint8_t WORDS_PORTRAIT[][5] = {
  {WD_IT, WD_IS, WD_HALF, WD_TEN_M, WD_END}, {WD_QUARTER, WD_TWENTY, WD_END},
  {WD_FIVE_M, WD_MINUTES, WD_TO, WD_END},     {WD_PAST, WD_H1, WD_H3, WD_END},
  {WD_H2, WD_H4, WD_H5, WD_END},              {WD_H6, WD_H7, WD_H8, WD_END},
  {WD_H9, WD_H10, WD_H11, WD_END},            {WD_H12, WD_OCLOCK, WD_END}};
static const uint8_t WORDS_LAND[][6] = {
  {WD_IT, WD_IS, WD_HALF, WD_TEN_M, WD_QUARTER, WD_END}, {WD_TWENTY, WD_FIVE_M, WD_MINUTES, WD_TO, WD_END},
  {WD_PAST, WD_H1, WD_H2, WD_H3, WD_H4, WD_END},         {WD_H5, WD_H6, WD_H7, WD_H8, WD_END},
  {WD_H9, WD_H10, WD_H11, WD_H12, WD_END},               {WD_OCLOCK, WD_END}};

static void drawClockWords(const struct tm &tm) {
  bool lit[WD_COUNT] = {};
  lit[WD_IT] = lit[WD_IS] = true;
  const int m5 = tm.tm_min / 5;
  int hour = tm.tm_hour % 12;
  if (m5 >= 7) hour = (hour + 1) % 12;   // from :35 on it's "... to" the next hour
  // The minute words for each five minutes, :00 to :55.
  static const uint8_t phrase[12][4] = {
    {WD_OCLOCK, WD_END},                           {WD_FIVE_M, WD_MINUTES, WD_PAST, WD_END},
    {WD_TEN_M, WD_PAST, WD_END},                   {WD_QUARTER, WD_PAST, WD_END},
    {WD_TWENTY, WD_MINUTES, WD_PAST, WD_END},      {WD_TWENTY, WD_FIVE_M, WD_MINUTES, WD_PAST},
    {WD_HALF, WD_PAST, WD_END},                    {WD_TWENTY, WD_FIVE_M, WD_MINUTES, WD_TO},
    {WD_TWENTY, WD_MINUTES, WD_TO, WD_END},        {WD_QUARTER, WD_TO, WD_END},
    {WD_TEN_M, WD_TO, WD_END},                     {WD_FIVE_M, WD_MINUTES, WD_TO, WD_END}};
  for (uint8_t id : phrase[m5]) {
    if (id == WD_END) break;
    lit[id] = true;
  }
  const int hourWord = WD_H1 + (hour + 11) % 12;   // hour 0 reads "twelve"
  lit[hourWord] = true;

  const int lines = land ? 6 : 8;
  auto lineAt = [&](int i) -> const uint8_t * { return land ? WORDS_LAND[i] : WORDS_PORTRAIT[i]; };
  // Font 4 if every line fits the width, otherwise font 2 for all of them.
  int font = 4;
  for (int i = 0; i < lines && font == 4; i++) {
    int w = 0;
    for (const uint8_t *p = lineAt(i); *p != WD_END; p++) w += g.width(WORD_TEXT[*p], 4) + g.width(" ", 4);
    if (w > W - 16) font = 2;
  }
  const int lh = font == 4 ? (land ? 26 : 27) : 20, top = land ? 30 : 34, gap = g.width(" ", font);
  const uint16_t faint = rgb(62, 62, 58);
  for (int i = 0; i < lines; i++) {
    const uint8_t *line = lineAt(i);
    int w = -gap;
    for (const uint8_t *p = line; *p != WD_END; p++) w += g.width(WORD_TEXT[*p], font) + gap;
    int x = (W - w) / 2;
    for (const uint8_t *p = line; *p != WD_END; p++) {
      const uint16_t c = !lit[*p] ? faint : *p == hourWord ? C_CORAL : C_TEXT;
      x += g.text(WORD_TEXT[*p], x, top + i * lh, font, c, C_BG) + gap;
    }
  }
  const int dy = top + lines * lh + (land ? 8 : 12), extra = tm.tm_min % 5;
  for (int i = 0; i < 4; i++) g.circle(W / 2 - 27 + i * 18, dy, 3, i < extra ? C_CORAL : C_TRACK);
}

// Binary: BCD columns for HH MM SS, 8-4-2-1 from the top, the digits underneath.
static void drawClockBinary(const struct tm &tm) {
  const char *ampm;
  const int hour = clockHour(tm, &ampm);
  const int digits[6] = {hour / 10, hour % 10, tm.tm_min / 10, tm.tm_min % 10, tm.tm_sec / 10, tm.tm_sec % 10};
  static const uint8_t bits[6] = {2, 4, 3, 4, 3, 4};   // a tens digit never needs all four
  const uint16_t colors[3] = {C_CORAL, C_GREEN, C_AMBER};
  const int sp = land ? 44 : 34, pair = land ? 14 : 10, r = land ? 13 : 11;
  const int x0 = (W - (5 * sp + 2 * pair)) / 2, top = land ? 46 : 58, rs = land ? 34 : 36;
  for (int i = 0; i < 6; i++) {
    const int x = x0 + i * sp + (i / 2) * pair;
    for (int b = 0; b < bits[i]; b++) {
      const bool on = digits[i] & (1 << b);
      g.circle(x, top + (3 - b) * rs, r, on ? colors[i / 2] : C_TRACK);
    }
    char d[2] = {(char)('0' + digits[i]), 0};
    g.text(d, x, top + 3 * rs + r + 10, 4, C_MUTED, C_BG, TC_DATUM);
  }
  // The hours-tens column has only 2 bits, so its 8 and 4 rows are empty: am/pm goes there.
  if (ampm) g.text(ampm, x0, top + rs / 2, 2, C_MUTED, C_BG, MC_DATUM);
}

static void drawClock() {
  g.fill(C_BG);
  if (!g_clock.synced()) {
    header("Clock", C_BG);
    g.text("Waiting for the time", W / 2, H / 2 - 30, 2, C_MUTED, C_BG, TC_DATUM);
    g.text("from the PC", W / 2, H / 2 - 12, 2, C_MUTED, C_BG, TC_DATUM);
    navBar((int)View::Clock);
    return;
  }
  char date[20];
  fmtDate(date, sizeof date, g_clock.localNow());
  header(date, C_BG);
  time_t t = g_clock.localNow();
  struct tm tm;
  gmtime_r(&t, &tm);
  switch (g_settings.clock) {
    case ClockStyle::Analog: drawClockAnalog(tm); break;
    case ClockStyle::Rings: drawClockRings(tm); break;
    case ClockStyle::Flip: drawClockFlip(tm); break;
    case ClockStyle::Words: drawClockWords(tm); break;
    case ClockStyle::Binary: drawClockBinary(tm); break;
    default: drawClockDigital(tm); break;
  }
  navBar((int)View::Clock);
}

// ---------- calendar ----------

// Days since 1970-01-01 for a civil date (H. Hinnant's algorithm), m 1..12.
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int daysInMonth(int y, int m) {
  static const uint8_t n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : n[m - 1];
}

// Grid geometry, shared by drawing and hit-testing. Six rows always fit above the nav bar.
struct CalGeo { int x0, cw, titleY, labelY, gridY, rh; };
static CalGeo calGeo() {
  return land ? CalGeo{20, 40, 26, 56, 74, 22} : CalGeo{15, 30, 30, 64, 84, 32};
}

static void drawCalendar() {
  g.fill(C_BG);
  if (!g_clock.synced()) {
    header("Calendar", C_BG);
    g.text("Waiting for the time", W / 2, H / 2 - 30, 2, C_MUTED, C_BG, TC_DATUM);
    g.text("from the PC", W / 2, H / 2 - 12, 2, C_MUTED, C_BG, TC_DATUM);
    navBar((int)View::Calendar);
    return;
  }
  char date[20];
  fmtDate(date, sizeof date, g_clock.localNow());
  header(date, C_BG);

  time_t t = g_clock.localNow();
  struct tm now;
  gmtime_r(&t, &now);
  int ym = (now.tm_year + 1900) * 12 + now.tm_mon + g_ui.monthOffset;
  const int year = ym / 12, month = ym % 12 + 1;
  const bool thisMonth = g_ui.monthOffset == 0;

  static const char *mon[] = {"January", "February", "March", "April", "May", "June", "July",
                              "August", "September", "October", "November", "December"};
  const CalGeo c = calGeo();
  char title[24];
  snprintf(title, sizeof title, "%s %d", mon[month - 1], year);
  // Amber title = not the current month; tapping its middle comes back.
  g.text(title, W / 2, c.titleY, 4, thisMonth ? C_TEXT : C_AMBER, C_BG, TC_DATUM);
  const int ay = c.titleY + 12;
  g.tri(12, ay, 22, ay - 7, 22, ay + 7, C_MUTED);
  g.tri(W - 12, ay, W - 22, ay - 7, W - 22, ay + 7, C_MUTED);

  static const char *wd[] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int i = 0; i < 7; i++)
    g.text(wd[i], c.x0 + i * c.cw + c.cw / 2, c.labelY, 2, C_DIM, C_BG, TC_DATUM);

  const int first = (daysFromCivil(year, month, 1) % 7 + 11) % 7;  // 1970-01-01 was a Thursday
  const int n = daysInMonth(year, month);
  const int r = min(c.rh, c.cw) / 2 - 2;
  for (int d = 1; d <= n; d++) {
    const int cell = first + d - 1, col = cell % 7, row = cell / 7;
    const int cx = c.x0 + col * c.cw + c.cw / 2, cy = c.gridY + row * c.rh + c.rh / 2;
    char s[4];
    snprintf(s, sizeof s, "%d", d);
    if (thisMonth && d == now.tm_mday) {
      g.circle(cx, cy, r, C_CORAL);
      g.text(s, cx, cy, 2, C_TEXT, C_CORAL, MC_DATUM);
    } else {
      g.text(s, cx, cy, 2, col == 0 || col == 6 ? C_DIM : C_TEXT, C_BG, MC_DATUM);
    }
  }
  navBar((int)View::Calendar);
}

// ---------- launcher ----------

static void appRect(int i, int &x, int &y, int &w, int &h) {
  const int cols = land ? 4 : 2, gap = land ? 6 : 8;
  w = (W - 24 - (cols - 1) * gap) / cols;
  h = land ? 80 : 56;
  x = 12 + (i % cols) * (w + gap);
  y = 30 + (i / cols) * (h + 6);
}

static uint16_t appColor(int i) {
  static const uint16_t c[] = {C_CORAL, C_GREEN, rgb(55, 138, 221), C_AMBER,
                               rgb(127, 119, 221), rgb(212, 83, 126), rgb(29, 158, 117), C_DIM};
  return c[i % 8];
}

static void drawLauncher() {
  g.fill(C_BG);
  header("Apps", C_BG);
  if (g_appCount == 0) {
    g.text("No apps yet", W / 2, H / 2 - 50, 4, C_TEXT, C_BG, TC_DATUM);
    g.text("Add them on the companion's", W / 2, H / 2 - 10, 2, C_MUTED, C_BG, TC_DATUM);
    g.text("settings page.", W / 2, H / 2 + 8, 2, C_MUTED, C_BG, TC_DATUM);
    navBar((int)View::Launcher);
    return;
  }
  for (int i = 0; i < g_appCount; i++) {
    int x, y, w, h;
    appRect(i, x, y, w, h);
    const bool pressed = i == g_ui.pressedApp;
    const uint16_t bg = pressed ? C_CORAL : C_TRACK;
    g.rrect(x, y, w, h, 6, bg);

    // A coloured disc with the first letter, the name underneath.
    const int r = land ? 16 : 12, cx = x + w / 2, cy = y + h / 2 - (land ? 10 : 9);
    const uint16_t disc = pressed ? C_TEXT : appColor(i);
    g.circle(cx, cy, r, disc);
    char letter[2] = {(char)toupper((unsigned char)g_apps[i].name[0]), 0};
    g.text(letter, cx, cy + 1, land ? 4 : 2, C_BG, disc, MC_DATUM);

    char name[sizeof g_apps[i].name];
    strlcpy(name, g_apps[i].name, sizeof name);
    for (int len = strlen(name); len > 0 && g.width(name, 2) > w - 8;) name[--len] = 0;
    g.text(name, cx, y + h - 19, 2, C_TEXT, bg, TC_DATUM);
  }
  navBar((int)View::Launcher);
}

// ---------- overlays ----------

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
    case View::Clock: drawClock(); break;
    case View::Calendar: drawCalendar(); break;
    case View::Launcher: drawLauncher(); break;
    case View::Today:
      if (!g_usage.valid) drawWaiting((int)v);
      else if (land) drawTodayLand();
      else drawToday();
      break;
    default:
      if (!g_usage.valid) drawWaiting((int)View::Main);
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

HitResult uiHitTest(View v, int x, int y) {
  HitResult r;
  if (v == View::Alert) {
    if (y >= btnY() - 10 && y <= btnY() + BTN_H + 10) r.kind = x < W / 2 ? Hit::Snooze : Hit::Dismiss;
    return r;
  }
  if (v == View::Back) return r;

  r.kind = navHit(x, y);
  if (r.kind != Hit::None) return r;

  if (v == View::Calendar && g_clock.synced()) {
    const CalGeo c = calGeo();
    if (y >= c.titleY - 8 && y < c.labelY)
      r.kind = x < W / 3 ? Hit::MonthPrev : x >= W * 2 / 3 ? Hit::MonthNext : Hit::MonthToday;
  } else if (v == View::Launcher) {
    for (int i = 0; i < g_appCount; i++) {
      int ax, ay, aw, ah;
      appRect(i, ax, ay, aw, ah);
      if (x >= ax && x < ax + aw && y >= ay && y < ay + ah) {
        r.kind = Hit::App;
        r.index = i;
        break;
      }
    }
  }
  return r;
}
