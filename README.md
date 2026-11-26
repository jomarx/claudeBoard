# claudeBoard: Claude usage meter

Firmware for the 2.8" ESP32-32E display board (E32R28T / E32N28T, ST7789 240x320,
sold by diymore). Pinout: https://www.lcdwiki.com/2.8inch_ESP32-32E-7789
All data arrives over the USB cable as newline-delimited JSON (115200 baud, CH340).
The protocol is documented at the top of `src/protocol.h`.

## Build and flash

Needs PlatformIO (the VS Code extension, or `pip install platformio`).

```
pio run -e e32r28t -t upload     # resistive touch (XPT2046), default
pio run -e e32n28t -t upload     # no-touch version, BOOT button only
```

On Windows, if the library install fails with "filename too long", build with a
short workspace path, for example: `set PLATFORMIO_WORKSPACE_DIR=C:\pio-cm`.

## PC companion (real data)

`companion/` reads Claude Code's own logs on this PC (`~/.claude/projects/**/*.jsonl`)
and sends a frame to the board every 20 seconds. It needs Node 18 or newer.

```
cd companion
npm install
node companion.js                  # finds the board (CH340) by itself
node companion.js --port COM8      # or name the port
node companion.js --dry-run --once # print one frame, no board needed
```

What comes from the logs: today's tokens, the estimated API-equivalent cost, the number of
sessions, the cache hit rate, tokens per hour and the share per model. The 5-hour reset time
is estimated (a window starts at the hour of your first message and lasts five hours).

The percentage of your plan limit is not in the logs. Two ways to get it on the gauge:

- `--limits` asks Claude's servers for your real 5-hour, weekly and Opus usage, the same numbers
  Claude Code's `/usage` screen shows. It uses the sign-in Claude Code already keeps in
  `~/.claude/.credentials.json`, and only talks to `api.anthropic.com`. The endpoint is not a
  public API, so it may change or stop working; the meter falls back to the log numbers if it does.
- `--session-budget 30000000` treats that many tokens (cache reads not counted) as 100% of a
  5-hour window. This is a rough guess you tune yourself.

Other options: `--interval N`, `--plan "Max 5x"`, `--brightness 0-255`, `--verbose`. The script
reconnects by itself when the board is unplugged or reset.

Nothing on the board holds any Claude credentials; it only ever sees the numbers.

## Test without the real host companion

```
pip install pyserial
python tools/send_fake.py --list
python tools/send_fake.py COM8           # demo numbers
python tools/send_fake.py COM8 --ramp    # walks 50 -> 100%, fires the alerts, then resets
```

## Controls

| Input | Normal screens | Alert screen |
|---|---|---|
| Tap / BOOT button | next screen | tap a button / BOOT dismisses |
| Long press (touch or BOOT) | cycle brightness | - |

Screen dims after 2 minutes idle; the first touch only wakes it. Alerts fire at
80%, 90% and 100% of the 5-hour limit (change with a `cfg` frame). A higher threshold
breaks through a snooze. When a window that hit 100% resets, a "You're back" screen shows.

## If something looks wrong on first boot

- The board flashes red, green, blue at boot while `-DBOOT_SELFTEST` is set; remove it once you are happy.
- Touch prints `{"type":"touch",...}` lines over serial while `-DTOUCH_DEBUG` is set, for calibration.

- Colours inverted: change `-DTFT_INVERSION_OFF` to `-DTFT_INVERSION_ON` in `platformio.ini`.
- Red and blue swapped: change `TFT_RGB_ORDER` to `TFT_BGR`.
- Garbled or blank picture: the panel may actually be an ILI9341; swap `-DST7789_DRIVER` for `-DILI9341_2_DRIVER`.
- Upside down: `tft.setRotation(2)` in `ui.cpp`.
- Taps land in the wrong place: add `-DTOUCH_SWAP_XY`, `-DTOUCH_INVERT_X` or `-DTOUCH_INVERT_Y`,
  or tune `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX`.
