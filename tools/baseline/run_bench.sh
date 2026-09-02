#!/usr/bin/env bash
# Run the #177 export benchmark against an installed release candidate.
#
#   bash tools/baseline/run_bench.sh <app.apk> [runs] [cells] [serial]
#
# <app.apk> is the exact APK installed on the device: its SHA-256 is passed to the
# instrumentation, which refuses to measure anything else (stale artifacts fail closed).
# Everything statistical happens afterwards in tools/baseline/bench_report.py.
#
# Film modeling powered by spektrafilm (GPLv3).
set -euo pipefail

APP_APK=${1:?usage: run_bench.sh <app.apk> [runs] [cells] [serial]}
RUNS=${2:-11}
CELLS=${3:-}
SERIAL=${4:-}

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# adb is a native binary: hand it native paths for LOCAL files (on Git Bash a /c/...
# path is not a path at all once MSYS conversion is off).
host_path() { (cd "$(dirname "$1")" && printf "%s/%s" "$(pwd -W 2>/dev/null || pwd)" "$(basename "$1")"); }
OUT=${SPK_BENCH_OUT:-$REPO/build/ticket177-bench}
PKG=com.spectrafilm.app
DEVICE_DIR=/sdcard/Android/data/$PKG/files
# adb on Git Bash mangles device paths unless conversion is disabled -- but the same
# variables break a Windows python invoked with a /c/... path, so scope them to adb only.
# ...and the inverse: a caller that exported those variables globally would break python
# on Git Bash, so python is invoked with them removed.
py_() { env -u MSYS2_ARG_CONV_EXCL -u MSYS_NO_PATHCONV python "$@"; }

adb_() {
  if [ -n "$SERIAL" ]; then
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 adb -s "$SERIAL" "$@"
  else
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 adb "$@"
  fi
}

# Device-only by construction: say so plainly instead of failing three commands later.
command -v adb >/dev/null || { echo "run_bench: adb is required (device-only gate)" >&2; exit 2; }
adb_ get-state >/dev/null 2>&1 || {
  echo "run_bench: no device connected${SERIAL:+ ($SERIAL)}; this gate cannot run without one" >&2
  exit 2
}

mkdir -p "$OUT"
py_ "$REPO/tools/baseline/corpus/make_source.py" --check
SOURCE=$OUT/corpus-source.png
[ -f "$SOURCE" ] || py_ "$REPO/tools/baseline/corpus/make_source.py" --out "$SOURCE"

APP_SHA=$(sha256sum "$APP_APK" | cut -d' ' -f1)
echo "run_bench: app $APP_APK sha256=$APP_SHA runs=$RUNS cells=${CELLS:-all}"

adb_ push "$(host_path "$SOURCE")" "$DEVICE_DIR/bench-source.png" >/dev/null
adb_ push "$(host_path "$REPO/tools/baseline/corpus.json")" "$DEVICE_DIR/bench-corpus.json" >/dev/null

# Timing automation makes the engine emit spk.stage_timings.v1 for every render; the
# logcat capture below is the native-stage half of the reconciliation evidence.
adb_ shell setprop log.tag.SpektraTiming VERBOSE || true
adb_ logcat -c || true

adb_ shell am force-stop $PKG
adb_ shell am instrument -w -r \
  -e ticket177_phase bench \
  -e ticket177_corpus "$DEVICE_DIR/bench-corpus.json" \
  -e ticket177_source "$DEVICE_DIR/bench-source.png" \
  -e ticket177_runs "$RUNS" \
  -e ticket177_cells "$CELLS" \
  -e ticket177_expect_app_sha256 "$APP_SHA" \
  $PKG.test/$PKG.ReleaseCandidateSmokeInstrumentation | tee "$OUT/instrumentation.txt"

grep -a "TICKET177_BENCH: PASS" "$OUT/instrumentation.txt" >/dev/null || {
  echo "run_bench: instrumentation did not report TICKET177_BENCH: PASS" >&2
  exit 1
}

adb_ logcat -d -s Spektra:I > "$OUT/stage-timings.log" || true
rm -rf "$OUT/capture"
# Pull into a path that does not exist yet: adb would otherwise nest the device
# directory inside it and the report would look for a file one level up.
adb_ pull "$DEVICE_DIR/ticket177" "$(host_path "$OUT/capture")" >/dev/null

GATE=()
[ "$RUNS" -ge 11 ] && GATE=(--gate)
py_ "$REPO/tools/baseline/bench_report.py" "$OUT/capture/capture.json" \
  --expect-app-sha256 "$APP_SHA" \
  --markdown "$OUT/report.md" "${GATE[@]}"
