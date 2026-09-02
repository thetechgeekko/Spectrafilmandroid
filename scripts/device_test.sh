#!/usr/bin/env bash
# Spektrafilm for Android — one-command on-device instrumented test runner (PR1). GPLv3.
# Film modeling powered by spektrafilm.
#
# Builds the debug + androidTest APKs, installs both on a connected arm64 device (e.g. SM-S948W),
# runs the headless instrumented suite (app/src/androidTest), captures logcat, and prints a
# pass/fail summary + the HTML/XML report paths. Nothing here touches app/src/main or the engine.
#
# Usage:  scripts/device_test.sh
# Env (overridable): ANDROID_SDK_ROOT, JAVA_HOME
set -euo pipefail

# --- repo root (this script lives in scripts/) ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"
GRADLE_FLAGS="${GRADLE_FLAGS:---stacktrace}"

ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
[ -x "$ADB" ] || ADB="adb"

echo "== Spektrafilm device test =="
echo "ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT"
echo "JAVA_HOME=$JAVA_HOME"

# --- 0. require a connected device ---
if ! command -v "$ADB" >/dev/null 2>&1 && [ ! -x "$ADB" ]; then
  echo "ERROR: adb not found (set ANDROID_SDK_ROOT or PATH)." >&2
  exit 2
fi
DEVICES="$("$ADB" devices | awk 'NR>1 && $2=="device" {print $1}')"
if [ -z "$DEVICES" ]; then
  echo "ERROR: no authorized device. Connect the phone, enable USB debugging, accept the RSA prompt:" >&2
  "$ADB" devices >&2
  exit 2
fi
echo "Device(s): $DEVICES"

# --- 1. build debug + androidTest APKs ---
echo "== [1/5] assembleDebug + assembleDebugAndroidTest =="
if ! ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest $GRADLE_FLAGS; then
  echo "Assemble failed — retrying with --info to surface the error:" >&2
  ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest --info || true
  exit 1
fi

# --- 2. install both APKs ---
echo "== [2/5] installDebug + installDebugAndroidTest =="
./gradlew :app:installDebug :app:installDebugAndroidTest $GRADLE_FLAGS

# --- 3. run the instrumented suite (clear logcat first so the capture is this run only) ---
echo "== [3/5] connectedDebugAndroidTest =="
"$ADB" logcat -c || true
RC=0
./gradlew :app:connectedDebugAndroidTest $GRADLE_FLAGS || RC=$?
if [ "$RC" -ne 0 ]; then
  echo "connectedDebugAndroidTest returned $RC — gathering artifacts, then re-run with --info if you need the trace." >&2
fi

# --- 4. capture logcat + scan for native-load failures ---
echo "== [4/5] logcat capture =="
LOGCAT="device_test_logcat.txt"
"$ADB" logcat -d > "$LOGCAT" 2>/dev/null || true
echo "logcat -> $LOGCAT"
if grep -Eiq 'UnsatisfiedLinkError' "$LOGCAT"; then
  echo "!! UnsatisfiedLinkError present in logcat — a JNI .so failed to load:" >&2
  grep -Ei 'UnsatisfiedLinkError|libspektra|libsfraw|libsftiff|libsfpng' "$LOGCAT" | head -40 >&2 || true
  RC=1
else
  echo "No UnsatisfiedLinkError in logcat. Native-lib mentions (if any):"
  grep -Ei 'libspektra|libsfraw|libsftiff|libsfpng' "$LOGCAT" | head -20 || echo "  (none logged)"
fi

# --- 5. report paths + JUnit XML summary ---
echo "== [5/5] reports =="
HTML="$(find app/build/reports/androidTests/connected -name index.html 2>/dev/null | head -1 || true)"
XML_DIR="app/build/outputs/androidTest-results/connected"
echo "HTML report: ${HTML:-<not found>}"
echo "XML results: $XML_DIR/*.xml"

# Extract an integer attribute (e.g. tests="12") from $line; 0 when absent.
attr() {
  local v
  v="$(printf '%s' "$line" | grep -o "$1=\"[0-9]*\"" | grep -o '[0-9]*' | head -1 || true)"
  printf '%s' "${v:-0}"
}

TESTS=0; FAILURES=0; ERRORS=0; SKIPPED=0
while IFS= read -r xml; do
  [ -n "$xml" ] || continue
  line="$(grep -o '<testsuite[^>]*>' "$xml" | head -1 || true)"
  TESTS=$((TESTS + $(attr tests)))
  FAILURES=$((FAILURES + $(attr failures)))
  ERRORS=$((ERRORS + $(attr errors)))
  SKIPPED=$((SKIPPED + $(attr skipped)))
done < <(find "$XML_DIR" -name '*.xml' 2>/dev/null)

echo "-------------------------------------------"
echo "SUMMARY: tests=$TESTS failures=$FAILURES errors=$ERRORS skipped=$SKIPPED"
echo "-------------------------------------------"
if [ "$FAILURES" -ne 0 ] || [ "$ERRORS" -ne 0 ] || [ "$RC" -ne 0 ]; then
  echo "RESULT: FAILED (see $HTML and $LOGCAT)"
  exit 1
fi
echo "RESULT: PASSED"
