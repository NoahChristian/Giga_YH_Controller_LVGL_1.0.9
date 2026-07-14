#!/usr/bin/env python3
"""Simulate a real touchscreen tap on the Unit 2 Giga dashboard.

Sends 'P' + 3-digit x + 3-digit y (zero-padded, e.g. "P402242" for
x=402,y=242) over Serial, which drives a second LVGL pointer indev
(see simTouchReadCb() in Giga_YH_Dashboard_Unit2.ino) through a real
press-then-release cycle -- actual LVGL hit-testing and click-bubbling,
the same code path a physical finger drives, not a shortcut around it.
Coordinates are in the dashboard's logical 800x480 landscape space
(same space dump_screen.py's PNGs are in), not raw panel pixels.

Useful for regression-testing tap targets (e.g. the makeColumn()
click-through bug fixed in v1.0.39) without needing hands on the board.

Usage:
    python sim_tap.py <x> <y> [COM9]
"""
import sys
import time
import serial


def tap(ser, x, y):
    if not (0 <= x <= 799 and 0 <= y <= 479):
        raise ValueError(f"({x},{y}) is outside the 800x480 logical screen")
    cmd = b"P" + f"{x:03d}{y:03d}".encode("ascii")
    ser.write(cmd)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    x, y = int(sys.argv[1]), int(sys.argv[2])
    port = sys.argv[3] if len(sys.argv) > 3 else "COM9"

    ser = serial.Serial(port, 115200, timeout=10)
    tap(ser, x, y)
    time.sleep(0.2)  # let the ~80ms sim press-release + lv_timer_handler redraw settle
    print(f"Tapped ({x},{y}) on {port}")


if __name__ == "__main__":
    main()
