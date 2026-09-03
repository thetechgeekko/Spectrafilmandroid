#!/usr/bin/env bash
# Spektrafilm for Android — build the #160 FFT-convolution bench for arm64. GPLv3.
# Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
#
#   tools/perf_lab/build_fft_conv_device_bench.sh [/path/to/ndk] [outdir]
#
# Built at the SHIPPING release flags, because a number measured at -O2 does not
# describe the APK (-ffast-math reassociates; see docs/research/perf-lab.md 14).
# Statically linked libc++ so the binary runs from /data/local/tmp with no
# libc++_shared.so pushed beside it.
set -euo pipefail

NDK=${1:-}
OUT=${2:-build/perf-lab}
if [ -z "$NDK" ]; then
  for c in "$HOME/AppData/Local/Android/Sdk/ndk/27.0.12077973" \
           "${ANDROID_SDK_ROOT:-}/ndk/27.0.12077973" /opt/android-ndk-r27; do
    [ -d "$c" ] && NDK=$c && break
  done
fi
[ -d "$NDK" ] || { echo "build_fft_conv_device_bench: no NDK (pass one)" >&2; exit 2; }

for host in windows-x86_64 linux-x86_64 darwin-x86_64; do
  BIN="$NDK/toolchains/llvm/prebuilt/$host/bin"
  [ -d "$BIN" ] && break
done
CXX="$BIN/clang++"
[ -x "$CXX" ] || CXX="$CXX.exe"

CPP=engine/spektra-core/src/main/cpp
mkdir -p "$OUT"
"$CXX" --target=aarch64-linux-android24 -std=c++17 \
  -O3 -ffast-math -fno-finite-math-only \
  -I"$CPP" -DSPK_FFT_CONVOLVE_TEST_HOOKS -static-libstdc++ \
  tools/perf_lab/fft_conv_device_bench.cpp \
  "$CPP/kernels/fft_convolve.cpp" "$CPP/kernels/fft.cpp" "$CPP/kernels/parallel.cpp" \
  -o "$OUT/fft_conv_bench"
echo "built $OUT/fft_conv_bench"
