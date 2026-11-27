# claudeBoard: Claude usage meter

Firmware for the 2.8" ESP32-32E display board (E32R28T / E32N28T, ST7789 240x320,
sold by diymore). Pinout: https://www.lcdwiki.com/2.8inch_ESP32-32E-7789
All data arrives over the USB cable as newline-delimited JSON (115200 baud, CH340) from a
companion app on the PC (`companion/`, a tray app with a settings page). The protocol is
documented at the top of `src/protocol.h`.

## Build and flash

Needs PlatformIO (the VS Code extension, or `pip install platformio`).

```
pio run -e e32r28t -t upload     # resistive touch (XPT2046), default
pio run -e e32n28t -t upload     # no-touch version, BOOT button only
```

Add `--upload-port COMx` to pick the port. Quit the companion first (tray menu, Quit): while
it runs it holds the serial port and the upload fails with "Access is denied".

On Windows, if the library install fails with "filename too long", build with a
short workspace path, for example: `set PLATFORMIO_WORKSPACE_DIR=C:\pio-cm`.

## PC companion (real data)

`companion/` reads Claude Code's own logs on this PC (`~/.claude/projects/**/*.jsonl`)
and sends a frame to the board every 20 seconds. It runs in the tray and has a settings page
at http://127.0.0.1:47820 (opened from the tray icon, and on first run).

### The exe

```
cd companion
npm install
npm run build:exe        # -> companion/dist/claudeBoard.exe (no Node needed to run it)
```

Double-click `claudeBoard.exe`. It finds the board by itself (the first CH340, USB id 1a86:7523),
puts an icon in the tray (green = live, amber = waiting or a limits error, grey = no board) and
opens the settings page the first time. Starting it again while it runs just opens the page.
The tray menu has Open settings, Reconnect, Start with Windows and Quit.

Settings page: serial port, update interval, orientation (portrait or landscape, each normal or flipped), clock style, brightness,
dim timeout, 12/24h clock, automatic screen change every N seconds, transition (none / wipe /
fade), alert thresholds, real limits on/off, session budget, plan label, start with Windows.
Changes apply to the board right away and are saved in `%APPDATA%\claudeBoard\config.json`;
the board also saves them itself (NVS), so it keeps them without the PC. The log is
`%APPDATA%\claudeBoard\companion.log`.

The build uses `@yao-pkg/pkg` (Node 22) and then marks the exe as a GUI app so no console
window opens (`tools/gui-subsystem.js`). The tray icon is a hidden PowerShell helper
(WinForms NotifyIcon), so nothing else ships with the exe.

### From source

```
node companion.js                  # same as the exe: tray + settings page
node companion.js --port COM12     # pick the port (wins over the saved setting)
node companion.js --dry-run --once # print one frame, no board needed
node companion.js --help
```

What comes from the logs: today's tokens, the estimated API-equivalent cost, the number of
sessions, the cache hit rate, tokens per hour and the share per model. The 5-hour reset time
is estimated (a window starts at the hour of your first message and lasts five hours).

The percentage of your plan limit is not in the logs. Two ways to get it on the gauge:

- "Real plan limits" (or `--limits`) asks Claude's servers for your real 5-hour, weekly and Opus
  usage, the same numbers Claude Code's `/usage` screen shows. It uses the sign-in Claude Code
  already keeps in `~/.claude/.credentials.json`, and only talks to `api.anthropic.com`. The
  endpoint is not a public API, so it may change or stop working; the meter falls back to the
  log numbers if it does.
- "Session budget" (or `--session-budget 30000000`) treats that many tokens (cache reads not
  counted) as 100% of a 5-hour window. This is a rough guess you tune yourself.

Command-line options (`--interval`, `--plan`, `--brightness`, `--limits`, `--verbose`,
`--no-tray`, `--no-browser`, `--ui-port`) win over the saved settings until that setting is
changed on the page. The companion reconnects by itself when the board is unplugged or reset.

Nothing on the board holds any Claude credentials; it only ever sees the numbers.

## Test without the real host companion

```
pip install pyserial
python tools/send_fake.py --list
python tools/send_fake.py COMx           # demo numbers
python tools/send_fake.py COMx --ramp    # walks 50 -> 100%, fires the alerts, then resets
```

With the companion running, the settings page's "Try it" buttons show the next screen, the
alert screen and the "you're back" screen without waiting for real usage.

## Controls

Six screens: **Main** (gauges), **Today** (daily detail), **Clock** (digital, analog, rings, flip, words or binary, picked
under "Clock style" on the settings page), **Weather** (current conditions for a place you
pick on the settings page), **Calendar** (this month, today
circled) and **Apps** (a launcher). Each has a bar along the bottom: previous, next, the page
dots, and play/pause for the automatic screen change.

| Input | Normal screens | Alert screen |
|---|---|---|
| Tap the bar | previous / next screen, play / pause | - |
| Tap elsewhere | Main, Today: next screen. Calendar: month title left / middle / right = previous month / this month / next month. Apps: opens that tile on the PC | tap a button |
| BOOT button | next screen | dismiss |
| Long press (touch or BOOT) | cycle brightness | - |

The RGB LED on the back is a status light ("Back LED" on the settings page): green, amber or red
with the 5-hour usage like the gauge, a red pulse while a limit alert is up, a green pulse when
the limit resets, a slow blue pulse when no data arrives from the PC, and a white blink when you
tap a launcher tile. "Alerts only" keeps it dark except for the pulses; it dims with the screen.

Weather comes from [Open-Meteo](https://open-meteo.com) (free, no account): search a place in the
settings page's Weather section and pick it. The companion fetches it every 15 minutes and only
sends that place's coordinates; the board never goes online. The condition turns amber when the
reading is over 3 hours old.

The launcher's tiles are set on the settings page (Launcher section, up to 8): a name for the
board and what to open, which is a program (`.exe`, with optional arguments), or a file,
folder or URL. The board only gets the names; tapping a tile asks the companion to open it,
so the PC side decides what runs. Each row has a Test button.

Screen dims after 2 minutes idle; the first touch only wakes it. Alerts fire at 80%, 90%
and 100% of the 5-hour limit. A higher threshold breaks through a snooze. When a window that
hit 100% resets, a "You're back" screen shows.

With "change screen every N seconds" set, the board pages by itself after N seconds without a
touch; it never pages away from an alert. The play/pause button stops and restarts that until
the setting changes or the board restarts; with the setting at 0, play pages every 10 seconds. The transition (none, wipe, fade) applies to every
page change, tapped or automatic; alerts always appear at once.

All of these are set on the companion's settings page and saved on the board, so it keeps them
when the PC is off. A long press still cycles brightness, and the companion saves that too.

## If something looks wrong on first boot

- The board flashes red, green, blue at boot while `-DBOOT_SELFTEST` is set; remove it once you are happy.
- Touch prints `{"type":"touch",...}` lines over serial while `-DTOUCH_DEBUG` is set, for calibration.

- Colours inverted: change `-DTFT_INVERSION_OFF` to `-DTFT_INVERSION_ON` in `platformio.ini`.
- Red and blue swapped: change `TFT_RGB_ORDER` to `TFT_BGR`.
- Garbled or blank picture: the panel may actually be an ILI9341; swap `-DST7789_DRIVER` for `-DILI9341_2_DRIVER`.
- Upside down or sideways: pick the orientation on the companion's settings page (touch follows).
- Taps land in the wrong place: add `-DTOUCH_SWAP_XY`, `-DTOUCH_INVERT_X` or `-DTOUCH_INVERT_Y`,
  or tune `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX`. The values in `platformio.ini` are calibrated for
  portrait (rotation 0); the other orientations are derived from them in code, so calibrate in portrait.
