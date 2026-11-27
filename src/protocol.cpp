#include "protocol.h"
#include "model.h"
#include "net.h"
#include "portal.h"
#include <ArduinoJson.h>

static char lineBuf[2048];
static size_t lineLen = 0;
static bool overflow = false;

void protocolBegin() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
}

// Where events go: the link the Sync setting picks. Wi-Fi without a server link falls back
// to USB, so a tap still shows up on a serial monitor.
Print &protocolEventOut() {
  if (g_settings.sync == SyncMode::Wifi && netServerConnected()) return netOut();
  return Serial;
}

void protocolSendHello(Print &out, bool overWifi) {
  // syncFromBoard: Sync was set on the board itself (setup page); a companion adopts it
  // rather than sending its own back. Cleared by the next cfg that carries sync.
  out.printf("{\"type\":\"hello\",\"fw\":\"%s\",\"w\":240,\"h\":320,\"cfg\":true,\"apps\":true,"
             "\"net\":true,\"link\":\"%s\"%s}\n",
             FW_VERSION, overWifi ? "wifi" : "usb",
             netBoardChoseSync() ? ",\"syncFromBoard\":\"wifi\"" : "");
}

// Copies text for the screen: the board fonts are ASCII only, so anything else shows as '?'.
static void copyAscii(char *dst, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i < n - 1; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : '?';
  dst[i] = 0;
}

static const char *FX_NAMES[] = {"none", "wipe", "fade"};
static const char *CLOCK_NAMES[CLOCK_STYLES] = {"digital", "analog", "rings", "flip", "words", "binary"};
static const char *LED_NAMES[LED_MODES] = {"off", "usage", "alerts"};
static const char *SYNC_NAMES[2] = {"usb", "wifi"};

void protocolSendSettings(Print &out) {
  const Settings &s = g_settings;
  out.printf("{\"type\":\"cfg\",\"brightness\":%u,\"alert1\":%u,\"alert2\":%u,\"h24\":%s,"
             "\"rot\":%u,\"cycle\":%u,\"fx\":\"%s\",\"dim\":%u,\"clock\":\"%s\","
             "\"led\":\"%s\",\"ledlvl\":%u,\"sync\":\"%s\"}\n",
             s.brightness, s.alert1, s.alert2, s.h24 ? "true" : "false",
             s.rot, s.cycle, FX_NAMES[(int)s.fx], s.dim, CLOCK_NAMES[(int)s.clock],
             LED_NAMES[(int)s.led], s.ledLevel, SYNC_NAMES[(int)s.sync]);
}

void protocolSendEvent(const char *name, int value) {
  protocolEventOut().printf("{\"type\":\"event\",\"name\":\"%s\",\"value\":%d}\n", name, value);
}

static void sendErr(Print &out, const char *msg) {
  JsonDocument d;
  d["type"] = "err";
  d["msg"] = msg;
  serializeJson(d, out);
  out.print('\n');
}

// The board's network state. The password and the token never leave the board.
void protocolSendNet(Print &out) {
  const NetSettings &n = netSettings();
  JsonDocument d;
  d["type"] = "net";
  d["sync"] = SYNC_NAMES[(int)g_settings.sync];
  d["configured"] = n.configured();
  d["ssid"] = n.ssid;
  d["ip"] = IPAddress(n.ip).toString();
  d["gw"] = IPAddress(n.gw).toString();
  d["mask"] = IPAddress(n.mask).toString();
  d["dns"] = IPAddress(n.dns).toString();
  d["host"] = n.host;
  d["port"] = n.port;
  d["wifi"] = netWifiState();
  d["server"] = netServerConnected() ? "connected" : "not connected";
  d["rssi"] = netRssi();
  // Memory is the main risk of running Wi-Fi beside the 76.8 KB screen buffer: watch it.
  d["setup"] = portalActive();
  if (portalActive()) d["setupSsid"] = portalSsid();   // not its password: that stays on the screen
  d["heap"] = ESP.getFreeHeap();
  d["largest"] = ESP.getMaxAllocHeap();
  serializeJson(d, out);
  out.print('\n');
}

// {"type":"wifi",...} from the companion: the network, the board's fixed address, the
// server and the token. Only over USB, so nobody on the network can move a board elsewhere.
// True when the settings were stored (then Wi-Fi restarts with them).
static bool applyWifi(Print &out, JsonObjectConst d, Link link) {
  if (link != Link::Usb) { sendErr(out, "wifi settings only over USB"); return false; }
  IPAddress ip, gw, mask, dns;
  if (!ip.fromString(d["ip"] | "") || !gw.fromString(d["gw"] | "") || !mask.fromString(d["mask"] | "")) {
    sendErr(out, "wifi: ip, gw and mask must be IPv4 addresses");
    return false;
  }
  if (!dns.fromString(d["dns"] | "")) dns = gw;
  NetSettings n;
  strlcpy(n.ssid, d["ssid"] | "", sizeof n.ssid);
  strlcpy(n.pass, d["pass"] | "", sizeof n.pass);
  strlcpy(n.host, d["host"] | "", sizeof n.host);
  netSetToken(n, d["token"] | "");
  n.port = constrain(d["port"] | 47821, 1, 65535);
  n.ip = (uint32_t)ip;
  n.gw = (uint32_t)gw;
  n.mask = (uint32_t)mask;
  n.dns = (uint32_t)dns;
  if (const char *why = netCheck(n)) {
    char msg[96];
    snprintf(msg, sizeof msg, "wifi: %s", why);
    sendErr(out, msg);
    return false;
  }
  netSave(n);
  protocolSendNet(out);
  return true;
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
  const char *led = d["led"];
  if (led) {
    for (int i = 0; i < LED_MODES; i++)
      if (!strcmp(led, LED_NAMES[i])) g_settings.led = (LedMode)i;
  }
  if (d["ledlvl"].is<int>()) g_settings.ledLevel = constrain(d["ledlvl"].as<int>(), 0, 255);
  const char *sync = d["sync"];
  if (sync) {
    if (!strcmp(sync, "usb")) g_settings.sync = SyncMode::Usb;
    else if (!strcmp(sync, "wifi")) g_settings.sync = SyncMode::Wifi;
    netSetBoardChoseSync(false);   // the companion has spoken; it knows the choice now
  }
}

void protocolHandleLine(const char *line, Requests &req, Link link) {
  // Every reply goes back on the link the message came in on.
  Print &out = link == Link::Wifi ? netOut() : (Print &)Serial;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, line);
  if (e) {
    sendErr(out, e.c_str());
    return;
  }
  const char *type = doc["type"] | "";

  if (!strcmp(type, "usage")) {
    applyUsage(doc.as<JsonObjectConst>());
    out.printf("{\"type\":\"ack\",\"seq\":%lu}\n", (unsigned long)(doc["seq"] | 0UL));
    req.usage = true;
    return;
  }
  if (!strcmp(type, "cfg")) {
    if (!req.config) req.before = g_settings;
    applyConfig(doc.as<JsonObjectConst>());
    req.config = true;
    protocolSendSettings(out);
    return;
  }
  if (!strcmp(type, "screen")) {
    if (doc["step"].is<int>()) req.step = constrain(doc["step"].as<int>(), -1, 1);
    else req.screen = doc["n"] | 0;
    return;
  }
  if (!strcmp(type, "weather")) {
    WeatherData &w = g_weather;
    if (doc["valid"].is<bool>() && !doc["valid"].as<bool>()) {
      w = WeatherData();
    } else {
      copyAscii(w.place, sizeof w.place, doc["place"] | "");
      w.temp = doc["t"] | 0;
      w.feels = doc["feels"] | w.temp;
      w.hi = doc["hi"] | w.temp;
      w.lo = doc["lo"] | w.temp;
      w.hum = constrain(doc["hum"] | 0, 0, 100);
      w.wind = constrain(doc["wind"] | 0, 0, 999);
      w.rain = constrain(doc["rain"] | -1, -1, 100);
      w.code = constrain(doc["code"] | 0, 0, 99);
      w.day = (doc["day"] | 1) != 0;
      w.at = doc["at"] | 0UL;
      w.valid = true;
    }
    req.weather = true;
    return;
  }
  if (!strcmp(type, "media")) {
    MediaData &m = g_media;
    const char *st = doc["state"] | "none";
    m.state = !strcmp(st, "playing") ? MediaState::Playing
            : !strcmp(st, "paused") ? MediaState::Paused
            : !strcmp(st, "stopped") ? MediaState::Stopped : MediaState::None;
    copyAscii(m.app, sizeof m.app, doc["app"] | "");
    copyAscii(m.title, sizeof m.title, doc["title"] | "");
    copyAscii(m.artist, sizeof m.artist, doc["artist"] | "");
    req.media = true;
    return;
  }
  if (!strcmp(type, "apps")) {
    JsonArrayConst list = doc["list"];
    uint8_t n = 0;
    for (JsonVariantConst v : list) {
      if (n >= MAX_APPS) break;
      const char *name = v | "";
      if (!*name) continue;
      copyAscii(g_apps[n].name, sizeof g_apps[n].name, name);
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
    else sendErr(out, "unknown view");
    return;
  }
  if (!strcmp(type, "ping")) {
    out.print("{\"type\":\"pong\"}\n");
    return;
  }
  if (!strcmp(type, "hello")) {
    protocolSendHello(out, link == Link::Wifi);
    protocolSendSettings(out);
    return;
  }
  if (!strcmp(type, "wifi")) {
    // Only stored settings restart Wi-Fi; a refused message leaves a working link alone.
    if (applyWifi(out, doc.as<JsonObjectConst>(), link)) req.net = true;
    return;
  }
  if (!strcmp(type, "net")) {
    protocolSendNet(out);
    return;
  }
  if (!strcmp(type, "setup")) {
    // Like the wifi message: only from someone with the cable.
    if (link != Link::Usb) { sendErr(out, "setup only over USB"); return; }
    req.setup = (doc["open"] | true) ? 1 : 0;
    return;
  }
  if (!strcmp(type, "authok")) return;   // the server accepted our token
  sendErr(out, "unknown type");
}

void protocolPoll(Requests &req) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) {
        sendErr(Serial, "line too long");
      } else if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        protocolHandleLine(lineBuf, req, Link::Usb);
      }
      lineLen = 0;
      overflow = false;
      continue;
    }
    if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    else overflow = true;
  }
}
