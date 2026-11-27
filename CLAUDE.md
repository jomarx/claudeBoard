# claudeBoard

Claude usage meter on a 2.8" ESP32-32E display board (E32R28T, ST7789 240x320, XPT2046 touch).
Firmware in `src/` (PlatformIO, Arduino, TFT_eSPI configured in `platformio.ini`), PC companion in
`companion/` (Node, packaged as `claudeBoard.exe`). Board and PC talk newline-delimited JSON over USB
serial, documented in `src/protocol.h`. Change the protocol in both places at once.

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
- Firmware 0.2.0: settings saved in NVS (`src/settings.cpp`), rotation 0/2 with touch flipped to match,
  auto screen cycle, wipe/fade transitions, configurable dim, `preview` frames.
- Companion 0.2.0 (`companion/src/`, CommonJS so pkg can bundle it): tray icon via a hidden PowerShell
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

1. Check the transitions and the flipped-orientation taps by eye; tune the wipe strip size/delay in
   `uiRender` (`src/ui.cpp`) if it looks slow or tears.
2. Remove `-DBOOT_SELFTEST=1` (red/green/blue flash at boot) and `-DTOUCH_DEBUG=1` once happy.
3. Exe icon/version resources (not set yet; editing resources after pkg can break its payload).

## Build notes (Windows)

- If the TFT_eSPI install fails with "filename too long", set `PLATFORMIO_WORKSPACE_DIR` to a short path.
- If building fails with `No module named intelhex`, `pip install intelhex` into PlatformIO's Python.
- `pio run -e e32r28t -t upload --upload-port COMx`; test without the companion: `python tools/send_fake.py COMx`.
- Two builds at once (e.g. both envs in parallel) can fail with odd compile errors from file locks; rerun.
