#!/usr/bin/env bash
# Local runner for the CI `engine-parity` job (.github/workflows/ci.yml).
#
# Builds and runs every host stage-parity test with the SAME argv the workflow
# uses, so a local run is the same gate CI applies. Not itself a CI job — the
# workflow remains the source of truth; keep the two in sync when tests are
# added.
#
# Usage:  tools/parity/run_engine_parity.sh [build_dir]
# Env:    JOBS=<n>   parallel compile jobs (default: CPU count)
set -uo pipefail
shopt -s nullglob

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/engine/spektra-core/src/main/cpp"
ASSET="$ROOT/engine/spektra-core/src/main/assets/spektra"
G="$ROOT/tools/parity/goldens"
LUT="$ASSET/luts/spectral_upsampling/irradiance_xy_tc.npy"
OUT="${1:-${TMPDIR:-/tmp}/spk-parity}"
JOBS="${JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu) )}"

mkdir -p "$OUT"
cd "$CPP"
SRC=(spektra.cpp gpu/*.cpp kernels/*.cpp io/*.cpp model/*.cpp profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp)
DEF=(-DSPK_TEST_DIR="\"$CPP/tests\"")

# Optional extra compile flags + sources, so an OPT-IN build variant can be gated
# against the SAME table CI runs rather than a hand-picked subset. The default is
# empty, so a plain invocation is byte-for-byte the CI build.
#
#   SPK_PARITY_EXTRA_FLAGS="-DSPK_ENABLE_HIGHWAY -I/path/to/highway ..." \
#   SPK_PARITY_EXTRA_SRC="/path/to/highway/hwy/*.cc" \
#     bash tools/parity/run_engine_parity.sh
#
# This does not touch the (test, argv) table, so the CI drift check above still
# compares like for like.
read -r -a EXTRA_FLAGS <<< "${SPK_PARITY_EXTRA_FLAGS:-}"
read -r -a EXTRA_SRC <<< "${SPK_PARITY_EXTRA_SRC:-}"

# The (test, argv) table, mirroring the workflow's build_run calls in order.
TESTS=(
  "test_simulate_e2e|$ASSET|$G/scan_portra|tests/scan_portra_input_rgb.f64|$G"
  "test_filming|$ASSET/profiles/kodak_portra_400.json|$G/scan_portra|$LUT|tests/scan_portra_input_rgb.f64"
  "test_spatial|$ASSET/profiles/kodak_portra_400.json|$G/scan_portra_spatial|$LUT|tests/scan_portra_input_rgb.f64"
  "test_crop_resize|$ASSET|$G/scan_portra_crop|tests/scan_portra_input_rgb.f64"
  "test_downscale|$ASSET|$G/scan_portra_downscale|tests/scan_portra_input_rgb.f64"
  "test_autoexposure|$ASSET|$G/scan_portra_autoexp|tests/scan_portra_input_rgb.f64|$G"
  "test_small_preview_aa|$ASSET|$G/small_preview_aa|tests/small_preview_aa_input_rgb.f64|$G"
  "test_diffusion|$G"
  "test_diffusion_e2e|$ASSET|$G/scan_diffusion|tests/scan_portra_input_rgb.f64|$G"
  "test_spectral_blur_e2e|$ASSET|$G/scan_spectral_blur|tests/scan_portra_input_rgb.f64|$G"
  "test_hanatos_surface_e2e|$ASSET|$G/scan_portra_surface|tests/scan_portra_input_rgb.f64|$G"
  "test_camera_uvir_e2e|$ASSET|$G/scan_portra_uvir|tests/scan_portra_input_rgb.f64|$G"
  "test_preflash_e2e|$ASSET|$G/print_portra_preflash|tests/scan_portra_input_rgb.f64|$G"
  "test_print_evcomp_e2e|$ASSET|$G/print_portra_evcomp|tests/scan_portra_input_rgb.f64|$G"
  "test_print_curves_morph|$ASSET|$G/print_curves_morph|$G"
  "test_scanner_bwcorr_e2e|$ASSET|$G/print_portra_bwcorr|tests/scan_portra_input_rgb.f64|$G"
  "test_provia_couplers_e2e|$ASSET|$G/scan_provia_couplers|tests/scan_portra_input_rgb.f64"
  "test_np_interp|tests/np_interp_cases.bin"
  "test_gamut_out_aces|tests/gamut_aces_cases.bin"
  "test_gamut_out_oklch|tests/gamut_oklch_cases.bin"
  "test_gamut_out_oklrab|tests/gamut_oklrab_cases.bin"
  "test_gamut_in_xy|tests/gamut_in_cases.bin"
  "test_lut_accel|$G"
  "test_lut_cache_e2e|$ASSET|tests/scan_portra_input_rgb.f64"
  "test_scanner_lut_e2e|$ASSET|$G/scan_portra|tests/scan_portra_input_rgb.f64"
  "test_enlarger_lut_e2e|$ASSET|$G/print_portra|tests/scan_portra_input_rgb.f64"
  "test_output_spaces|$ASSET/profiles/kodak_portra_400.json|$G/scan_portra|$CPP/tests"
  "test_lensblur|$ASSET/profiles/kodak_portra_400.json|$G/scan_portra_lensblur|$LUT|tests/scan_portra_input_rgb.f64"
  "test_parallel|$ASSET|tests/scan_portra_input_rgb.f64"
  "test_tonecurve|$ASSET/profiles/kodak_portra_400.json|$G/scan_portra"
  "test_half"
  "test_bake_lut|$ASSET"
  "test_params_passthrough|$ASSET|tests/scan_portra_input_rgb.f64"
  "test_highlight_boost_e2e|$ASSET|$G/scan_portra_boost|tests/scan_portra_input_rgb.f64|$G"
  "test_spatial_decouple_e2e|$ASSET|$G/scan_portra_lensblur_nohalation|tests/scan_portra_input_rgb.f64|$G"
  "test_print_spatial_e2e|$ASSET|$G/print_portra_spatial|tests/scan_portra_input_rgb.f64|$G"
  "test_grain|$G/scan_portra/film_density_cmy.spkvec|tests/grain_ref_density.spkvec"
  "test_grain_sublayer|$G/scan_portra/film_density_cmy.spkvec|tests/grain_sublayer_ref_density.spkvec|$ASSET/profiles/kodak_portra_400.json"
)

echo "engine-parity: ${#TESTS[@]} tests -> $OUT (jobs=$JOBS)"

# Drift guard: the workflow is the source of truth. If a test is added there and
# not here (or vice versa) this script would silently under-test, so fail loudly
# on a count mismatch rather than reporting a green run over a stale table.
WF="$ROOT/.github/workflows/ci.yml"
if [ -f "$WF" ]; then
  wf_count="$(grep -cE '^ *build_run ' "$WF" || true)"
  if [ "$wf_count" -ne "${#TESTS[@]}" ]; then
    echo "engine-parity: TABLE DRIFT — ci.yml runs $wf_count tests, this script ${#TESTS[@]}."
    echo "  Sync tools/parity/run_engine_parity.sh with .github/workflows/ci.yml."
    exit 1
  fi
fi

# --- compile (parallel) -----------------------------------------------------
pids=()
for entry in "${TESTS[@]}"; do
  name="${entry%%|*}"
  ( g++ -std=c++17 -O2 -pthread -I. -I"$ROOT/tools/parity" "${DEF[@]}" \
      ${EXTRA_FLAGS[@]+"${EXTRA_FLAGS[@]}"} \
      "tests/$name.cpp" "${SRC[@]}" ${EXTRA_SRC[@]+"${EXTRA_SRC[@]}"} \
      -o "$OUT/$name" 2> "$OUT/$name.build" ) &
  pids+=($!)
  # Throttle without `wait -n` (absent from the bash 3.2 shipped on macOS).
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
done
wait

fail=0
for entry in "${TESTS[@]}"; do
  name="${entry%%|*}"
  if [ ! -x "$OUT/$name" ]; then
    echo "$name: FAIL (build)"; sed -n '1,15p' "$OUT/$name.build"; fail=1
  fi
done
[ "$fail" -eq 0 ] || { echo "engine-parity: BUILD FAILURES"; exit 1; }

# --- run --------------------------------------------------------------------
for entry in "${TESTS[@]}"; do
  name="${entry%%|*}"
  rest="${entry#"$name"}"; rest="${rest#|}"
  args=()
  if [ -n "$rest" ]; then IFS='|' read -r -a args <<< "$rest"; fi
  rc=0
  # ${args[@]+...} keeps an EMPTY array from tripping `set -u` on bash 3.2.
  "$OUT/$name" ${args[@]+"${args[@]}"} > "$OUT/$name.out" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "$name: FAIL (exit $rc)"; tail -n 12 "$OUT/$name.out"; fail=1
  elif [ ! -s "$OUT/$name.out" ]; then
    echo "$name: FAIL (no output)"; fail=1
  elif grep -qiE "FAIL" "$OUT/$name.out"; then
    echo "$name: FAIL"; grep -iE "FAIL" "$OUT/$name.out" | head -n 6; fail=1
  else
    echo "$name: ok"
  fi
done

if [ "$fail" -ne 0 ]; then echo "engine-parity: FAILED"; else echo "engine-parity: ALL OK"; fi
exit $fail
