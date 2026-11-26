#pragma once
#include <Arduino.h>

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
//   {"type":"cfg","brightness":200,"alert1":80,"alert2":90,"h24":true}
//   {"type":"screen","n":0}                            -> jump to a screen
//
// Device -> host
//   {"type":"hello","fw":"0.1.0","w":240,"h":320}      (sent on boot)
//   {"type":"ack","seq":1}
//   {"type":"err","msg":"..."}
//   {"type":"event","name":"dismiss"|"snooze"|"screen","value":N}
//
// Every field inside "usage" is optional; missing ones keep their last value.

enum class FrameResult { None, Usage, Config, Screen, Other };

void protocolBegin();
// Reads whatever is waiting on Serial. Returns what the last full line was.
FrameResult protocolPoll(int &screenRequest);
void protocolSendHello();
void protocolSendEvent(const char *name, int value);
