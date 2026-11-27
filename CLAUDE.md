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
- Touch XPT2046 on its own bus (VSPI in `src/input.cpp`): SCK 25, MOSI 32, MISO 39, CS 33, IRQ 36.
- RGB LED 22/16/17 (low = on), speaker amp enable 4 (low = on), battery ADC 34, BOOT button GPIO 0.
- CH340 auto-reset: DTR/RTS drive EN and GPIO0. The companion releases both after opening the port, and
  the firmware ignores BOOT until it reads released, otherwise opening the port fakes a button press.

## Companion layout (`companion/`)

- `companion.js` is only the entry point; the code is in `src/`: `main.js` (CLI flags, wiring, settings
  page handlers), `usage.js` (log scan, limits, frames), `board.js` (serial, reconnect, hello handshake),
  `config.js` (defaults + validation, `%APPDATA%\claudeBoard\config.json`), `server.js`, `tray.js`,
  `startup.js` (HKCU Run value), `log.js`. The page is `public/index.html` (no framework).
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

## Status

- Display, screens, alerts, and the companion with `--limits` all work. The board is now on COM12
  (CH340, 1a86:7523); COM3 is an unrelated CH9102 with the same vendor id, so auto-detect matches the PID.
- Firmware 0.3.0: screens Main/Today/Clock/Calendar/Launcher (clock redraws every second while shown;
  style is the `clock` board setting, big digits are TFT_eSPI font 8), each with a nav bar (prev/next/play-pause;
  tap targets come from `uiHitTest` in `ui.cpp`, actions in `handleTap` in `main.cpp`); settings saved in NVS (`src/settings.cpp`), rotation 0-3 (portrait/landscape, each flippable; landscape has its own Main/Today layouts in `ui.cpp`, touch remapped in `input.cpp`),
  auto screen cycle, wipe/fade transitions, configurable dim, `preview` frames.
- Launcher: tiles live in the companion config (`apps`); the board gets names only (`apps` frame) and
  sends a `launch` event, which `launcher.js` opens (`.exe` spawned directly, anything else via the shell).
- Companion 0.3.0 (`companion/src/`, CommonJS so pkg can bundle it): tray icon via a hidden PowerShell
  NotifyIcon helper, settings page on 127.0.0.1:47820 (Host check + JSON-only POST), config in
  `%APPDATA%\claudeBoard`, single instance via the UI port. `npm run build:exe` -> `dist/claudeBoard.exe`.
- `--limits` reads the `claude` CLI sign-in from `~/.claude/.credentials.json` and calls the undocumented
  `api.anthropic.com/api/oauth/usage` endpoint. The token expires every few hours; running `claude` renews it.
  The desktop app's sign-in is not in that file.
- Claude credentials never go to the board.
- Touch calibrated 2026-11-27 for portrait (raw X mirrored, no swap); `TOUCH_DEBUG` prints
  `{"type":"touch",...}` per tap if it needs redoing.
- Tested on the board: cfg echo, NVS surviving a reset, live settings from the page, reconnect.
  Not yet checked by eye: wipe/fade on the glass, taps in the flipped orientation, the tray icon.

## Next steps

1. Check the transitions, landscape layouts, nav bar, calendar and launcher, and the flipped/landscape
   taps by eye; tune the wipe strip size/delay in
   `uiRender` (`src/ui.cpp`) if it looks slow or tears.
2. Remove `-DBOOT_SELFTEST=1` (red/green/blue flash at boot) and `-DTOUCH_DEBUG=1` once happy.
3. Exe icon/version resources (not set yet; editing resources after pkg can break its payload).

## Build notes (Windows)

- If the TFT_eSPI install fails with "filename too long", set `PLATFORMIO_WORKSPACE_DIR` to a short path.
- If building fails with `No module named intelhex`, `pip install intelhex` into PlatformIO's Python.
- `pio run -e e32r28t -t upload --upload-port COMx`; test without the companion: `python tools/send_fake.py COMx`.
- Two builds at once (e.g. both envs in parallel) can fail with odd compile errors from file locks; rerun.
