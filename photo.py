#!/usr/bin/env python3
"""
photos_embed.py — embed any file into a PNG; recover it byte-for-byte later.

Requires: pip install Pillow numpy

Usage:
  encode:  python photos_embed.py encode <input_file> <output.png>
  decode:  python photos_embed.py decode <input.png> <output_file>

How it works:
  Encode: file bytes are packed into RGB pixel values of a lossless PNG.
          An 8-byte header at the start stores the original file length.
  Decode: pixel values are unpacked back to bytes; the header tells us
          exactly how many bytes to extract, discarding any padding.

Google Photos preserves PNGs byte-for-byte in "original quality" mode,
so the round-trip SHA-256 will match.

Limits:
  Google Photos has a 200 MP pixel limit per image (200,000,000 pixels).
  At 3 bytes/pixel that's ~600 MB per PNG. For larger files, split first.
"""

import sys
import math
import struct
import hashlib
from pathlib import Path

import numpy as np
from PIL import Image

# Pillow's default decompression-bomb guard kicks in at ~178 MP and would
# block decoding of images larger than ~535 MB. We generate these images
# ourselves so the guard isn't needed here.
Image.MAX_IMAGE_PIXELS = None

HEADER_FMT  = ">Q"                       # big-endian uint64
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 8 bytes
MAX_PIXELS  = 200_000_000                # Google Photos 200 MP limit
MAX_BYTES   = MAX_PIXELS * 3 - HEADER_SIZE


def encode(input_path: str, output_png: str) -> None:
    data     = Path(input_path).read_bytes()
    file_len = len(data)

    if file_len > MAX_BYTES:
        sys.exit(
            f"[encode] ERROR: file is {file_len / 1e6:.1f} MB, "
            f"max is {MAX_BYTES / 1e6:.0f} MB per PNG "
            f"(Google Photos 200 MP limit). Split the file first."
        )

    # Payload = 8-byte length header + raw file bytes
    payload = struct.pack(HEADER_FMT, file_len) + data

    # How many RGB pixels do we need?
    n_pixels = math.ceil(len(payload) / 3)

    # Roughly square image
    width  = math.ceil(math.sqrt(n_pixels))
    height = math.ceil(n_pixels / width)
    total  = width * height * 3

    # Pad payload to fill the rectangle exactly
    padded = payload + b"\x00" * (total - len(payload))

    # numpy does the heavy lifting — no Python-level loops
    arr = np.frombuffer(padded, dtype=np.uint8).reshape(height, width, 3)
    Image.fromarray(arr, "RGB").save(output_png, format="PNG", compress_level=0)

    sha     = hashlib.sha256(data).hexdigest()
    png_mb  = Path(output_png).stat().st_size / 1024 / 1024
    print(f"[encode] {input_path}  ({file_len / 1024 / 1024:.2f} MB)")
    print(f"         → {output_png}  ({width}×{height} px, {png_mb:.2f} MB on disk)")
    print(f"         SHA-256: {sha}")


def decode(input_png: str, output_path: str) -> None:
    arr = np.array(Image.open(input_png).convert("RGB"))

    # Flatten pixel array back to a flat byte stream
    raw = arr.flatten().tobytes()

    # Read the 8-byte header to learn how many bytes to extract
    (file_len,) = struct.unpack(HEADER_FMT, raw[:HEADER_SIZE])

    if file_len > len(raw) - HEADER_SIZE:
        sys.exit(
            f"[decode] ERROR: header claims {file_len} bytes "
            f"but image only holds {len(raw) - HEADER_SIZE}. "
            f"Wrong file or corrupt PNG."
        )

    data = raw[HEADER_SIZE : HEADER_SIZE + file_len]
    Path(output_path).write_bytes(data)

    sha = hashlib.sha256(data).hexdigest()
    print(f"[decode] {input_png}")
    print(f"         → {output_path}  ({file_len / 1024 / 1024:.2f} MB)")
    print(f"         SHA-256: {sha}")


def main() -> None:
    if len(sys.argv) != 4 or sys.argv[1] not in ("encode", "decode"):
        print(__doc__)
        sys.exit(1)

    cmd, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
    if cmd == "encode":
        encode(a, b)
    else:
        decode(a, b)


if __name__ == "__main__":
    main()