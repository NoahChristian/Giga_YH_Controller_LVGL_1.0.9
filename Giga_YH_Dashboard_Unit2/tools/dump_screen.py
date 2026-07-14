#!/usr/bin/env python3
"""Screen exporter for the Unit 2 Giga dashboard.

Sends 'D' over the board's Serial port to trigger dumpFramebufferToSerial()
(see Giga_YH_Dashboard_Unit2.ino), reads back the raw RGB565 framebuffer,
un-rotates it (the physical Giga Display Shield panel is portrait; the
sketch renders our 800x480 landscape dashboard onto it via a 270-degree
LVGL-side rotation, so the raw framebuffer comes back portrait-native and
needs the inverse rotation to look like the intended landscape screen), and
writes a real PNG -- no PIL/Pillow required, just pyserial + zlib.

Every capture is written under captures/ with a timestamp in the filename
(never overwritten) and appended as a row to captures/capture_log.csv, so
there's an audit trail of what was captured and when.

Usage:
    python dump_screen.py [COM9] [screen_name]
"""
import sys
import struct
import zlib
import csv
import datetime
import os
import serial

CAPTURES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")
LOG_PATH = os.path.join(CAPTURES_DIR, "capture_log.csv")


def timestamp():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S")


def log_capture(filename, screen_name, width, height, note=""):
    os.makedirs(CAPTURES_DIR, exist_ok=True)
    is_new = not os.path.exists(LOG_PATH)
    with open(LOG_PATH, "a", newline="") as f:
        w = csv.writer(f)
        if is_new:
            w.writerow(["timestamp_iso", "screen", "filename", "width", "height", "note"])
        w.writerow([datetime.datetime.now().isoformat(timespec="seconds"), screen_name, filename, width, height, note])


def read_line(ser):
    line = ser.readline()
    return line.decode("ascii", errors="replace").strip()


def rgb565_to_rgb888_row(row_bytes, width):
    """row_bytes: width*2 bytes, little-endian RGB565 pixels -> bytes of RGB888."""
    out = bytearray(width * 3)
    for i in range(width):
        lo = row_bytes[i * 2]
        hi = row_bytes[i * 2 + 1]
        pixel = lo | (hi << 8)
        r5 = (pixel >> 11) & 0x1F
        g6 = (pixel >> 5) & 0x3F
        b5 = pixel & 0x1F
        out[i * 3 + 0] = (r5 * 255) // 31
        out[i * 3 + 1] = (g6 * 255) // 63
        out[i * 3 + 2] = (b5 * 255) // 31
    return bytes(out)


def write_png(path, width, height, rgb_rows):
    """rgb_rows: list of `height` bytes objects, each width*3 bytes (RGB888)."""
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit depth, color type 2 = RGB
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)  # filter type 0 (None) per scanline
        raw += row
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def rotate_rows_90(rows, src_w, src_h, clockwise):
    """rows: list of src_h bytes objects, each src_w*3 bytes (RGB888).
    Returns (dst_w, dst_h, dst_rows) rotated 90 degrees."""
    dst_w, dst_h = src_h, src_w
    dst_rows = [bytearray(dst_w * 3) for _ in range(dst_h)]
    for y in range(src_h):
        row = rows[y]
        for x in range(src_w):
            px = row[x * 3:x * 3 + 3]
            if clockwise:
                dx, dy = src_h - 1 - y, x
            else:
                dx, dy = y, src_w - 1 - x
            dst_rows[dy][dx * 3:dx * 3 + 3] = px
    return dst_w, dst_h, [bytes(r) for r in dst_rows]


def capture_current_screen(ser, screen_name="manual", clockwise=False, note=""):
    """Triggers a dump of whatever screen is currently shown, writes a
    timestamped PNG under captures/, appends a capture_log.csv row, and
    returns the output path."""
    ser.reset_input_buffer()
    ser.write(b"D")

    # skip any interleaved log lines until the dump marker
    for _ in range(200):
        line = read_line(ser)
        if line == "FBDUMP":
            break
    else:
        raise RuntimeError("Never saw FBDUMP marker -- is the board booted and idle?")

    w_str, h_str = read_line(ser).split("x")
    width, height = int(w_str), int(h_str)
    fmt = read_line(ser)
    if fmt != "RGB565":
        raise RuntimeError(f"Unexpected pixel format: {fmt}")

    total = width * height * 2
    data = bytearray()
    while len(data) < total:
        chunk = ser.read(min(65536, total - len(data)))
        if not chunk:
            raise RuntimeError(f"Timed out at {len(data)}/{total} bytes")
        data += chunk

    end_marker = read_line(ser)
    while end_marker == "":
        end_marker = read_line(ser)
    if end_marker != "FBEND":
        print(f"Warning: expected FBEND marker, got {end_marker!r}")

    rows = [rgb565_to_rgb888_row(data[y * width * 2:(y + 1) * width * 2], width) for y in range(height)]
    out_w, out_h, out_rows = rotate_rows_90(rows, width, height, clockwise)

    os.makedirs(CAPTURES_DIR, exist_ok=True)
    out_path = os.path.join(CAPTURES_DIR, f"{screen_name}_{timestamp()}.png")
    write_png(out_path, out_w, out_h, out_rows)
    log_capture(out_path, screen_name, out_w, out_h, note)
    print(f"Wrote {out_path} ({out_w}x{out_h})")
    return out_path


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
    screen_name = sys.argv[2] if len(sys.argv) > 2 else "manual"

    ser = serial.Serial(port, 115200, timeout=10)
    capture_current_screen(ser, screen_name)


if __name__ == "__main__":
    main()
