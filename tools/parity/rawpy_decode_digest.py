#!/usr/bin/env python3
"""Print the latest desktop rawpy processed-buffer digest for RAW parity audits."""

from __future__ import annotations

import argparse
import hashlib

import rawpy


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    args = parser.parse_args()

    with rawpy.imread(args.input) as raw:
        image = raw.postprocess(
            output_color=rawpy.ColorSpace.ACES,
            output_bps=16,
            no_auto_bright=True,
            gamma=(1.0, 1.0),
            use_camera_wb=True,
            half_size=False,
        )

    digest = hashlib.sha256(image.tobytes(order="C")).hexdigest()
    print(
        f"rawpy={rawpy.__version__} libraw={rawpy.libraw_version} "
        f"flags={rawpy.flags} width={image.shape[1]} height={image.shape[0]} "
        f"dtype={image.dtype} bytes={image.nbytes} sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
