#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Offline host sanitizer qualification shared by CI and release.
#
# This executes JSON/profile/neutral-filter hostile-input regressions, JNI safety
# helpers, and the engine C cancellation ABI. The actual Android JNI bridge is
# exercised separately by EngineBoundaryInstrumentation; this host runner must
# never be described as runtime sanitizer coverage of that bridge.

set -euo pipefail
shopt -s nullglob

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
cd -- "$repo_root"

# qualifier | evidence label | source family
# This manifest is also the fail-closed public inventory consumed by policy
# tests. The run loop below executes every row exactly once.
readonly SUITE_SPECS=(
  "asan-ubsan|engine-jni-safety-helpers|engine-jni-safety"
  "asan-ubsan|engine-c-cancellation-abi|engine-c-cancellation"
  "asan-ubsan|engine-json-profile-hostile-inputs|engine-json-profile"
  "asan-ubsan|engine-npy-hostile-inputs|engine-npy"
  "asan-ubsan|png-writer-hostile-jni-helpers|png-writer"
  "asan-ubsan|tiff-writer-hostile-jni-helpers|tiff-writer"
  "tsan|engine-c-cancellation-race|engine-c-cancellation"
  "tsan|engine-jni-allocation-registry-race|engine-jni-safety"
  "tsan|png-writer-cancellation-race|png-writer"
  "tsan|tiff-writer-cancellation-race|tiff-writer"
)

list_suites() {
  printf '%s\n' "${SUITE_SPECS[@]}"
}

usage() {
  printf 'usage: %s [--run|--list]\n' "${0##*/}" >&2
}

mode="${1:---run}"
case "$mode" in
  --list)
    if (( $# != 1 )); then usage; exit 2; fi
    list_suites
    exit 0
    ;;
  --run)
    if (( $# != 0 && $# != 1 )); then usage; exit 2; fi
    ;;
  *)
    usage
    exit 2
    ;;
esac

build_root="${SPK_NATIVE_SAFETY_BUILD_DIR:-build/native-safety}"
cxx="${CXX:-clang++}"
mkdir -p -- "$build_root"

export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=1:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1:report_signal_unsafe=0}"

readonly ENGINE_CPP="engine/spektra-core/src/main/cpp"
readonly PNG_CPP="lib/pngwriter/src/main/cpp"
readonly TIFF_CPP="lib/tiffwriter/src/main/cpp"

engine_sources=(
  "$ENGINE_CPP/spektra.cpp"
  "$ENGINE_CPP"/gpu/*.cpp
  "$ENGINE_CPP"/kernels/*.cpp
  "$ENGINE_CPP"/io/*.cpp
  "$ENGINE_CPP"/model/*.cpp
  "$ENGINE_CPP"/profiles/*.cpp
  "$ENGINE_CPP"/runtime/*.cpp
  "$ENGINE_CPP"/runtime/stages/*.cpp
)

run_suite() {
  local spec="$1"
  local qualifier label component
  IFS='|' read -r qualifier label component <<< "$spec"

  local -a common=(
    -std=c++17
    -O1
    -g
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all
  )
  case "$qualifier" in
    asan-ubsan) common+=(-fsanitize=address,undefined) ;;
    tsan) common+=(-fsanitize=thread) ;;
    *) printf 'unknown native safety qualifier: %s\n' "$qualifier" >&2; return 2 ;;
  esac

  local output="$build_root/$label"
  local -a command=("$cxx" "${common[@]}" -pthread)
  local -a run_args=()
  case "$component" in
    engine-jni-safety)
      command+=(
        -I "$ENGINE_CPP"
        "$ENGINE_CPP/tests/test_jni_safety.cpp"
      )
      ;;
    engine-c-cancellation)
      command+=(
        -I "$ENGINE_CPP"
        "$ENGINE_CPP/tests/test_cancellation_api.cpp"
        "${engine_sources[@]}"
      )
      run_args+=(
        "$ENGINE_CPP/../assets/spektra"
        "$ENGINE_CPP/tests/scan_portra_input_rgb.f64"
      )
      ;;
    engine-npy)
      command+=(
        -I "$ENGINE_CPP"
        "$ENGINE_CPP/tests/test_npy_lut.cpp"
        "$ENGINE_CPP/io/npy_lut.cpp"
      )
      run_args+=(
        "$ENGINE_CPP/../assets/spektra/luts/spectral_upsampling/irradiance_xy_tc.npy"
      )
      ;;
    engine-json-profile)
      command+=(
        -I "$ENGINE_CPP"
        "$ENGINE_CPP/tests/test_json_profile.cpp"
        "$ENGINE_CPP/profiles/profile.cpp"
        "$ENGINE_CPP/runtime/print_digest.cpp"
        "$ENGINE_CPP/model/color_output.cpp"
        "$ENGINE_CPP/model/density_curves.cpp"
        "$ENGINE_CPP/model/gamut_compression.cpp"
        "$ENGINE_CPP/kernels/spectral_upsampling.cpp"
        "$ENGINE_CPP/kernels/interp.cpp"
        "$ENGINE_CPP/kernels/parallel.cpp"
      )
      run_args+=(
        "$ENGINE_CPP/../assets/spektra/profiles"
        "$ENGINE_CPP/../assets/spektra/filters/neutral_print_filters.json"
      )
      ;;
    png-writer)
      command+=(
        -I "$PNG_CPP"
        "$PNG_CPP/tests/test_png_writer.cpp"
        "$PNG_CPP/png_writer.cpp"
        -lz
      )
      ;;
    tiff-writer)
      command+=(
        -I "$TIFF_CPP"
        "$TIFF_CPP/tests/test_tiff_writer.cpp"
        "$TIFF_CPP/tiff_writer.cpp"
      )
      ;;
    *) printf 'unknown native safety source family: %s\n' "$component" >&2; return 2 ;;
  esac
  command+=(-o "$output")

  printf 'NATIVE_SAFETY_SUITE: %s\n' "$spec"
  "${command[@]}"
  "$output" "${run_args[@]}"
}

for spec in "${SUITE_SPECS[@]}"; do
  run_suite "$spec"
done

printf 'NATIVE_SAFETY_RUNNER: PASS\n'
