#!/usr/bin/env python3
"""
Turn the pendant's P-command frame-buffer dump into a PNG.

The firmware prints one line per dump:

    [dump]<face_index>:<2048 hex chars>

2048 hex chars = 1024 bytes = exactly the SSD1306 frame buffer.
The layout is page-major: 8 pages of 128 bytes, each byte holding 8
vertical pixels, least significant bit on top.

Usage:
    python dump_preview.py                 # read stdin, write face.png
    python dump_preview.py -o out.png      # custom output
    python dump_preview.py -s COM5         # talk to the board directly

Requires: pyserial (only for -s), Pillow
"""

import argparse
import re
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow:  pip install pillow")

W, H = 128, 64
RE_DUMP = re.compile(r"\[dump\](\d+):([0-9a-fA-F]{2048})")


def buffer_to_image(buf: bytes) -> Image.Image:
    """SSD1306 page-major buffer -> 1-bit PIL image."""
    if len(buf) != 1024:
        raise ValueError(f"expected 1024 bytes, got {len(buf)}")
    im = Image.new("1", (W, H), 0)
    px = im.load()
    for page in range(8):
        base = page * 128
        for x in range(W):
            byte = buf[base + x]
            if not byte:
                continue
            for bit in range(8):
                if byte >> bit & 1:
                    px[x, page * 8 + bit] = 1
    return im


def find_dump(text: str, want: int | None = None):
    """Return (face_index, bytes) for the last matching dump in text."""
    found = None
    for m in RE_DUMP.finditer(text):
        idx = int(m.group(1))
        if want is None or idx == want:
            found = (idx, bytes.fromhex(m.group(2)))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", default="face.png")
    ap.add_argument("-s", "--serial", help="port to query, e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("-i", "--index", type=int, help="which face to expect (validates the dump)")
    ap.add_argument("--scale", type=int, default=4, help="nearest-neighbour upscale (default 4)")
    args = ap.parse_args()

    if args.serial:
        import serial
        import time

        s = serial.Serial(args.serial, 115200, timeout=0.2)
        print(f"reading {args.serial} ...", file=sys.stderr)
        if args.index is not None:
            s.write(f"{args.index}\n".encode())
            time.sleep(0.6)
        s.reset_input_buffer()
        s.write(b"P\n")
        text = ""
        deadline = time.time() + 5
        while time.time() < deadline:
            chunk = s.read(8192)
            if chunk:
                text += chunk.decode("utf-8", "replace")
                hit = find_dump(text, args.index)
                if hit and (args.index is None or hit[0] == args.index):
                    break
        s.close()
        source = text
    else:
        source = sys.stdin.read()

    hit = find_dump(source, args.index)
    if not hit:
        sys.exit("no complete [dump] line found" +
                 (f" for face {args.index}" if args.index is not None else ""))

    idx, buf = hit
    im = buffer_to_image(buf)
    if args.scale > 1:
        im = im.resize((W * args.scale, H * args.scale), Image.NEAREST)
    im.save(args.output)
    print(f"face {idx} -> {args.output}  ({im.width}x{im.height})")


if __name__ == "__main__":
    main()
