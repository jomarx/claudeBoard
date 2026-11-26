# Claude usage meter (ESP32 2.4" ST7789 touch board)

Firmware for the diymore ESP32 240x320 touch board (CYD 2432S024 family).
All data arrives over the USB cable as newline-delimited JSON (115200 baud, CH340).
The protocol is documented at the top of `src/protocol.h`.

## Build and flash

Needs PlatformIO (the VS Code extension, or `pip install platformio`).

```
pio run -e cyd24r -t upload      # resistive touch (XPT2046), default
pio run -e cyd24c -t upload      # capacitive touch (CST820)
```

On Windows, if the library install fails with "filename too long", build with a
short workspace path, for example: `set PLATFORMIO_WORKSPACE_DIR=C:\pio-cm`.

## Test without the real host companion

```
pip install pyserial
python tools/send_fake.py --list
python tools/send_fake.py COM5           # demo numbers
python tools/send_fake.py COM5 --ramp    # walks 50 -> 100%, fires the alerts, then resets
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

- Colours inverted: change `-DTFT_INVERSION_OFF` to `-DTFT_INVERSION_ON` in `platformio.ini`.
- Red and blue swapped: change `TFT_RGB_ORDER` to `TFT_BGR`.
- Garbled or blank picture: the panel may actually be an ILI9341; swap `-DST7789_DRIVER` for `-DILI9341_2_DRIVER`.
- Upside down: `tft.setRotation(2)` in `ui.cpp`.
- Taps land in the wrong place: add `-DTOUCH_SWAP_XY`, `-DTOUCH_INVERT_X` or `-DTOUCH_INVERT_Y`,
  or tune `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX`.
