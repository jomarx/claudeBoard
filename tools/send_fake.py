"""Push fake usage data to the meter over USB serial, to test the firmware
before the real host companion exists.

    pip install pyserial
    python tools/send_fake.py COM5              # steady demo data
    python tools/send_fake.py COM5 --ramp       # climb 50% -> 100% to trigger alerts, then reset
    python tools/send_fake.py --list            # show serial ports

Lines the device sends back (hello, ack, events) are printed.
"""

import argparse
import json
import random
import sys
import threading
import time

import serial
import serial.tools.list_ports


def reader(port):
    while True:
        try:
            line = port.readline().decode(errors="replace").strip()
        except serial.SerialException:
            return
        if line:
            print("<-", line)


def frame(seq, session_pct, session_reset):
    now = int(time.time())
    hour = time.localtime().tm_hour
    hourly = [0] * 24
    for h in range(hour + 1):
        hourly[h] = random.randint(0, 600_000) if 8 <= h <= hour else random.randint(0, 20_000)
    tz = -time.altzone if time.daylight and time.localtime().tm_isdst else -time.timezone
    return {
        "type": "usage",
        "seq": seq,
        "now": now,
        "tz": tz,
        "plan": "Max 5x",
        "session": {"pct": session_pct, "reset": session_reset},
        "week": {"pct": 41, "reset": now + 4 * 86400 + 3600},
        "opus": {"pct": 73},
        "today": {
            "tokens": sum(hourly),
            "cost": round(sum(hourly) / 1e6 * 4.4, 2),
            "sessions": 3,
            "cache": 87,
            "hourly": hourly,
            "models": [
                {"n": "Opus 5.5", "p": 68},
                {"n": "Sonnet 5.5", "p": 29},
                {"n": "Haiku 5.5", "p": 3},
            ],
        },
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=5.0, help="seconds between frames")
    ap.add_argument("--ramp", action="store_true", help="climb to 100%% then reset the window")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list or not args.port:
        for p in serial.tools.list_ports.comports():
            print(f"{p.device:8} {p.description}")
        if not args.port:
            print("\nGive the meter's port, e.g. python tools/send_fake.py COM5")
        return

    port = serial.Serial(args.port, args.baud, timeout=1)
    # Opening the port pulses DTR/RTS on the CH340, which resets the ESP32.
    # Give it time to boot and print its hello line.
    time.sleep(2.5)
    threading.Thread(target=reader, args=(port,), daemon=True).start()

    seq = 0
    pct = 50 if args.ramp else 62
    reset = int(time.time()) + 6480  # 1h 48m from now
    try:
        while True:
            seq += 1
            port.write((json.dumps(frame(seq, pct, reset), separators=(",", ":")) + "\n").encode())
            print(f"-> usage seq={seq} session={pct}%")
            if args.ramp:
                pct += 5
                if pct > 105:
                    pct = 3
                    reset = int(time.time()) + 5 * 3600
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
