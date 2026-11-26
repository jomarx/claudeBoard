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
  Serial.printf("{\"type\":\"hello\",\"fw\":\"%s\",\"w\":240,\"h\":320}\n", FW_VERSION);
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
}

static FrameResult handleLine(const char *line, int &screenRequest) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, line);
  if (e) {
    sendErr(e.c_str());
    return FrameResult::Other;
  }
  const char *type = doc["type"] | "";

  if (!strcmp(type, "usage")) {
    applyUsage(doc.as<JsonObjectConst>());
    Serial.printf("{\"type\":\"ack\",\"seq\":%lu}\n", (unsigned long)(doc["seq"] | 0UL));
    return FrameResult::Usage;
  }
  if (!strcmp(type, "cfg")) {
    applyConfig(doc.as<JsonObjectConst>());
    Serial.println("{\"type\":\"ack\",\"cfg\":true}");
    return FrameResult::Config;
  }
  if (!strcmp(type, "screen")) {
    screenRequest = doc["n"] | 0;
    return FrameResult::Screen;
  }
  if (!strcmp(type, "ping")) {
    Serial.println("{\"type\":\"pong\"}");
    return FrameResult::Other;
  }
  if (!strcmp(type, "hello")) {
    protocolSendHello();
    return FrameResult::Other;
  }
  sendErr("unknown type");
  return FrameResult::Other;
}

FrameResult protocolPoll(int &screenRequest) {
  FrameResult last = FrameResult::None;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) {
        sendErr("line too long");
      } else if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        FrameResult r = handleLine(lineBuf, screenRequest);
        if (r != FrameResult::Other || last == FrameResult::None) last = r;
      }
      lineLen = 0;
      overflow = false;
      continue;
    }
    if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    else overflow = true;
  }
  return last;
}
