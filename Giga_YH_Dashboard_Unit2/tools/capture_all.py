#!/usr/bin/env python3
"""Capture every screen of the Unit 2 dashboard in one pass, using the
serial-triggered navigation commands ('H'/'T'/'C'/'B'/'G'/'M') added
alongside the screen exporter. Writes one timestamped PNG per screen under
captures/ and appends a row per screen to captures/capture_log.csv (see
dump_screen.py) -- an audit trail of every capture ever taken, tagged with
a shared run_id so all six screens from one run are easy to group.

Usage: python capture_all.py [COM9]
"""
import sys
import time
import datetime
import serial

from dump_screen import capture_current_screen

SCREENS = [
    ("H", "home"),
    ("T", "time"),
    ("C", "connection"),
    ("B", "battery"),
    ("G", "grid"),
    ("M", "almanac"),
]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
    ser = serial.Serial(port, 115200, timeout=10)

    run_id = datetime.datetime.now().strftime("run-%Y%m%d-%H%M%S")
    for cmd, name in SCREENS:
        ser.reset_input_buffer()
        ser.write(cmd.encode("ascii"))
        time.sleep(0.5)  # let lv_scr_load + lv_timer_handler actually redraw
        capture_current_screen(ser, screen_name=name, note=run_id)

    ser.write(b"H")  # leave it back on Home when done


if __name__ == "__main__":
    main()
