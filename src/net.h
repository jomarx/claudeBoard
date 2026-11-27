#pragma once
#include <Arduino.h>

// Wi-Fi sync (Settings::sync == Wifi): a station on the stored network with a fixed address,
// and a WebSocket client to the companion at ws://host:port/board. The messages are the
// same as over USB (protocol.h), one per WebSocket text message.

// Kept in their own NVS namespace, outside Settings, so the password and the token are
// never part of the cfg echo.
struct NetSettings {
  char ssid[33] = "";
  char pass[65] = "";
  uint32_t ip = 0, gw = 0, mask = 0, dns = 0;   // the board's fixed address (IPAddress raw)
  char host[64] = "";                            // the server: the companion's PC
  uint16_t port = 47821;
  char token[65] = "";                           // pairing token from the companion

  bool configured() const { return ssid[0] && host[0] && token[0] && ip && mask; }
};

struct Requests;

void netBegin();                        // load the settings; start if Sync is Wi-Fi
void netApply();                        // (re)start or stop to match Sync and the settings
void netLoop(Requests &req);            // service the link; incoming messages land in req
void netSave(const NetSettings &n);     // store new settings (netApply then uses them)
// Why these settings can't be used, or nullptr when they can. Shared by the USB wifi message
// and the setup page, so both apply the same rules.
const char *netCheck(const NetSettings &n);
const NetSettings &netSettings();

Print &netOut();                        // writes one message per line to the server
bool netServerConnected();
const char *netWifiState();             // "off" | "connecting" | "connected"
int netRssi();                          // dBm, 0 when not connected
