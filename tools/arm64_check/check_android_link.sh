#!/usr/bin/env bash
# Spektrafilm for Android — link the engine the way the ANDROID build does. GPLv3.
# Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
#
# WHY THIS EXISTS
#
# CI has broken twice from the same root cause: a change verified in an
# environment better equipped than CI. The specific trap is documented in
# CLAUDE.md — the HOST parity build compiles with a glob
# (`kernels/*.cpp model/*.cpp ...`) while the ANDROID build enumerates every
# source in CMakeLists.txt by hand. A new .cpp that is missing from CMakeLists
# therefore passes all 39 host gates and fails only at the Android `ninja`
# step, as an undefined reference. Commit 551c57f cost a red CI run exactly
# that way.
#
# The host suite also never compiles spektra_jni.cpp at all (it is `#ifdef
# __ANDROID__` territory), so a JNI-side mistake is invisible to it too.
#
# This script closes both holes without a device and without gradle: it links
# libspektra.so for arm64 with the real NDK clang, the real shipping flags, the
# CMakeLists source list, `-Wl,--no-undefined`, and the project's 16 KB page
# flag — then checks the LOAD alignment CI gates.
#
# USAGE
#   tools/arm64_check/check_android_link.sh [/path/to/ndk]
# Default NDK path is /opt/android-ndk-r27. Get one with:
#   curl -sSLo ndk.zip https://dl.google.com/android/repository/android-ndk-r27-linux.zip
#   unzip -q ndk.zip -d /opt/
set -euo pipefail

NDK_ROOT="${1:-/opt/android-ndk-r27}"
CXX="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++"
LIBDIR="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/24"
CPP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../engine/spektra-core/src/main/cpp" && pwd)"
OUT="${TMPDIR:-/tmp}/libspektra_arm64_check.so"

[ -x "$CXX" ] || { echo "FAIL: no NDK clang at $CXX (pass the NDK path as \$1)"; exit 2; }
cd "$CPP_DIR"

# The source list CMakeLists ENUMERATES. Deliberately not a glob: a glob here
# would defeat the entire purpose of the check.
SRCS=$(grep -oE '(runtime/)?(stages/)?[a-z0-9_]+/[a-z0-9_]+\.cpp|^[[:space:]]+[a-z0-9_]+\.cpp' CMakeLists.txt \
        | tr -d ' ' | sed 's#^stages/#runtime/stages/#' | grep -v '^tests/' | sort -u)
COUNT=$(echo "$SRCS" | wc -l | tr -d ' ')
echo "linking $COUNT enumerated sources for arm64 with $(basename "$CXX")"

# Any source present on disk but ABSENT from CMakeLists is the 551c57f failure
# mode. Report it before linking: the link may still succeed if nothing
# references it yet, which is precisely how it slips through.
ONDISK=$(ls -1 kernels/*.cpp model/*.cpp io/*.cpp profiles/*.cpp runtime/*.cpp \
             runtime/stages/*.cpp gpu/*.cpp 2>/dev/null | sort -u)
MISSING=$(comm -23 <(echo "$ONDISK") <(echo "$SRCS") || true)
if [ -n "$MISSING" ]; then
    echo "FAIL: on disk but NOT in CMakeLists.txt (the host glob build hides this):"
    echo "$MISSING" | sed 's/^/    /'
    exit 1
fi

"$CXX" -std=c++17 -O3 -ffast-math -fno-finite-math-only -fPIC -shared -I. \
    $SRCS -L"$LIBDIR" -landroid -llog \
    -Wl,--no-undefined -Wl,-z,max-page-size=16384 -o "$OUT"

ALIGN=$(readelf -lW "$OUT" | awk '/LOAD/{print $NF}' | sort -u)
echo "LOAD alignment: $ALIGN"
[ "$ALIGN" = "0x4000" ] || { echo "FAIL: expected 0x4000 (16 KB pages), got '$ALIGN'"; exit 1; }

echo "arm64-link: OK ($OUT)"
