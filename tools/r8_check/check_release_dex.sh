#!/usr/bin/env bash
# Spektrafilm for Android — assert the R8-minified dex still contains every
# member the JNI layer resolves by string. GPLv3.
# Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
#
# WHY THIS EXISTS
#
# R8 removed kotlin.Triple.getFirst/getSecond/getThird (and the kotlin.Pair pair)
# from every release APK this project has shipped. proguard-rules.pro keeps
# com.spectrafilm.engine.**, so GrainParams.getAgxParticleScale() survived and
# returned a real Triple — but nothing in BYTECODE ever calls Triple.getFirst.
# The only caller is spektra_jni.cpp, by literal string, which R8 cannot see. So
# it shrank them as unreachable.
#
# `-dontobfuscate` did not save us: it prevents RENAMING, not REMOVAL.
#
# Consequence: 19 engine params (grain particle scale, density min, uniformity,
# halation scatter/strength, four coupler gammas, camera UV/IR, scanner unsharp,
# crop centre/size) marshalled as 0.0 on every release render, with no crash and
# no log. Zero grain particle scale is degenerate — that is why the slide route
# rendered a flat constant.
#
# NOTHING THAT COMPILES OR LINKS CAN CATCH THIS. The native side builds and links
# fine; the Kotlin side builds fine; the host parity suite never runs R8 or JNI.
# The defect only exists in the shrunk dex. So this check reads the dex.
#
# USAGE
#   tools/r8_check/check_release_dex.sh <path-to-minified.apk> [build-tools-dir]
# In CI, run it after :app:assembleRelease (see .github/workflows/r8-smoke.yml).
set -euo pipefail

APK="${1:-}"
BT="${2:-${ANDROID_HOME:-/opt/android-sdk}/build-tools/35.0.0}"
[ -n "$APK" ] || { echo "usage: $0 <apk> [build-tools-dir]"; exit 2; }
[ -f "$APK" ] || { echo "FAIL: no such APK: $APK"; exit 2; }

DEXDUMP="$BT/dexdump"
# A MISSING TOOL MUST FAIL, NOT PASS. The whole point of this script is to catch
# a silent absence; a silently skipped check would be the same class of bug.
[ -x "$DEXDUMP" ] || { echo "FAIL: dexdump not found at $DEXDUMP — cannot verify the dex. Install build-tools or pass its path as \$2."; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
unzip -oq "$APK" 'classes*.dex' -d "$WORK"
DEXES=$(ls "$WORK"/classes*.dex 2>/dev/null || true)
[ -n "$DEXES" ] || { echo "FAIL: no classes*.dex inside $APK"; exit 2; }

DUMP="$WORK/dump.txt"
for d in $DEXES; do "$DEXDUMP" "$d" >> "$DUMP"; done
[ -s "$DUMP" ] || { echo "FAIL: dexdump produced no output — the parse below would be vacuous"; exit 2; }

# Guard against source drift: if spektra_jni.cpp starts crossing a kotlin type
# this script does not know about, say so rather than silently checking less.
# The original bug was a list (the keep rules) drifting from reality.
JNI="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../engine/spektra-core/src/main/cpp" && pwd)/spektra_jni.cpp"
if [ -f "$JNI" ]; then
    FOUND=$(grep -o 'Lkotlin/[A-Za-z]*;' "$JNI" | sort -u | tr '\n' ' ')
    EXPECTED="Lkotlin/Pair; Lkotlin/Triple; "
    if [ "$FOUND" != "$EXPECTED" ]; then
        echo "FAIL: spektra_jni.cpp crosses kotlin types this check does not cover."
        echo "      found in source: $FOUND"
        echo "      covered here   : $EXPECTED"
        echo "      Extend this script (and proguard-rules.pro) before shipping."
        exit 1
    fi
fi

# Assert a member exists on a class. Isolates the class's own block first, so a
# same-named method on some OTHER class cannot satisfy the check.
rc=0
check_member() {
    local desc="$1" kind="$2" name="$3"
    local block
    block=$(awk -v d="Class descriptor  : '$desc'" '
        index($0, d) { inblk=1; next }
        inblk && /Class descriptor  :/ { exit }
        inblk { print }' "$DUMP")
    if [ -z "$block" ]; then
        # Class absent entirely: a parse failure or a fully-shrunk class. Either
        # way this is NOT a pass — distinguishing it from "member missing" is
        # what stops a broken parser from reporting success.
        echo "FAIL: class $desc not found in the dex at all (parse failure, or R8 removed the whole class)"
        rc=1
        return
    fi
    if echo "$block" | grep -q "name          : '$name'"; then
        echo "  ok   $desc $kind $name"
    else
        echo "  FAIL $desc $kind $name  <-- R8 removed it; JNI resolves it by string and will read 0.0"
        rc=1
    fi
}

echo "checking $(basename "$APK") for the members spektra_jni.cpp resolves by string"
for m in getFirst getSecond getThird; do check_member "Lkotlin/Triple;" method "$m"; done
for m in getFirst getSecond;           do check_member "Lkotlin/Pair;"   method "$m"; done

if [ "$rc" -ne 0 ]; then
    echo
    echo "release-dex: FAILED — the minified APK would marshal Triple/Pair params as 0.0."
    echo "Fix: keep rules in app/proguard-rules.pro (-keep class kotlin.Triple { *; })."
    echo "spektra_jni.cpp also falls back to the backing fields, but do not rely on that alone."
    exit 1
fi
echo "release-dex: OK"
