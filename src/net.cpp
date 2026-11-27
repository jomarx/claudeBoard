#include "net.h"
#include "model.h"
#include "protocol.h"
#include <WiFi.h>
#include <Preferences.h>
#include <WebSocketsClient.h>

static NetSettings cfg;
static WebSocketsClient ws;
static bool running = false;    // radio on, client started
static bool serverUp = false;   // WebSocket open (auth sent)
static Requests *pending = nullptr;

// Print that sends each completed line as one WebSocket text message. Lines written while
// the server is down are dropped: events then go to USB instead (protocolEventOut).
class LineOut : public Print {
 public:
  size_t write(uint8_t c) override {
    if (c == '\n') {
      if (len && serverUp) ws.sendTXT(buf, len);
      len = 0;
      return 1;
    }
    if (len < sizeof buf) buf[len++] = c;
    return 1;
  }

 private:
  uint8_t buf[1024];
  size_t len = 0;
};
static LineOut out;

Print &netOut() { return out; }
bool netServerConnected() { return running && serverUp; }
const NetSettings &netSettings() { return cfg; }

const char *netWifiState() {
  if (!running) return "off";
  return WiFi.status() == WL_CONNECTED ? "connected" : "connecting";
}

int netRssi() { return running && WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

static void load() {
  Preferences p;
  p.begin("net", true);
  if (p.getBytesLength("cfg") == sizeof(NetSettings)) p.getBytes("cfg", &cfg, sizeof cfg);
  p.end();
}

void netSave(const NetSettings &n) {
  cfg = n;
  Preferences p;
  p.begin("net", false);
  p.putBytes("cfg", &cfg, sizeof cfg);
  p.end();
}

static void onWs(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      serverUp = true;
      // The server drops anything before a valid token.
      char auth[112];
      snprintf(auth, sizeof auth, "{\"type\":\"auth\",\"token\":\"%s\"}", cfg.token);
      ws.sendTXT(auth);
      protocolSendHello(out, true);
      protocolSendSettings(out);
      break;
    }
    case WStype_DISCONNECTED:
      serverUp = false;
      break;
    case WStype_TEXT: {
      static char line[2048];
      if (!pending || length >= sizeof line) break;
      memcpy(line, payload, length);
      line[length] = 0;
      protocolHandleLine(line, *pending, Link::Wifi);
      break;
    }
    default:
      break;
  }
}

void netApply() {
  if (running) {
    ws.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    running = false;
    serverUp = false;
  }
  // USB mode never turns the radio on (it would also take heap the screen buffer needs).
  if (g_settings.sync != SyncMode::Wifi || !cfg.configured()) return;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // a tap should reach the PC without waiting for a sleep cycle
  WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gw), IPAddress(cfg.mask), IPAddress(cfg.dns));
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);

  ws.begin(cfg.host, cfg.port, "/board");
  ws.onEvent(onWs);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);   // ping every 15 s, drop after 2 missed pongs
  running = true;
}

void netBegin() {
  load();
  netApply();
}

void netLoop(Requests &req) {
  if (!running) return;
  pending = &req;
  ws.loop();
  pending = nullptr;
}
