#!/usr/bin/env bash
# Spektrafilm for Android — spectral band-width probe runner. GPLv3.
#
# Answers: "what does a lower spectral band count cost us?" — the question that
# decides whether vkdt's filmsim (44 bands @ 10 nm) is adoptable, given our engine
# runs 81 bands @ 5 nm.
#
# Method (no engine change, no device, no GPU):
#   1. coarsen_profiles.py rebuilds the bundled asset tree with the film/paper
#      spectral arrays band-limited to N nm, kept on the native 81-sample grid.
#   2. band_probe renders the same fixture through both trees and reports the
#      error distribution in 8-bit display codes (output is sRGB-encoded, so a
#      delta of d is d*255 codes).
#
# Stride 1 is a control: it must come back at ~0 codes, proving the copy + JSON
# round-trip perturbs nothing and the whole delta is the band width.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$REPO/engine/spektra-core/src/main/cpp"
ASSET="$REPO/engine/spektra-core/src/main/assets/spektra"
FIXTURE="$CPP/tests/scan_portra_input_rgb.f64"
WORK="${SPK_BAND_WORK:-/tmp/spk_band_probe}"
STRIDES="${SPK_BAND_STRIDES:-1 2 3 4}"

mkdir -p "$WORK"

echo "== building band_probe (host) =="
( cd "$CPP" && g++ -std=c++17 -O2 -pthread -I. \
    "$REPO/tools/spectral_bands/band_probe.cpp" \
    spektra.cpp gpu/*.cpp kernels/*.cpp io/*.cpp model/*.cpp profiles/*.cpp \
    runtime/*.cpp runtime/stages/*.cpp \
    -o "$WORK/band_probe" )

for s in $STRIDES; do
    python3 "$REPO/tools/spectral_bands/coarsen_profiles.py" \
        "$ASSET" "$WORK/asset_s$s" --stride "$s" --mode "${SPK_BAND_MODE:-linear}"
done

# asset_s1 is the control tree: same information as the shipped assets, but put
# through the identical copy + JSON round-trip, so it is the correct A side.
for s in $STRIDES; do
    [ "$s" = "1" ] && continue
    bands=$(( (81 + s - 1) / s ))
    echo
    echo "########## 81 bands @ 5 nm  vs  $bands bands @ $((5*s)) nm ##########"
    "$WORK/band_probe" "$WORK/asset_s1" "$WORK/asset_s$s" "$FIXTURE" 64 64
done

echo
echo "== control: the round-trip alone must be ~0 codes =="
"$WORK/band_probe" "$ASSET" "$WORK/asset_s1" "$FIXTURE" 64 64
