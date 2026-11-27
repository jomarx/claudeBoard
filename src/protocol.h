#pragma once
#include <Arduino.h>
#include "model.h"

// Newline-delimited JSON over USB serial (CH340), 115200 8N1.
//
// Host -> device
//   {"type":"hello"}                                   -> device answers with hello
//   {"type":"ping"}                                    -> {"type":"pong"}
//   {"type":"usage","seq":1,"now":1764143520,"tz":28800,
//    "plan":"Max 5x",
//    "session":{"pct":62,"reset":1764150000},
//    "week":{"pct":41,"reset":1764547200},
//    "opus":{"pct":73},
//    "today":{"tokens":4200000,"cost":18.4,"sessions":3,"cache":87,
//             "hourly":[0,0,...24 values],
//             "models":[{"n":"Opus 5.5","p":68},{"n":"Sonnet 5.5","p":29}]}}
//   {"type":"cfg","brightness":200,"alert1":80,"alert2":90,"h24":true,
//    "rot":0,"cycle":0,"fx":"none","dim":120,"clock":"digital","led":"usage","ledlvl":60}
//                                                      -> device answers with its cfg
//       rot: 0 portrait, 1 landscape, 2 portrait flipped, 3 landscape flipped
//       (hello's w/h stay the panel's native 240x320); cycle: seconds between automatic page
//       changes (0 off); fx: "none" | "wipe" | "fade"; dim: idle seconds (0 never);
//       clock: the clock screen's face, "digital" | "analog" | "rings" | "flip" | "words" |
//       "binary" | "dots" (binary without the digit row); led: the back LED, "off" | "usage" | "alerts"; ledlvl: its brightness 0-255.
//       Every field is optional; {"type":"cfg"} alone just asks for the current values.
//       Settings are saved on the board and survive a reboot.
//   {"type":"screen","n":0}                            -> jump to a screen: 0 main, 1 today,
//                                                         2 clock, 3 weather, 4 media,
//                                                         5 calendar, 6 launcher (View in ui.h)
//   {"type":"screen","step":1}                         -> next (1) or previous (-1) screen
//   {"type":"apps","list":["Outlook","Chrome"]}        -> launcher tile names, at most 8,
//       15 printable ASCII chars each; the list replaces the previous one. The board
//       only knows names: what a tile opens stays on the PC.
//   {"type":"weather","place":"Manila","t":31,"feels":36,"hum":70,"wind":12,"code":3,
//    "day":1,"hi":33,"lo":26,"rain":40,"at":1764143520}
//       -> current weather: degrees C, km/h, WMO code, rain = today's chance % (-1 unknown),
//       at = observation time. {"type":"weather","valid":false} clears it.
//   {"type":"media","state":"playing","app":"Spotify","title":"...","artist":"..."}
//       -> now playing on the PC; state "playing" | "paused" | "stopped" | "none" (no session).
//   {"type":"preview","view":"alert"|"back"}           -> show an overlay to test it
//   {"type":"wifi","ssid":"...","pass":"...","ip":"192.168.1.50","gw":"192.168.1.1",
//    "mask":"255.255.255.0","dns":"192.168.1.1","host":"192.168.1.20","port":47821,"token":"..."}
//       -> USB ONLY: stores the Wi-Fi network, the board's fixed address, the server and the
//       pairing token; answers with net. Refused over Wi-Fi. "sync":"wifi" in cfg then uses it.
//   {"type":"net"}                                     -> device answers with net
//   {"type":"setup","open":true|false}                 -> USB ONLY: open or close Wi-Fi setup on the
//       board (its own network + a page at 192.168.4.1, see portal.h); same as holding BOOT 5 s
//
// Over Wi-Fi the same messages travel one per WebSocket text message, on ws://host:port/board,
// after a handshake (net.cpp, companion/src/lan.js) in which the token never crosses the network:
//   server -> board  {"type":"challenge","nonce":"<32 random hex>"}
//   board -> server  {"type":"auth","hmac":"<HMAC-SHA256(token, nonce), 64 hex>"}
//   server -> board  {"type":"authok"}   then the board sends hello and cfg
// A wrong answer closes the socket; 5 in a minute lock that address out for a minute.
// Replies go back on the link the request came in on; events go to the Sync link.
//
// Device -> host
//   {"type":"hello","fw":"0.5.0","w":240,"h":320,"cfg":true,"apps":true,"net":true,
//    "link":"usb"|"wifi","syncFromBoard":"wifi"}       (on boot, on Wi-Fi connect, on request)
//       syncFromBoard (only when set): Sync was chosen on the board's own setup page; the
//       companion adopts it instead of sending its own. The next cfg with "sync" clears it.
//   {"type":"net","sync":"usb"|"wifi","configured":true,"ssid":"...","ip":"...","gw":"...",
//    "mask":"...","dns":"...","host":"...","port":47821,"wifi":"connected"|"connecting"|"off",
//    "server":"connected"|"not connected","rssi":-60,"setup":false,"setupSsid":"claudeBoard-XXXX",
//    "heap":...,"largest":...}   setup = the setup network is open; setupSsid only then (its
//    password stays on the board's screen)
//                                                      (never the password or the token)
//   {"type":"cfg",...all settings above...}            (after hello and after every cfg)
//   {"type":"ack","seq":1}
//   {"type":"err","msg":"..."}
//   {"type":"event","name":"dismiss"|"snooze"|"screen"|"brightness","value":N}
//   {"type":"event","name":"play","value":1|0}         (auto page change started / paused)
//   {"type":"event","name":"launch","value":i}         (launcher tile i tapped)
//   {"type":"event","name":"media","value":k}          (media button: 0 previous, 1 play/pause,
//                                                        2 next, 3 volume down, 4 mute, 5 up)
//
// Every field inside "usage" is optional; missing ones keep their last value.

// What the frames read in one poll asked for.
struct Requests {
  bool usage = false;     // a usage frame arrived
  bool config = false;    // a cfg frame arrived
  Settings before;        // settings before the first cfg frame in this poll
  int screen = -1;        // page to jump to
  int step = 0;           // or pages to move by (+1 / -1)
  bool apps = false;      // the launcher list changed
  bool weather = false;   // a weather frame arrived
  bool media = false;     // a media (now playing) frame arrived
  int preview = -1;       // 0 = alert, 1 = back
  bool net = false;       // new Wi-Fi settings were stored
  int setup = -1;         // 1 = open Wi-Fi setup on the board, 0 = close it
};

// Which link a message came in on; its replies go back the same way.
enum class Link : uint8_t { Usb, Wifi };

void protocolBegin();
// Reads whatever is waiting on Serial.
void protocolPoll(Requests &req);
// One message from either link (net.cpp feeds the Wi-Fi ones in).
void protocolHandleLine(const char *line, Requests &req, Link link);
void protocolSendHello(Print &out, bool overWifi);
void protocolSendSettings(Print &out);
// The board's network state (the net reply); also sent unasked over USB when setup opens or closes.
void protocolSendNet(Print &out);
// Goes to the Sync link: Wi-Fi when that's chosen and connected, else USB.
void protocolSendEvent(const char *name, int value);
