#include "net.h"
#include "model.h"
#include "protocol.h"
#include "portal.h"
#include <WiFi.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>

static NetSettings cfg;
static WebSocketsClient ws;
static bool running = false;    // radio on, client started
static bool serverUp = false;   // WebSocket open and the server accepted our challenge answer
static Requests *pending = nullptr;
static bool boardChoseSync = false;   // Sync set on the board's setup page (NVS "chose")

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
  boardChoseSync = p.getBool("chose", false);
  p.end();
}

void netSave(const NetSettings &n) {
  cfg = n;
  Preferences p;
  p.begin("net", false);
  p.putBytes("cfg", &cfg, sizeof cfg);
  p.end();
}

void netSetBoardChoseSync(bool on) {
  if (on == boardChoseSync) return;
  boardChoseSync = on;
  Preferences p;
  p.begin("net", false);
  p.putBool("chose", on);
  p.end();
}

bool netBoardChoseSync() { return boardChoseSync; }

void netSuspend() {
  if (!running) return;
  ws.disconnect();
  WiFi.disconnect(true);
  running = false;
  serverUp = false;
}

const char *netCheck(const NetSettings &n) {
  if (!n.ssid[0]) return "the Wi-Fi network name is missing";
  if (!n.ip || !n.gw || !n.mask) return "the board IP, gateway and subnet mask are all needed";
  if ((n.ip & n.mask) != (n.gw & n.mask)) return "the board IP is not in the gateway's subnet";
  if (!n.host[0]) return "the server address is missing";
  if (n.port < 1024) return "the server port must be 1024 to 65535";
  if (strlen(n.token) != 32) return "the pairing token must be the 32 characters shown on the companion's page";
  // Lower case only: the token is the HMAC key, and the companion's is lower case.
  // (Both entry points lower-case what was typed first; see netSetToken.)
  for (const char *p = n.token; *p; p++)
    if (!isdigit((unsigned char)*p) && (*p < 'a' || *p > 'f')) return "the pairing token has a character that is not 0-9 or a-f";
  return nullptr;
}

void netSetToken(NetSettings &n, const char *typed) {
  size_t i = 0;
  for (; typed[i] && i < sizeof n.token - 1; i++) n.token[i] = tolower((unsigned char)typed[i]);
  n.token[i] = 0;
}

// HMAC-SHA256(key, msg) as 64 lowercase hex characters.
static void hmacHex(const char *key, const char *msg, char hex[65]) {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)key, strlen(key),
                  (const uint8_t *)msg, strlen(msg), mac);
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
}

// Before authok only the handshake is spoken: the server's {"type":"challenge","nonce":...}
// gets {"type":"auth","hmac":HMAC-SHA256(token, nonce)}, so the token itself never goes over
// the network, and a captured answer is useless for the next connection.
static void handshake(const char *line) {
  JsonDocument d;
  if (deserializeJson(d, line)) return;
  const char *type = d["type"] | "";
  if (!strcmp(type, "challenge")) {
    const char *nonce = d["nonce"] | "";
    if (strlen(nonce) < 16 || strlen(nonce) > 64) return;
    char hex[65], auth[96];
    hmacHex(cfg.token, nonce, hex);
    snprintf(auth, sizeof auth, "{\"type\":\"auth\",\"hmac\":\"%s\"}", hex);
    ws.sendTXT(auth);
  } else if (!strcmp(type, "authok")) {
    serverUp = true;
    protocolSendHello(out, true);
    protocolSendSettings(out);
  }
}

static void onWs(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      serverUp = false;   // not until the server accepts our answer to its challenge
      break;
    case WStype_DISCONNECTED:
      serverUp = false;
      break;
    case WStype_TEXT: {
      static char line[2048];
      if (length >= sizeof line) break;
      memcpy(line, payload, length);
      line[length] = 0;
      if (!serverUp) handshake(line);
      else if (pending) protocolHandleLine(line, *pending, Link::Wifi);
      break;
    }
    default:
      break;
  }
}

void netApply() {
  // The setup network owns the radio while it is open; portalStop() calls back here.
  if (portalActive()) return;
  if (running) {
    ws.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    running = false;
    serverUp = false;
  }
  // USB mode never turns the radio on (it would also take heap the screen buffer needs), and
  // turns it fully off after the setup network used it, so the Wi-Fi driver's memory comes back.
  if (g_settings.sync != SyncMode::Wifi || !cfg.configured()) {
    if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.mode(WIFI_OFF);
    return;
  }

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
