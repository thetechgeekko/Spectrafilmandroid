#!/usr/bin/env python3
"""Generate the pinned benchmark source image for the #177 corpus.

The source must be byte-identical on every machine that regenerates it, so the
image is a closed-form function of (x, y) and the PNG is written with stored
(uncompressed) deflate blocks: zlib level 0 emits a fixed container that does
not drift between zlib builds the way levels 1-9 can.

    python tools/baseline/corpus/make_source.py --out /tmp/base.png
    python tools/baseline/corpus/make_source.py --check   # verify the pins

Film modeling powered by spektrafilm (GPLv3).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

REPO = pathlib.Path(__file__).resolve().parents[3]
CORPUS = REPO / "tools" / "baseline" / "corpus.json"


def pixel(x: int, y: int, width: int, height: int) -> tuple[int, int, int]:
    """Deterministic sRGB test pattern: exposure ramp x four chroma bands, plus a
    fine checker so a resampling or channel-order regression cannot hide in it."""
    band = (y * 4) // height
    t = x / (width - 1)
    v = int(round((0.02 + t * t * 0.96) * 255.0))
    q = 0.25
    if band == 0:
        r = g = b = v
    elif band == 1:
        r, g, b = v, int(v * q), int(v * q)
    elif band == 2:
        r, g, b = int(v * q), v, int(v * q)
    else:
        r, g, b = int(v * q), int(v * q), v
    if ((x >> 3) + (y >> 3)) & 1:
        r, g, b = min(255, r + 6), min(255, g + 6), min(255, b + 6)
    return r, g, b


def raw_scanlines(width: int, height: int) -> bytes:
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter type 0 (None) keeps the bytes closed-form
        row = bytearray(width * 3)
        for x in range(width):
            r, g, b = pixel(x, y, width, height)
            i = x * 3
            row[i] = r
            row[i + 1] = g
            row[i + 2] = b
        rows += row
    return bytes(rows)


def _chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def png_bytes(width: int, height: int) -> bytes:
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit truecolor
    # level 0: stored deflate blocks, so the compressed stream is a pure function
    # of the input on every zlib build.
    idat = zlib.compress(raw_scanlines(width, height), 0)
    srgb = _chunk(b"sRGB", bytes([0]))  # rendering intent 0 = perceptual
    gama = _chunk(b"gAMA", struct.pack(">I", 45455))
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + srgb + gama
            + _chunk(b"IDAT", idat) + _chunk(b"IEND", b""))


def load_source_spec() -> dict:
    corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
    return corpus["source"]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=pathlib.Path, help="write the PNG here")
    ap.add_argument("--check", action="store_true",
                    help="regenerate in memory and compare against corpus.json")
    args = ap.parse_args(argv)
    spec = load_source_spec()
    data = png_bytes(spec["width"], spec["height"])
    digest = hashlib.sha256(data).hexdigest()
    if args.out:
        args.out.write_bytes(data)
    if args.check:
        if digest != spec["sha256"] or len(data) != spec["bytes"]:
            print(f"corpus source drift: sha256={digest} bytes={len(data)} "
                  f"expected sha256={spec['sha256']} bytes={spec['bytes']}",
                  file=sys.stderr)
            return 1
        print(f"corpus source: OK ({spec['width']}x{spec['height']}, "
              f"{len(data)} bytes, sha256 {digest[:16]}...)")
        return 0
    print(json.dumps({"sha256": digest, "bytes": len(data)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
