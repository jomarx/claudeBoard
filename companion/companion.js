#!/usr/bin/env node
// claudeBoard companion: reads Claude Code's local logs on this PC and pushes
// usage frames to the meter over USB serial (protocol in ../src/protocol.h).
// Runs in the tray with a settings page; `node companion.js --help` for options.
//
// Nothing here is sent anywhere except the serial port, unless limits are on
// (then it also asks api.anthropic.com for your plan usage).

require('./src/main').main();
