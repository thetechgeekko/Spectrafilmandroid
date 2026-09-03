#!/usr/bin/env bash
# #119 preview-latency capture against an installed release candidate.
#
#   bash tools/baseline/run_preview.sh <app.apk> [runs] [serial]
#
# Times SpektraEngine.simulatePreview at the default 640 px on the decoded corpus
# source, print vs film-scan route, alternating an EV nudge per sample (slider-drag
# settle without the compose/present overhead — the report says so explicitly).
# Run it AFTER run_bench.sh: the bench phase clears the on-device evidence directory.
#
# Film modeling powered by spektrafilm (GPLv3).
set -euo pipefail

APP_APK=${1:?usage: run_preview.sh <app.apk> [runs] [serial]}
RUNS=${2:-15}
SERIAL=${3:-}

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
host_path() { (cd "$(dirname "$1")" && printf "%s/%s" "$(pwd -W 2>/dev/null || pwd)" "$(basename "$1")"); }
OUT=${SPK_BENCH_OUT:-$REPO/build/ticket177-preview}
PKG=com.spectrafilm.app
DEVICE_DIR=/sdcard/Android/data/$PKG/files
py_() { env -u MSYS2_ARG_CONV_EXCL -u MSYS_NO_PATHCONV python "$@"; }

adb_() {
  if [ -n "$SERIAL" ]; then
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 adb -s "$SERIAL" "$@"
  else
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 adb "$@"
  fi
}

command -v adb >/dev/null || { echo "run_preview: adb is required (device-only gate)" >&2; exit 2; }
adb_ get-state >/dev/null 2>&1 || {
  echo "run_preview: no device connected${SERIAL:+ ($SERIAL)}; this gate cannot run without one" >&2
  exit 2
}

mkdir -p "$OUT"
py_ "$REPO/tools/baseline/corpus/make_source.py" --check
SOURCE=$OUT/corpus-source.png
[ -f "$SOURCE" ] || py_ "$REPO/tools/baseline/corpus/make_source.py" --out "$SOURCE"

APP_SHA=$(sha256sum "$APP_APK" | cut -d' ' -f1)
echo "run_preview: app $APP_APK sha256=$APP_SHA runs=$RUNS"

adb_ push "$(host_path "$SOURCE")" "$DEVICE_DIR/bench-source.png" >/dev/null
adb_ push "$(host_path "$REPO/tools/baseline/corpus.json")" "$DEVICE_DIR/bench-corpus.json" >/dev/null

adb_ shell am instrument -w -r \
  -e ticket177_phase preview \
  -e ticket177_corpus "$DEVICE_DIR/bench-corpus.json" \
  -e ticket177_source "$DEVICE_DIR/bench-source.png" \
  -e ticket177_runs "$RUNS" \
  -e ticket177_expect_app_sha256 "$APP_SHA" \
  $PKG.test/$PKG.ReleaseCandidateSmokeInstrumentation | tee "$OUT/instrumentation.txt"

grep -a "TICKET177_PREVIEW: PASS" "$OUT/instrumentation.txt" >/dev/null || {
  echo "run_preview: instrumentation did not report TICKET177_PREVIEW: PASS" >&2
  exit 1
}

adb_ pull "$DEVICE_DIR/ticket177/preview.json" "$(host_path "$OUT/preview.json")" >/dev/null

GATE=()
[ "$RUNS" -ge 10 ] && GATE=(--gate)
py_ "$REPO/tools/baseline/preview_report.py" "$OUT/preview.json" \
  --markdown "$OUT/preview-report.md" "${GATE[@]}"
