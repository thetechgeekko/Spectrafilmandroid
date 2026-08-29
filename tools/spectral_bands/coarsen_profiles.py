#!/usr/bin/env python3
"""Rebuild the bundled spektra asset tree with spectrally COARSENED film/paper
profiles, to measure what a lower band-count model (e.g. vkdt filmsim's 44 bands
at 10 nm) would cost in output error.

The profiles are kept on their native 81-sample / 5 nm grid; only the INFORMATION
is reduced. Wavelength-indexed arrays are partitioned into non-overlapping blocks
of `stride` samples, each block is replaced by its band value, and that value is
replicated back across the block. A stride-2 profile therefore carries exactly the
information a 41-sample / 10 nm model would, while running through the unmodified
engine at 81 samples -- so no engine change is needed and the measured delta is
attributable to band width alone.

Band value is computed on the LINEAR physical quantity, which is what a careful
low-band port would do:
    log_sensitivity S      ->  log10(mean(10**S))
    *_density D            ->  -log10(mean(10**-D))
--naive averages in the log/density domain instead (the lazy port), and
--decimate takes the first sample of each block (no averaging at all). The three
bracket the answer.

NaN (JSON null) entries are left exactly where they were: the null MASK is
identical between baseline and coarsened, so the only variable is the value.
Block averages ignore nulls; an all-null block stays null.
"""
import argparse, json, math, os, shutil, sys

SPECTRAL_KEYS_LOG = ("log_sensitivity",)
SPECTRAL_KEYS_DENSITY = ("channel_density", "base_density", "midscale_neutral_density")


def blocks(n, stride):
    i = 0
    while i < n:
        yield list(range(i, min(i + stride, n)))
        i += stride


def band_value(vals, mode, domain):
    """vals: the non-null raw values in one block. Returns the band value."""
    if mode == "decimate":
        return vals[0]
    if mode == "naive":
        return sum(vals) / len(vals)
    # "linear" (default): average the linear physical quantity.
    if domain == "log":            # log10 sensitivity
        lin = [10.0 ** v for v in vals]
        return math.log10(sum(lin) / len(lin))
    else:                          # optical density
        lin = [10.0 ** (-v) for v in vals]
        return -math.log10(sum(lin) / len(lin))


def coarsen_array(arr, stride, mode, domain):
    """arr is (S,) or (S,3) with possible None entries. Returns a new array of the
    same shape and the same None mask."""
    vec = isinstance(arr[0], list)
    ch = len(arr[0]) if vec else 1
    out = [list(r) if vec else r for r in arr]
    for blk in blocks(len(arr), stride):
        for c in range(ch):
            vals = []
            for i in blk:
                v = arr[i][c] if vec else arr[i]
                if v is not None:
                    vals.append(float(v))
            if not vals:
                continue
            bv = band_value(vals, mode, domain)
            for i in blk:
                cur = arr[i][c] if vec else arr[i]
                if cur is None:
                    continue          # keep the null mask identical
                if vec:
                    out[i][c] = bv
                else:
                    out[i] = bv
    return out


def coarsen_profile(path, stride, mode):
    d = json.load(open(path))
    data = d.get("data", {})
    touched = []
    for k in SPECTRAL_KEYS_LOG:
        if k in data and data[k]:
            data[k] = coarsen_array(data[k], stride, mode, "log")
            touched.append(k)
    for k in SPECTRAL_KEYS_DENSITY:
        if k in data and data[k]:
            data[k] = coarsen_array(data[k], stride, mode, "density")
            touched.append(k)
    return d, touched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src_asset_dir")
    ap.add_argument("dst_asset_dir")
    ap.add_argument("--stride", type=int, required=True,
                    help="samples per band on the native 5 nm grid (2 => 10 nm)")
    ap.add_argument("--mode", choices=("linear", "naive", "decimate"), default="linear")
    a = ap.parse_args()

    if os.path.exists(a.dst_asset_dir):
        shutil.rmtree(a.dst_asset_dir)
    # Symlink-free copy so the engine sees a normal tree.
    shutil.copytree(a.src_asset_dir, a.dst_asset_dir)

    prof_dir = os.path.join(a.dst_asset_dir, "profiles")
    n = 0
    keys = set()
    for name in sorted(os.listdir(prof_dir)):
        if not name.endswith(".json"):
            continue
        p = os.path.join(prof_dir, name)
        d, touched = coarsen_profile(p, a.stride, a.mode)
        keys.update(touched)
        with open(p, "w") as f:
            json.dump(d, f)
        n += 1
    bands = math.ceil(81 / a.stride)
    print(f"coarsened {n} profiles to stride={a.stride} "
          f"({bands} bands @ {5*a.stride} nm), mode={a.mode}")
    print(f"  arrays touched: {', '.join(sorted(keys))}")


if __name__ == "__main__":
    main()
