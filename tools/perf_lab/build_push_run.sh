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
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/engine/spektra-core/src/main/cpp"
WORK="${TMPDIR:-/tmp}/spk-perf-lab"
DEV=/data/local/tmp/spk_perf_lab
mkdir -p "$WORK"

# --- toolchain -------------------------------------------------------------
NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
  NDK="$(ls -d "${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "ERROR: set ANDROID_NDK_HOME (NDK r27)"; exit 1; }
HOST_TAG="$(uname | tr '[:upper:]' '[:lower:]')-x86_64"
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

CFLAGS="-std=c++17 -O3 -ffast-math -fno-finite-math-only -pthread -I$CPP"
HWY_FLAGS="-DSPK_ENABLE_HIGHWAY -DHWY_COMPILE_ONLY_STATIC=1 -DHWY_DISABLE_PCLMUL_AES=1 -I$HWY_SRC"

# ===========================================================================
echo
echo "=== 1/3  f64 Highway lanes on the halation tier ==========================="
# ===========================================================================
"$CXX" $CFLAGS $HWY_FLAGS \
  "$CPP/tests/test_exp_filter_hwy.cpp" \
  "$CPP/kernels/exponential_filter.cpp" "$CPP/kernels/gaussian_hwy.cpp" \
  "$CPP/kernels/parallel.cpp" $HWY_LIB_SRC \
  -o "$WORK/test_exp_filter_hwy"
adb push -q "$WORK/test_exp_filter_hwy" "$DEV/" >/dev/null
adb shell "chmod 755 $DEV/test_exp_filter_hwy"

echo "--- SIMD ON ---"
adb shell "$DEV/test_exp_filter_hwy" | tee "$WORK/simd_on.txt"
echo "--- SIMD OFF (scalar) ---"
adb shell "SPK_SIMD=0 $DEV/test_exp_filter_hwy" | tee "$WORK/simd_off.txt"

# The checksums are the equality proof across the two processes: same bits out,
# whatever the timings say. A mismatch means the lanes are NOT a restatement of
# the scalar loop and the whole opt-in is unsafe, so fail loudly.
if diff <(grep -o 'checksum=[0-9a-f]*' "$WORK/simd_on.txt") \
        <(grep -o 'checksum=[0-9a-f]*' "$WORK/simd_off.txt") >/dev/null; then
  echo "VERDICT: end-to-end checksums IDENTICAL across SIMD on/off"
else
  echo "VERDICT: *** CHECKSUM MISMATCH — f64 lanes are not bit-identical ***"
fi

# ===========================================================================
echo
echo "=== 2/3  big.LITTLE affinity ============================================="
# ===========================================================================
adb shell "cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq" 2>/dev/null \
  | tr -d '\r' | paste -sd' ' | sed 's/^/cpuinfo_max_freq per core: /'
echo "(baseline = the SIMD ON run above; each row re-runs it under a pinning policy)"
for ratio in 1.00 0.80 0.50; do
  printf 'SPK_BIG_CORES=1 ratio=%s : ' "$ratio"
  adb shell "SPK_BIG_CORES=1 SPK_BIG_CORE_RATIO=$ratio $DEV/test_exp_filter_hwy" \
    | grep '1024x768' | sed 's/^bench //'
done

# ===========================================================================
echo
echo "=== 3/3  parity-affecting levers (GEMM / separable PSF / fp16) ============"
# ===========================================================================
"$CXX" $CFLAGS \
  "$ROOT/tools/perf_lab/perf_lab.cpp" \
  "$CPP/kernels/exponential_filter.cpp" "$CPP/kernels/gaussian.cpp" \
  "$CPP/kernels/gaussian_hwy.cpp" "$CPP/kernels/parallel.cpp" "$CPP/kernels/half.cpp" \
  -o "$WORK/perf_lab"
adb push -q "$WORK/perf_lab" "$DEV/" >/dev/null
adb shell "chmod 755 $DEV/perf_lab"
# Single-threaded: these levers are about per-core work, and the fork-join
# already scales all of them. Threads would only add scheduler noise.
adb shell "SPK_NUM_THREADS=1 $DEV/perf_lab"

echo
echo "=== done. Full logs in $WORK ==="
echo "Paste the three sections back to the agent; the decision rule is in"
echo "docs/research/perf-lab.md (a lever ships only if it is both faster AND"
echo "inside the oracle band, or is confined to the preview path)."
