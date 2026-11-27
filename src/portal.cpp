#include "portal.h"
#include "net.h"
#include "model.h"
#include "settings.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static WebServer *web = nullptr;
static DNSServer *dns = nullptr;
static bool active = false;
static char apSsid[24] = "";
static char apPass[12] = "";
static uint32_t restartAt = 0;
static String networks;   // <option>s for the network field, once the scan finishes

bool portalActive() { return active; }
const char *portalSsid() { return apSsid; }
const char *portalPass() { return apPass; }
int portalClients() { return active ? WiFi.softAPgetStationNum() : 0; }

// Everything that goes into the page from settings or a scan is escaped.
static String esc(const char *s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += *s;
    }
  }
  return o;
}

static String ip(uint32_t v) { return v ? IPAddress(v).toString() : String(""); }

static const char STYLE[] PROGMEM =
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font:16px system-ui,sans-serif;background:#141413;color:#f1efe8;margin:0;padding:20px;max-width:440px}"
    "h1{font-size:20px}label{display:block;margin:12px 0 4px;color:#b4b2a9;font-size:14px}"
    "input{width:100%;box-sizing:border-box;padding:10px;border-radius:6px;border:1px solid #444;background:#1f1f1d;color:#f1efe8;font:inherit}"
    "button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:6px;background:#d97757;color:#fff;font:inherit;font-weight:600}"
    ".err{background:#4a1b0c;color:#f5c4b3;padding:10px;border-radius:6px}.hint{color:#888780;font-size:13px}</style>";

static void sendForm(const char *error) {
  const NetSettings &n = netSettings();
  String h;
  h.reserve(4096);
  h += F("<!doctype html><html><head><meta charset=utf-8><title>claudeBoard Wi-Fi setup</title>");
  h += FPSTR(STYLE);
  h += F("</head><body><h1>claudeBoard Wi-Fi setup</h1>");
  if (error) { h += F("<p class=err>"); h += esc(error); h += F("</p>"); }
  h += F("<form method=post action=/save>"
         "<label>Wi-Fi network</label><input name=ssid list=nets required maxlength=32 value=\"");
  h += esc(n.ssid);
  h += F("\"><datalist id=nets>");
  h += networks;
  h += F("</datalist><label>Wi-Fi password</label><input name=pass type=password maxlength=64 "
         "placeholder='empty keeps the saved one'>"
         "<label>Board IP (fixed, outside the router's DHCP range)</label><input name=ip required value=\"");
  h += ip(n.ip);
  h += F("\"><label>Gateway (the router)</label><input name=gw required value=\"");
  h += ip(n.gw);
  h += F("\"><label>Subnet mask</label><input name=mask required value=\"");
  h += n.mask ? ip(n.mask) : String("255.255.255.0");
  h += F("\"><label>DNS (empty = the gateway)</label><input name=dns value=\"");
  h += ip(n.dns);
  h += F("\"><label>Server: the PC running the companion</label><input name=host required maxlength=63 value=\"");
  h += esc(n.host);
  h += F("\"><label>Server port</label><input name=port type=number min=1024 max=65535 required value=\"");
  h += String(n.port ? n.port : 47821);
  h += F("\"><label>Pairing token (Sync section of the companion's settings page)</label>"
         "<input name=token maxlength=32 placeholder='empty keeps the saved one'>"
         "<button>Save and restart</button></form>"
         "<p class=hint>Nothing changes until you save. Cancel on the board closes this network.</p></body></html>");
  web->send(error ? 400 : 200, "text/html; charset=utf-8", h);
}

// Only phones on the setup network itself are served (the station is off while setup is
// open, but check anyway): Wi-Fi settings must never come from the home network.
static bool fromSetupNetwork() {
  if (web->client().localIP() == WiFi.softAPIP()) return true;
  web->send(403, "text/plain", "setup is only reachable on the board's own setup network");
  return false;
}

static void handleSave() {
  if (!fromSetupNetwork()) return;
  NetSettings n = netSettings();   // start from what is saved: empty password/token keep theirs
  strlcpy(n.ssid, web->arg("ssid").c_str(), sizeof n.ssid);
  if (web->arg("pass").length()) strlcpy(n.pass, web->arg("pass").c_str(), sizeof n.pass);
  if (web->arg("token").length()) netSetToken(n, web->arg("token").c_str());
  strlcpy(n.host, web->arg("host").c_str(), sizeof n.host);
  if (web->arg("port").length()) n.port = constrain(web->arg("port").toInt(), 0, 65535);   // empty keeps it
  IPAddress a, g, m, d;
  if (!a.fromString(web->arg("ip")) || !g.fromString(web->arg("gw")) || !m.fromString(web->arg("mask"))) {
    sendForm("The board IP, gateway and subnet mask must look like 192.168.1.50.");
    return;
  }
  if (!d.fromString(web->arg("dns"))) d = g;
  n.ip = (uint32_t)a;
  n.gw = (uint32_t)g;
  n.mask = (uint32_t)m;
  n.dns = (uint32_t)d;
  if (const char *why = netCheck(n)) {
    String msg = "Not saved: ";
    msg += why;
    msg += ".";
    sendForm(msg.c_str());
    return;
  }
  netSave(n);
  g_settings.sync = SyncMode::Wifi;
  netSetBoardChoseSync(true);   // so a companion on USB adopts Wi-Fi instead of undoing it
  settingsSave();
  String h = F("<!doctype html><html><head><meta charset=utf-8>");
  h += FPSTR(STYLE);
  h += F("</head><body><h1>Saved</h1><p>The board restarts and joins <b>");
  h += esc(n.ssid);
  h += F("</b>. You can leave this network now.</p></body></html>");
  web->send(200, "text/html; charset=utf-8", h);
  restartAt = millis() + 1500;   // let the page reach the phone first
}

// Phones probe a known URL to detect a captive network; anything unknown goes to the form.
static void redirect() {
  if (!fromSetupNetwork()) return;
  web->sendHeader("Location", "http://192.168.4.1/", true);
  web->send(302, "text/plain", "");
}

void portalStart() {
  if (active) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "claudeBoard-%02X%02X", mac[4], mac[5]);
  snprintf(apPass, sizeof apPass, "%08lu", (unsigned long)(esp_random() % 100000000UL));

  // Setup owns the radio: the station and server link drop (portalStop brings them back),
  // so nothing on the home network can reach this form. AP+STA only so the network scan
  // works; the station joins nothing.
  netSuspend();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, apPass);
  WiFi.scanNetworks(true);   // async: the list fills in when it's done
  networks = "";

  dns = new DNSServer();
  dns->start(53, "*", IPAddress(192, 168, 4, 1));
  web = new WebServer(80);
  web->on("/", HTTP_GET, []() { if (fromSetupNetwork()) sendForm(nullptr); });
  web->on("/save", HTTP_POST, handleSave);
  web->onNotFound(redirect);
  web->begin();
  active = true;
}

void portalStop() {
  if (!active) return;
  web->stop();
  delete web;
  web = nullptr;
  dns->stop();
  delete dns;
  dns = nullptr;
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  active = false;
  netApply();   // back to whatever Sync wants: station only, or radio off
}

void portalLoop() {
  if (!active) return;
  dns->processNextRequest();
  web->handleClient();
  const int found = WiFi.scanComplete();
  if (found >= 0) {
    for (int i = 0; i < found; i++) {
      networks += "<option value=\"";
      networks += esc(WiFi.SSID(i).c_str());
      networks += "\">";
    }
    WiFi.scanDelete();
  }
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
}
