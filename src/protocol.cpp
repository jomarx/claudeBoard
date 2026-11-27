#include "protocol.h"
#include "model.h"
#include <ArduinoJson.h>

static char lineBuf[2048];
static size_t lineLen = 0;
static bool overflow = false;

void protocolBegin() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
}

void protocolSendHello() {
  Serial.printf("{\"type\":\"hello\",\"fw\":\"%s\",\"w\":240,\"h\":320,\"cfg\":true,\"apps\":true}\n", FW_VERSION);
}

static const char *FX_NAMES[] = {"none", "wipe", "fade"};
static const char *CLOCK_NAMES[CLOCK_STYLES] = {"digital", "analog", "rings", "flip", "words", "binary"};

void protocolSendSettings() {
  const Settings &s = g_settings;
  Serial.printf("{\"type\":\"cfg\",\"brightness\":%u,\"alert1\":%u,\"alert2\":%u,\"h24\":%s,"
                "\"rot\":%u,\"cycle\":%u,\"fx\":\"%s\",\"dim\":%u,\"clock\":\"%s\"}\n",
                s.brightness, s.alert1, s.alert2, s.h24 ? "true" : "false",
                s.rot, s.cycle, FX_NAMES[(int)s.fx], s.dim, CLOCK_NAMES[(int)s.clock]);
}

void protocolSendEvent(const char *name, int value) {
  Serial.printf("{\"type\":\"event\",\"name\":\"%s\",\"value\":%d}\n", name, value);
}

static void sendErr(const char *msg) {
  JsonDocument d;
  d["type"] = "err";
  d["msg"] = msg;
  serializeJson(d, Serial);
  Serial.println();
}

static void readLimit(JsonObjectConst o, int16_t &pct, uint32_t *reset) {
  if (o.isNull()) return;
  if (o["pct"].is<float>()) pct = constrain((int)roundf(o["pct"].as<float>()), -1, 999);
  if (reset && o["reset"].is<uint32_t>()) *reset = o["reset"].as<uint32_t>();
}

static void applyUsage(JsonObjectConst d) {
  UsageData &u = g_usage;

  if (d["now"].is<uint32_t>()) {
    g_clock.epochAtSync = d["now"].as<uint32_t>();
    g_clock.millisAtSync = millis();
  }
  if (d["tz"].is<int32_t>()) g_clock.tzOffset = d["tz"].as<int32_t>();
  if (d["plan"].is<const char *>()) strlcpy(u.plan, d["plan"], sizeof(u.plan));

  readLimit(d["session"], u.sessionPct, &u.sessionReset);
  readLimit(d["week"], u.weekPct, &u.weekReset);
  readLimit(d["opus"], u.opusPct, nullptr);  // {"pct":-1} hides the Opus bar

  JsonObjectConst t = d["today"];
  if (!t.isNull()) {
    if (t["tokens"].is<uint64_t>()) u.tokens = t["tokens"].as<uint64_t>();
    if (t["cost"].is<float>()) u.cost = t["cost"].as<float>();
    if (t["sessions"].is<int>()) u.sessions = constrain(t["sessions"].as<int>(), 0, 255);
    if (t["cache"].is<float>()) u.cachePct = constrain((int)roundf(t["cache"].as<float>()), 0, 100);
    JsonArrayConst h = t["hourly"];
    if (!h.isNull()) {
      for (int i = 0; i < 24; i++) u.hourly[i] = i < (int)h.size() ? h[i].as<uint32_t>() : 0;
    }
    JsonArrayConst m = t["models"];
    if (!m.isNull()) {
      u.modelCount = 0;
      for (JsonObjectConst e : m) {
        if (u.modelCount >= 4) break;
        ModelShare &s = u.models[u.modelCount++];
        strlcpy(s.name, e["n"] | "?", sizeof(s.name));
        s.pct = constrain(e["p"] | 0, 0, 100);
      }
    }
  }

  u.valid = true;
  g_lastFrameMs = millis();
  if (g_lastFrameMs == 0) g_lastFrameMs = 1;
}

static void applyConfig(JsonObjectConst d) {
  if (d["brightness"].is<int>()) g_settings.brightness = constrain(d["brightness"].as<int>(), 5, 255);
  if (d["alert1"].is<int>()) g_settings.alert1 = constrain(d["alert1"].as<int>(), 1, 100);
  if (d["alert2"].is<int>()) g_settings.alert2 = constrain(d["alert2"].as<int>(), 1, 100);
  if (d["h24"].is<bool>()) g_settings.h24 = d["h24"];
  if (d["rot"].is<int>()) {
    int r = d["rot"].as<int>();
    g_settings.rot = (r >= 0 && r <= 3) ? r : 0;
  }
  if (d["cycle"].is<int>()) g_settings.cycle = constrain(d["cycle"].as<int>(), 0, 3600);
  if (d["dim"].is<int>()) g_settings.dim = constrain(d["dim"].as<int>(), 0, 3600);
  const char *fx = d["fx"];
  if (fx) {
    for (int i = 0; i < 3; i++)
      if (!strcmp(fx, FX_NAMES[i])) g_settings.fx = (Fx)i;
  }
  const char *clock = d["clock"];
  if (clock) {
    for (int i = 0; i < CLOCK_STYLES; i++)
      if (!strcmp(clock, CLOCK_NAMES[i])) g_settings.clock = (ClockStyle)i;
  }
}

static void handleLine(const char *line, Requests &req) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, line);
  if (e) {
    sendErr(e.c_str());
    return;
  }
  const char *type = doc["type"] | "";

  if (!strcmp(type, "usage")) {
    applyUsage(doc.as<JsonObjectConst>());
    Serial.printf("{\"type\":\"ack\",\"seq\":%lu}\n", (unsigned long)(doc["seq"] | 0UL));
    req.usage = true;
    return;
  }
  if (!strcmp(type, "cfg")) {
    if (!req.config) req.before = g_settings;
    applyConfig(doc.as<JsonObjectConst>());
    req.config = true;
    protocolSendSettings();
    return;
  }
  if (!strcmp(type, "screen")) {
    if (doc["step"].is<int>()) req.step = constrain(doc["step"].as<int>(), -1, 1);
    else req.screen = doc["n"] | 0;
    return;
  }
  if (!strcmp(type, "apps")) {
    JsonArrayConst list = doc["list"];
    uint8_t n = 0;
    for (JsonVariantConst v : list) {
      if (n >= MAX_APPS) break;
      const char *name = v | "";
      if (!*name) continue;
      // The board fonts are ASCII only; anything else shows as '?'.
      size_t i = 0;
      for (; name[i] && i < sizeof g_apps[n].name - 1; i++)
        g_apps[n].name[i] = (name[i] >= 32 && name[i] < 127) ? name[i] : '?';
      g_apps[n].name[i] = 0;
      n++;
    }
    g_appCount = n;
    req.apps = true;
    return;
  }
  if (!strcmp(type, "preview")) {
    const char *v = doc["view"] | "";
    if (!strcmp(v, "alert")) req.preview = 0;
    else if (!strcmp(v, "back")) req.preview = 1;
    else sendErr("unknown view");
    return;
  }
  if (!strcmp(type, "ping")) {
    Serial.println("{\"type\":\"pong\"}");
    return;
  }
  if (!strcmp(type, "hello")) {
    protocolSendHello();
    protocolSendSettings();
    return;
  }
  sendErr("unknown type");
}

void protocolPoll(Requests &req) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) {
        sendErr("line too long");
      } else if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        handleLine(lineBuf, req);
      }
      lineLen = 0;
      overflow = false;
      continue;
    }
    if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    else overflow = true;
  }
}
