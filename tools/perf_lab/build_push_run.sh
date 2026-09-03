#!/usr/bin/env bash
# Spektrafilm for Android — on-device perf lab: run every untried lever in one pass.
# GPLv3. Film modeling powered by spektrafilm.
#
# Runs on the OWNER'S LAPTOP (needs the NDK + an attached device); the agent
# container has neither. Mirrors tools/gpu_probe/build_push_run.sh: cross-compile
# arm64 -> adb push to /data/local/tmp -> run -> print.
#
#   bash tools/perf_lab/build_push_run.sh
#
# What it answers, in order:
#   1. f64 Highway lanes on the HALATION tier  — the default-ON spatial path the
#      earlier f32-only round never touched. A/B by SPK_SIMD, with an end-to-end
#      checksum compared across the two processes so "faster" is also "identical".
#   2. big.LITTLE affinity — is the render pool being parked on efficiency cores?
#      Sweeps SPK_BIG_CORE_RATIO so the cluster split is measured, not assumed.
#   3. The three parity-affecting levers (perf_lab.cpp): batched/GEMM-shaped
#      spectral integral, Gaussian-mixture diffusion PSF, fp16/f32 plane storage.
#      Each prints a speedup AND a deviation from the exact path.
#
# Env:
#   ANDROID_NDK_HOME  NDK r27 root (auto-detected under $ANDROID_SDK_ROOT/ndk)
set -uo pipefail
# NOT -e. A failing correctness test is exactly when the later sections matter
# most -- the device run that found the f64 problem lost the affinity sweep and
# every lever to a single non-zero exit. Failures are reported and the script
# carries on; the final summary says what failed.
FAILED=""

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/engine/spektra-core/src/main/cpp"
WORK="${TMPDIR:-/tmp}/spk-perf-lab"
DEV=/data/local/tmp/spk_perf_lab
mkdir -p "$WORK"

# --- toolchain -------------------------------------------------------------
NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
  # Try the per-OS default SDK locations, not just the Linux one.
  for cand in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" \
              "$HOME/Android/Sdk" "$HOME/Library/Android/sdk" \
              "${LOCALAPPDATA:-$HOME/AppData/Local}/Android/Sdk"; do
    [ -n "$cand" ] && [ -d "$cand/ndk" ] || continue
    NDK="$(ls -d "$cand"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
    [ -n "$NDK" ] && break
  done
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "ERROR: set ANDROID_NDK_HOME (NDK r27)"; exit 1; }
# NDK prebuilt dirs are linux-x86_64 / darwin-x86_64 / windows-x86_64. Git Bash's
# `uname` reports mingw64_nt-10.0-26200, which matches none of them -- this cost a
# real device run, so map it explicitly rather than lower-casing and hoping.
case "$(uname -s)" in
  Linux*)                      HOST_TAG=linux-x86_64 ;;
  Darwin*)                     HOST_TAG=darwin-x86_64 ;;
  MINGW*|MSYS*|CYGWIN*|Windows*) HOST_TAG=windows-x86_64 ;;
  *)                           HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64" ;;
esac
CXX="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android24-clang++"
[ -x "$CXX" ] || { echo "ERROR: no arm64 clang++ at $CXX"; exit 1; }
adb devices | grep -qw device || { echo "ERROR: no device in 'adb devices'"; exit 1; }
echo "NDK : $NDK"
echo "dev : $(adb shell getprop ro.product.model | tr -d '\r')"
adb shell "mkdir -p $DEV"

# --- Highway: the pinned release asset (same pin as the CMake build) --------
HWY_VER=1.4.0
HWY_SHA=36f672ab48ddb3c8555e9e89e16fe400cd7d16c6eb455a1a3d0c146a63ababdc
HWY_SRC="$WORK/highway-$HWY_VER"
if [ ! -f "$HWY_SRC/hwy/highway.h" ]; then
  echo "fetching highway $HWY_VER..."
  curl -sSL -o "$WORK/hwy.tar.gz" \
    "https://github.com/google/highway/releases/download/$HWY_VER/highway-$HWY_VER.tar.gz"
  echo "$HWY_SHA  $WORK/hwy.tar.gz" | sha256sum -c - || { echo "ERROR: Highway SHA256 mismatch"; exit 1; }
  tar xzf "$WORK/hwy.tar.gz" -C "$WORK"
fi
HWY_LIB_SRC=$(ls "$HWY_SRC"/hwy/*.cc | grep -v '_test\.cc$' | tr '\n' ' ')

# -static-libstdc++ matters: without it the pushed binary needs libc++_shared.so,
# which is not on /data/local/tmp, and the run dies with
# "CANNOT LINK EXECUTABLE ... library libc++_shared.so not found".
# $CPP is quoted at every use below because a checkout path may contain spaces.
CFLAGS="-std=c++17 -O3 -ffast-math -fno-finite-math-only -pthread -static-libstdc++"
HWY_FLAGS="-DSPK_ENABLE_HIGHWAY -DHWY_COMPILE_ONLY_STATIC=1 -DHWY_DISABLE_PCLMUL_AES=1 -I\"$HWY_SRC\""

# One binary now covers everything. The f64 Highway tier that section 1 used to
# test was REMOVED after the first device run measured it ~2% slower than scalar
# (arm64 NEON gives f64 two lanes, and the IIR is latency-bound on a serial
# recurrence) -- see docs/research/perf-lab.md §1. What survives is the f32 tier,
# which sits on the grain path the device says is hot.
"$CXX" $CFLAGS -I"$CPP" $HWY_FLAGS \
  "$CPP/tests/test_gaussian_hwy.cpp" \
  "$CPP/kernels/gaussian.cpp" "$CPP/kernels/gaussian_hwy.cpp" \
  "$CPP/kernels/parallel.cpp" $HWY_LIB_SRC \
  -o "$WORK/test_gaussian_hwy" || FAILED="$FAILED build:test_gaussian_hwy"
"$CXX" $CFLAGS -I"$CPP" \
  "$ROOT/tools/perf_lab/perf_lab.cpp" \
  "$CPP/kernels/exponential_filter.cpp" "$CPP/kernels/gaussian.cpp" \
  "$CPP/kernels/gaussian_hwy.cpp" "$CPP/kernels/parallel.cpp" "$CPP/kernels/half.cpp" \
  "$CPP/kernels/stats.cpp" \
  -o "$WORK/perf_lab" || FAILED="$FAILED build:perf_lab"

for b in test_gaussian_hwy perf_lab; do
  [ -f "$WORK/$b" ] || continue
  adb push -q "$WORK/$b" "$DEV/" >/dev/null && adb shell "chmod 755 $DEV/$b"
done

# ===========================================================================
echo
echo "=== 1/3  f32 Highway lanes on the grain/glare FIR ========================="
# ===========================================================================
# Prints max_abs alongside each comparison. Byte-equality holds at -O2 but NOT
# under these shipping flags: -ffast-math lets the compiler reassociate the
# SCALAR reference too, so the two sides land a couple of ULP apart with neither
# being wrong. The bar is absolute distance (the oracle band is absolute); a
# relative measure explodes wherever a normalised tap sum passes near zero.
adb shell "$DEV/test_gaussian_hwy" || FAILED="$FAILED run:test_gaussian_hwy"

# ===========================================================================
echo
echo "=== 2/3  big.LITTLE affinity (the one lever that was BOTH faster and exact)"
# ===========================================================================
# First device run: 73.50 ms unpinned -> 46.41 ms at ratio 1.00, a 1.58x on the
# default-ON spatial path, with the checksum UNCHANGED. Two 4.74 GHz prime cores
# beat all eight, because the fork-join waits on its slowest chunk and the six
# 3.63 GHz cores set that pace. The checksum on every row is the proof that
# affinity moved WHERE work ran and not WHAT it computed.
adb shell "cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq" 2>/dev/null \
  | tr -d '\r' | paste -sd' ' | sed 's/^/cpuinfo_max_freq per core: /'
printf 'baseline (no pinning)      : '
adb shell "$DEV/perf_lab --halation-only"
for ratio in 1.00 0.80 0.50; do
  printf 'SPK_BIG_CORES=1 ratio=%s : ' "$ratio"
  adb shell "SPK_BIG_CORES=1 SPK_BIG_CORE_RATIO=$ratio $DEV/perf_lab --halation-only"
done

# ===========================================================================
echo
echo "=== 3/3  parity-affecting levers + the irregular-kernel profile =========="
# ===========================================================================
# Single-threaded: these levers are about per-core work, and the fork-join
# already scales all of them. Threads would only add scheduler noise.
adb shell "SPK_NUM_THREADS=1 $DEV/perf_lab" || FAILED="$FAILED run:perf_lab"

echo
if [ -n "$FAILED" ]; then
  echo "=== done, WITH FAILURES:$FAILED ==="
else
  echo "=== done, all sections ran. Full logs in $WORK ==="
fi
echo "Paste the three sections back to the agent; the decision rule is in"
echo "docs/research/perf-lab.md (a lever ships only if it is both faster AND"
echo "inside the oracle band, or is confined to the preview path)."
