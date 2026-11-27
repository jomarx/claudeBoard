# claudeBoard

Claude usage meter on a 2.8" ESP32-32E display board (E32R28T, ST7789 240x320, XPT2046 touch).
Firmware in `src/` (PlatformIO, Arduino, TFT_eSPI configured in `platformio.ini`), PC companion in
`companion/` (Node, packaged as `claudeBoard.exe`). Board and PC talk newline-delimited JSON over USB
serial, documented in `src/protocol.h`. Change the protocol in both places at once.

## Commands

There is no test suite or linter; verification is building, flashing and looking at the board.

```
pio run                                        # build default env (e32r28t, touch)
pio run -e e32n28t                             # no-touch variant (BOOT button only)
pio run -e e32r28t -t upload --upload-port COM12
pio device monitor                             # 115200; shows hello/cfg/ack/event JSON
python tools/send_fake.py COM12 [--ramp]       # drive the board without the companion

cd companion
npm install
node companion.js --dry-run --once             # print one usage frame, no board/UI (npm run dry)
node companion.js --port COM12 --verbose       # run from source, log all board traffic
npm run build:exe                              # pkg -> dist/claudeBoard.exe, then GUI-subsystem patch
```

## Architecture

**Firmware** (`src/`): `main.cpp` owns all state and runs a single cooperative loop. Each pass
`protocolPoll()` fills a `Requests` struct (what frames arrived) instead of acting directly;
`loop()` then applies settings (diffing against `req.before`), handles screen jumps/previews,
ticks alerts, dim and auto-cycle, polls `inputPoll()`, and finally calls `uiRender()` only if
`dirty`. Globals `g_usage`/`g_settings`/`g_clock` live in `model.h`; the board has no RTC, so
`Clock` is resynced from `now`/`tz` in every usage frame.
- Views (`ui.h`): `Main`/`Today` are pageable (`PageCount`); `Alert` and `Back` are overlays.
  `main.cpp` tracks `page` (underlying page) separately from `shown` (what's on the glass), and
  transitions (`fx`) only animate page changes, never overlays.
- Alert logic (thresholds, snooze, new-window detection, "you're back") is all in
  `evaluateAlerts()` in `main.cpp`; a new 5-hour window is detected from the reset time moving
  forward or usage dropping sharply.
- User actions on the board (page, brightness, snooze, dismiss) go back to the PC as `event`
  frames so the companion can save them in its config.

**Companion** (`companion/src/`): `main.js` wires everything. `board.js` emits `ready` on the
board's `hello`, after which main sends `cfg` then a usage frame; the board echoes `cfg`, which
keeps the page in sync. `usage.js` `buildFrame()` combines the log scan of
`~/.claude/projects/**/*.jsonl` with optional `--limits` data. Saved config is overridden by CLI
flags until the setting is changed on the page (`effective()` in `main.js`).

## Hardware facts (from https://www.lcdwiki.com/2.8inch_ESP32-32E-7789)

- LCD on HSPI: SCK 14, MOSI 13, MISO 12, CS 15, DC 2, RST tied to EN, **backlight GPIO 21** (high = on).
  The panel needs `TFT_INVERSION_ON` and `TFT_RGB_ORDER=TFT_BGR` (checked on the glass 2026-11-27:
  with inversion off the dark theme showed white, with RGB order the coral gauge showed blue).
- Touch XPT2046 on its own bus (VSPI in `src/input.cpp`): SCK 25, MOSI 32, MISO 39, CS 33, IRQ 36.
- RGB LED 22/16/17 (low = on), speaker amp enable 4 (low = on), battery ADC 34, BOOT button GPIO 0.
- CH340 auto-reset: DTR/RTS drive EN and GPIO0. The companion releases both after opening the port, and
  the firmware ignores BOOT until it reads released, otherwise opening the port fakes a button press.

## Companion layout (`companion/`)

- `companion.js` is only the entry point; the code is in `src/`: `main.js` (CLI flags, wiring, settings
  page handlers), `usage.js` (log scan, limits, frames), `board.js` (serial, reconnect, hello handshake),
  `config.js` (defaults + validation, `%APPDATA%\claudeBoard\config.json`), `server.js`, `tray.js`
  (tray icon, window raising, media session and keys), `hub.js` / `netboard.js` / `lan.js` (Wi-Fi sync),
  `launcher.js`, `weather.js`, `media.js`, `text.js` (ASCII for the board), `startup.js` (HKCU Run value), `log.js`. The page is `public/index.html` (no framework).
- Keep it CommonJS: pkg bundles CommonJS reliably, ESM not.
- Any new setting goes in `config.js` `normalize()` (all input passes through it), the page, and for
  board settings also `Settings` in `src/model.h`, `applyConfig`/`protocolSendSettings` and `settings.cpp`.
- `npm run build:exe` writes `dist/claudeBoard.exe` (git-ignored). Quit the running exe first, or the
  build can't replace the file; quit it before flashing too, since it holds the serial port.
- Testing without touching the real config: set `CLAUDEBOARD_DIR` to a scratch folder and pass
  `--ui-port` with another port, plus `--no-browser`.
- The tray helper is PowerShell 5.1 run with `-EncodedCommand`. It reads stdin with its own
  `StreamReader` because `Console.In.ReadLineAsync` blocks in .NET Framework, and it exits when
  stdin closes, so it never outlives the companion.

## Status (0.5.0)

- Firmware 0.5.0, companion 0.5.0. Eight screens in `View` (`src/ui.h`): Main, Today, Clock, Weather, Media,
  Calendar, Launcher, Network, plus the Alert and Back overlays. Each pageable screen has a nav bar (prev, next, page
  dots, play/pause of the auto page change); tap targets come from `uiHitTest` (`ui.cpp`), actions from
  `handleTap` (`main.cpp`). Text that may not fit goes through `fitText` (`ui.cpp`).
- Rotation 0-3; landscape has its own Main/Today/Weather/Media layouts; touch is calibrated in portrait
  (2026-11-27: raw X mirrored, no swap) and remapped per rotation in `input.cpp`.
- Clock: seven styles (digital, analog, rings, flip, words, binary, dots; `ClockStyle` and `CLOCK_STYLES`
  in `model.h`, `CLOCK_NAMES` in `protocol.cpp`, `protocol.h`, `CLOCK_STYLES` in `config.js`, a button on
  the page), redrawn every second while shown; big digits are TFT_eSPI font 8. "dots" is "binary" without
  the digit row, grid centred and dots 2 px bigger.
- Back RGB LED (`src/led.cpp`, PWM channels 1-3; the backlight is 0): usage colour, alert/back pulses,
  blue pulse when offline; `led`/`ledlvl` board settings.
- Weather: `companion/src/weather.js` fetches Open-Meteo every 15 min for `config.weather` (lat/lon from the
  page's place search, `/api/geocode`); a `weather` frame goes out only when `weatherState.seq` moves, and a
  fetch's answer is dropped if the place changed while it was out.
- Media: the tray helper polls Windows' media session (WinRT `GlobalSystemMediaTransportControlsSessionManager`,
  non-blocking via AsTask) and presses media keys (`key|<vk>`); `media.js` builds the `media` frame and maps
  the board's `media` event (MediaKey in `ui.h`) to key codes. Board text goes through `text.js` `ascii()`.
- Launcher: tiles live in the companion config (`apps`); the board gets names only (`apps` frame) and sends a
  `launch` event, which `launcher.js` opens (`.exe` spawned directly, anything else via the shell); the tray
  helper then raises the app's window (`front|...`), since Windows won't let a background process do it.
- Companion (CommonJS so pkg can bundle it): tray icon via a hidden PowerShell NotifyIcon helper, settings page
  on 127.0.0.1:47820 (Host check + JSON-only POST), config in `%APPDATA%\claudeBoard`, single instance via the
  UI port. It spots firmware without the 0.3+ screens by the `apps` flag in hello (kept as `screens`
  in `board.js` state) and says so on the page.
- The board's CH340 (1a86:7523) has shown up as COM12 and COM13 depending on the USB port; COM3 is an
  unrelated CH9102 with the same vendor id. Auto-detect (`autoPort` in `board.js`) takes only the CH340.
- `--limits` reads the `claude` CLI sign-in from `~/.claude/.credentials.json` and calls the undocumented
  `api.anthropic.com/api/oauth/usage` endpoint; the token expires every few hours and running `claude`
  renews it (the desktop app's sign-in is not in that file). Claude credentials never go to the board.
- Every commit is reviewed by `.githooks/post-commit` (headless, read-only `claude -p`, results in `reviews/`,
  git-ignored); enable per clone with `git config core.hooksPath .githooks`.
- Wi-Fi sync (0.5.0): Sync is USB **or** Wi-Fi (board setting `sync`), never both. Over Wi-Fi the board
  (`src/net.cpp`: station with a fixed IP + `links2004/WebSockets` client) answers the server's `challenge`
  with `auth` = HMAC-SHA256(pairing token, nonce) (the token never crosses the network), then sends the
  same protocol messages as USB, one per WebSocket message. `protocolHandleLine(line, req,
  link)` answers on the link a message came from; events go to the Sync link (`protocolEventOut`). Network
  settings live in NVS namespace `net`, outside `Settings`, so the password and token are never echoed;
  they arrive only in a `wifi` message over USB. The companion's `hub.js` holds the USB `Board` and any
  `NetBoard`s (`netboard.js`, both `BoardLink`s from `board.js`): data to the Sync side, `cfg` to every
  link. `lan.js` listens on `0.0.0.0:<lan.port>` (default 47821) only while Sync is Wi-Fi; the settings
  page stays on 127.0.0.1. Test the LAN side without hardware: `node tools/fake_wifi_board.js host port token`.
  Measured on the board: largest free block 70 KB before Wi-Fi, 45 KB with it (the screen buffer is
  allocated before Wi-Fi starts). USB mode keeps the radio fully off (`WiFi.mode(WIFI_OFF)`).
- Setup on the board (`src/portal.cpp`): BOOT held 5 s, the page's "Open Wi-Fi setup on the board", the USB
  `setup` message, or Wi-Fi Sync with nothing stored opens a WPA2 network `claudeBoard-XXXX` (fresh 8-digit
  password on the screen) with a captive form at 192.168.4.1; saving stores, sets Sync to Wi-Fi, restarts.
  Nothing is erased by opening it. `netCheck()` validates for both the USB `wifi` message and the form.
- Screens: an 8th page, Network (Sync, Wi-Fi, signal, board IP, server, free memory); in Wi-Fi Sync the
  header shows a Wi-Fi mark (green server up, amber network only, red no network).
- Debug switches `-DBOOT_SELFTEST=1` and `-DTOUCH_DEBUG=1` are commented out in `platformio.ini`.
- Tested over serial on the board: every screen, style and state in portrait and landscape, cfg echo, NVS
  across a reset, reconnect. Not yet checked by eye on the glass: the layouts, landscape taps, the media
  keys acting on the PC, the LED colours, apps coming to the front, the tray icon.

## Next steps

1. Test Wi-Fi sync on the real network (the setup form or the page), and the on-board setup from a phone;
   the PC used so far has no Wi-Fi adapter, so neither was possible from it.
2. Check the items above on the glass; tune layouts in `ui.cpp` (geometry is in small `*Geo` functions or
   at the top of each `draw*`), and the wipe strip size/delay in `uiRender` if it looks slow or tears.
3. Exe icon/version resources (not set yet; editing resources after pkg can break its payload).
4. Ideas not started: degrees F / mph for weather, alert beeps on the unused speaker (amp enable GPIO 4).

## Build notes (Windows)

- If the TFT_eSPI install fails with "filename too long", set `PLATFORMIO_WORKSPACE_DIR` to a short path.
- If building fails with `No module named intelhex`, `pip install intelhex` into PlatformIO's Python.
- `pio run -e e32r28t -t upload --upload-port COMx`; test without the companion: `python tools/send_fake.py COMx`.
- Two builds at once (e.g. both envs in parallel) can fail with odd compile errors from file locks; rerun.
