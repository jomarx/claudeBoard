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
//    "rot":0,"cycle":0,"fx":"none","dim":120,"clock":"digital"}  -> device answers with its cfg
//       rot: 0 portrait, 1 landscape, 2 portrait flipped, 3 landscape flipped
//       (hello's w/h stay the panel's native 240x320); cycle: seconds between automatic page
//       changes (0 off); fx: "none" | "wipe" | "fade"; dim: idle seconds (0 never);
//       clock: the clock screen's face, "digital" | "analog".
//       Every field is optional; {"type":"cfg"} alone just asks for the current values.
//       Settings are saved on the board and survive a reboot.
//   {"type":"screen","n":0}                            -> jump to a screen: 0 main, 1 today,
//                                                         2 clock, 3 calendar, 4 launcher
//   {"type":"screen","step":1}                         -> next (1) or previous (-1) screen
//   {"type":"apps","list":["Outlook","Chrome"]}        -> launcher tile names, at most 8,
//       15 printable ASCII chars each; the list replaces the previous one. The board
//       only knows names: what a tile opens stays on the PC.
//   {"type":"preview","view":"alert"|"back"}           -> show an overlay to test it
//
// Device -> host
//   {"type":"hello","fw":"0.3.0","w":240,"h":320,"cfg":true,"apps":true}   (on boot)
//   {"type":"cfg",...all settings above...}            (after hello and after every cfg)
//   {"type":"ack","seq":1}
//   {"type":"err","msg":"..."}
//   {"type":"event","name":"dismiss"|"snooze"|"screen"|"brightness","value":N}
//   {"type":"event","name":"play","value":1|0}         (auto page change started / paused)
//   {"type":"event","name":"launch","value":i}         (launcher tile i tapped)
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
  int preview = -1;       // 0 = alert, 1 = back
};

void protocolBegin();
// Reads whatever is waiting on Serial.
void protocolPoll(Requests &req);
void protocolSendHello();
void protocolSendSettings();
void protocolSendEvent(const char *name, int value);
