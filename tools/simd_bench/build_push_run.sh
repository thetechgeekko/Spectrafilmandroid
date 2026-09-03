#!/usr/bin/env bash
# Spektrafilm for Android — on-device A/B bench for the two SIMD/codegen
# experiments (#155): Highway f32 FIR lanes and the Halide diffusion convolution.
# GPLv3. Film modeling powered by spektrafilm.
#
# Runs on the OWNER'S LAPTOP (needs the NDK + an attached device); the agent
# container has neither. Mirrors the proven tools/gpu_probe/build_push_run.sh
# pattern: cross-compile arm64 -> adb push to /data/local/tmp -> run -> print.
#
#   bash tools/simd_bench/build_push_run.sh
#
# Env:
#   ANDROID_NDK_HOME  NDK r27 root (auto-detected under $ANDROID_SDK_ROOT/ndk)
#   SKIP_HALIDE=1     skip the Halide half (no `pip install halide` needed)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/engine/spektra-core/src/main/cpp"
WORK="${TMPDIR:-/tmp}/spk-simd-bench"
DEV=/data/local/tmp/spk_simd
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

# --- Highway: fetch the pinned release asset (same pin as the CMake build) ---
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

echo "building Highway FIR test (arm64, NEON)..."
# shellcheck disable=SC2086
"$CXX" -std=c++17 -O2 -static-libstdc++ \
  -DSPK_ENABLE_HIGHWAY -DHWY_COMPILE_ONLY_STATIC -DHWY_DISABLE_PCLMUL_AES \
  -I"$CPP" -I"$HWY_SRC" \
  "$CPP/tests/test_gaussian_hwy.cpp" "$CPP/kernels/gaussian.cpp" \
  "$CPP/kernels/gaussian_hwy.cpp" "$CPP/kernels/parallel.cpp" $HWY_LIB_SRC \
  -o "$WORK/test_gaussian_hwy"

adb shell "mkdir -p $DEV"
adb push -q "$WORK/test_gaussian_hwy" "$DEV/" >/dev/null
adb shell "chmod 755 $DEV/test_gaussian_hwy"
echo
echo "===== Highway f32 FIR — bit-identity + throughput (on device) ====="
adb shell "$DEV/test_gaussian_hwy" | tail -8
echo "----- same binary, SIMD disabled (SPK_SIMD=0) -----"
adb shell "SPK_SIMD=0 $DEV/test_gaussian_hwy" | tail -3

# --- Halide: generate the arm64 AOT lib, then bench it ----------------------
if [ "${SKIP_HALIDE:-0}" = "1" ]; then
  echo; echo "(Halide half skipped: SKIP_HALIDE=1)"; exit 0
fi
python3 -c "import halide" 2>/dev/null || {
  echo; echo "Halide half needs the wheel:  pip install halide"; exit 0; }

echo
echo "generating Halide AOT (arm-64-android)..."
python3 "$ROOT/tools/halide/gen_diffusion_conv.py" "$WORK/halide_out" arm-64-android
HL_INC="$(python3 -c 'import halide,os;print(os.path.dirname(halide.__file__))')/include"

echo "building Halide conv bench (arm64)..."
"$CXX" -std=c++17 -O2 -static-libstdc++ \
  -I"$WORK/halide_out" -I"$HL_INC" \
  -DSPK_HALIDE_HEADER='"spk_diffusion_conv_arm_64_android.h"' \
  "$ROOT/tools/halide/bench_diffusion_conv.cpp" \
  "$WORK/halide_out/spk_diffusion_conv_arm_64_android.a" \
  -o "$WORK/bench_diffusion_conv"

adb push -q "$WORK/bench_diffusion_conv" "$DEV/" >/dev/null
adb shell "chmod 755 $DEV/bench_diffusion_conv"
echo
echo "===== Halide vs scalar diffusion convolution (on device) ====="
echo "--- both SERIAL (HL_NUM_THREADS=1): pure scheduling win ---"
adb shell "HL_NUM_THREADS=1 $DEV/bench_diffusion_conv 3"
echo "--- Halide parallel vs scalar serial (upper bound, threading included) ---"
adb shell "$DEV/bench_diffusion_conv 3"
echo
echo "done. Paste both tables into issue #155."
