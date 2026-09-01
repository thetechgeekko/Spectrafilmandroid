#!/usr/bin/env bash
# CI emulator smoke for .github/workflows/ci.yml (android-emulator job).
# Lives in a file because reactivecircus/android-emulator-runner runs every
# line of its `script:` input in a separate `sh -c` — variables, heredocs and
# multi-line `if` blocks never survive from one line to the next.
set -euo pipefail

# bash on stdin so the same script is portable across both workflows.
bash -euo pipefail -s <<'EMULATOR_SCRIPT'
mkdir -p emulator-artifacts

APK=$(ls apk/*.apk | head -1)
echo "Installing APK: $APK"
adb install -r "$APK"

ENGINE_TEST_APK=engine-boundary/spektra-core-debug-androidTest.apk
test -f "$ENGINE_TEST_APK"
adb install -r "$ENGINE_TEST_APK"
set +e
adb shell am instrument -w -r \
  com.spectrafilm.engine.test/com.spectrafilm.engine.EngineBoundaryInstrumentation \
  | tee emulator-artifacts/engine-boundary-instrumentation.txt
ENGINE_INSTRUMENT_STATUS=${PIPESTATUS[0]}
set -e
if [ "$ENGINE_INSTRUMENT_STATUS" -ne 0 ]; then
  echo "ENGINE BOUNDARY FAILED: adb instrumentation transport failed"
  exit 1
fi
grep -Fq 'ENGINE_BOUNDARY_INSTRUMENTATION: PASS' \
  emulator-artifacts/engine-boundary-instrumentation.txt
grep -Fx 'INSTRUMENTATION_CODE: -1' \
  emulator-artifacts/engine-boundary-instrumentation.txt
if grep -Fq 'ENGINE_BOUNDARY_INSTRUMENTATION: FAIL' \
    emulator-artifacts/engine-boundary-instrumentation.txt; then
  echo "ENGINE BOUNDARY FAILED: runner reported failure"
  exit 1
fi
printf 'PASS\n' > emulator-artifacts/engine-boundary-instrumentation-api35.txt

echo "--- Launching MainActivity ---"
# -W waits until onResume; captures ShortMsg/LongMsg on crash.
adb shell am start -W \
  -n "com.spectrafilm.app/.MainActivity" \
  -a android.intent.action.MAIN \
  -c android.intent.category.LAUNCHER

echo "--- Waiting 8 s for the app to fully initialise ---"
sleep 8

echo "--- Capturing screenshot ---"
adb exec-out screencap -p > emulator-artifacts/screenshot.png || true

echo "--- Dumping logcat ---"
adb logcat -d > emulator-artifacts/logcat.txt || true

echo "--- Asserting no FATAL EXCEPTION in com.spectrafilm.app ---"
# Android prints "FATAL EXCEPTION: main" and "Process: com.spectrafilm.app"
# on separate lines, so scan the lines *following* each fatal marker for
# our package. UnsatisfiedLinkError lines name our .so directly.
if grep -A4 -E "FATAL EXCEPTION" emulator-artifacts/logcat.txt \
     | grep -q "com.spectrafilm.app" \
   || grep -E "UnsatisfiedLinkError" emulator-artifacts/logcat.txt \
     | grep -q -E "libspektra|libsfraw|libsftiff|com.spectrafilm.app"; then
  echo "SMOKE TEST FAILED: fatal crash or UnsatisfiedLinkError detected in com.spectrafilm.app"
  grep -A6 -E "FATAL EXCEPTION|UnsatisfiedLinkError" emulator-artifacts/logcat.txt || true
  exit 1
fi

echo "--- Asserting MainActivity is the resumed/focused activity ---"
ACTIVITIES=$(adb shell dumpsys activity activities 2>/dev/null || true)
if echo "$ACTIVITIES" | grep -q "com.spectrafilm.app/.MainActivity"; then
  echo "SMOKE TEST PASSED: MainActivity is present in the activity stack."
else
  echo "SMOKE TEST FAILED: com.spectrafilm.app/.MainActivity not found in activity stack."
  echo "$ACTIVITIES" | grep -E "mResumedActivity|mFocusedActivity|TopResumedActivity" || true
  exit 1
fi

echo "--- Smoke test complete ---"
EMULATOR_SCRIPT

