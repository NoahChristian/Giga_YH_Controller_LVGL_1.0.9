#!/usr/bin/env python3
"""Chained Change-WiFi / Change-MQTT stress test for the 2026-07-17 hang
investigation. Reproduces the user's real-hardware sequence (WiFi x2 then
MQTT x2, no reboot in between, short-but-real gaps between taps) via
simulated touch over serial, logging every board serial line -- most
importantly the MEM[...] lines emitted by logMemStatus() after each flow
-- so free_size / free_biggest_size / frag_pct can be compared across
invocations to distinguish a genuine leak from fragmentation.

Usage: python chained_hang_test.py [COM9]
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
ser = serial.Serial(PORT, 115200, timeout=0.5)

log_lines = []


def drain(seconds, label=None):
    if label:
        print(f"--- {label} ({seconds}s) ---")
    end = time.time() + seconds
    while time.time() < end:
        line = ser.readline()
        if line:
            text = line.decode(errors="replace").rstrip()
            if text:
                print(text)
                log_lines.append(text)


def tap(x, y, label):
    cmd = f"P{x:03d}{y:03d}".encode()
    ser.write(cmd)
    print(f">>> tap {label} at ({x},{y})")
    time.sleep(0.3)


ser.reset_input_buffer()

CHANGE_WIFI = (123, 405)
CHANGE_MQTT = (677, 405)
SCAN_CANCEL = (100, 45)
MQTT_CANCEL_BACK = (100, 30)
KB_ENTER_NUMBER = (640, 318)   # checkmark key, LV_KEYBOARD_MODE_NUMBER (host screen) -- verified by screenshot
KB_ENTER_TEXT = (735, 443)     # checkmark key, LV_KEYBOARD_MODE_TEXT_LOWER (username/password) -- verified by screenshot; (640,380) is WRONG, it's the "." key

print("=== Navigate to Connection screen ===")
ser.write(b"C")
drain(2, "settle on Connection")

for i in range(1, 3):
    print(f"\n=== Change WiFi invocation {i}/2 ===")
    tap(*CHANGE_WIFI, f"Change WiFi #{i}")
    drain(3, "scan screen appears")
    # WiFi.scanNetworks() is a real blocking scan -- give it generous real time
    drain(10, f"waiting for scan #{i} to complete")
    tap(*SCAN_CANCEL, f"Cancel scan #{i}")
    drain(3, f"back on Connection after WiFi #{i}")

for i in range(1, 3):
    print(f"\n=== Change MQTT invocation {i}/2 ===")
    tap(*CHANGE_MQTT, f"Change MQTT #{i}")
    drain(2, "host screen appears (prefilled)")
    tap(*KB_ENTER_NUMBER, f"submit host #{i}")
    drain(2, "username screen appears (prefilled)")
    tap(*KB_ENTER_TEXT, f"submit username #{i}")
    drain(2, "password screen appears (prefilled)")
    tap(*KB_ENTER_TEXT, f"submit password #{i} -> real MQTT reconnect")
    drain(8, f"waiting for MQTT reconnect #{i}")

print("\n=== Final settle, watching for resumed telemetry (proves no hang) ===")
drain(10, "final settle")

print("\n=== MEM[...] lines observed this run ===")
for l in log_lines:
    if l.startswith("MEM["):
        print(l)

ser.close()
