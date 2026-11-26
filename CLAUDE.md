# claudeBoard

Claude usage meter on a 2.8" ESP32-32E display board (E32R28T, ST7789 240x320, XPT2046 touch).
Firmware in `src/` (PlatformIO, Arduino, TFT_eSPI configured in `platformio.ini`), PC companion in
`companion/` (Node). Board and PC talk newline-delimited JSON over USB serial, documented in `src/protocol.h`.

## Hardware facts (from https://www.lcdwiki.com/2.8inch_ESP32-32E-7789)

- LCD on HSPI: SCK 14, MOSI 13, MISO 12, CS 15, DC 2, RST tied to EN, **backlight GPIO 21** (high = on).
- Touch XPT2046 on its own bus (VSPI in `src/input.cpp`): SCK 25, MOSI 32, MISO 39, CS 33, IRQ 36.
- RGB LED 22/16/17 (low = on), speaker amp enable 4 (low = on), battery ADC 34, BOOT button GPIO 0.
- CH340 auto-reset: DTR/RTS drive EN and GPIO0. The companion releases both after opening the port, and
  the firmware ignores BOOT until it reads released, otherwise opening the port fakes a button press.

## Status

- Display, screens, alerts, and the companion with `--limits` all work (tested on COM8).
- `--limits` reads the `claude` CLI sign-in from `~/.claude/.credentials.json` and calls the undocumented
  `api.anthropic.com/api/oauth/usage` endpoint. The token expires every few hours; running `claude` renews it.
  The desktop app's sign-in is not in that file.
- Claude credentials never go to the board.

## Next steps

1. Calibrate touch: `TOUCH_DEBUG` prints `{"type":"touch","rx","ry",...}` per tap; tune `TOUCH_X/Y_MIN/MAX`,
   add `TOUCH_SWAP_XY` / `TOUCH_INVERT_X/Y` in `platformio.ini` if needed.
2. Remove `-DBOOT_SELFTEST=1` (red/green/blue flash at boot) and `-DTOUCH_DEBUG=1` once happy.
3. Auto-start the companion at Windows login (Task Scheduler or a Startup shortcut).

## Build notes (Windows)

- If the TFT_eSPI install fails with "filename too long", set `PLATFORMIO_WORKSPACE_DIR` to a short path.
- If building fails with `No module named intelhex`, `pip install intelhex` into PlatformIO's Python.
- `pio run -e e32r28t -t upload --upload-port COMx`; test without the companion: `python tools/send_fake.py COMx`.
