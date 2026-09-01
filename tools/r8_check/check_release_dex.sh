#!/usr/bin/env bash
# Spektrafilm for Android — assert the R8-minified dex still contains every
# member required across JNI and the separately packaged AndroidTest boundary.
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

# With no APK argument, look where :app:assembleRelease puts one. A tool that
# prints a usage line when the obvious artifact is sitting right there invites
# being run once, ignored, and forgotten -- and this check exists precisely
# because a silent absence shipped.
if [ -z "$APK" ]; then
    GUESS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/app/build/outputs/apk/release"
    APK=$(ls -1 "$GUESS"/*.apk 2>/dev/null | head -1 || true)
    [ -n "$APK" ] && echo "no APK given; using $APK"
fi
[ -n "$APK" ] || { echo "usage: $0 <apk> [build-tools-dir]"; echo "  (and no APK found under app/build/outputs/apk/release/ to fall back on)"; exit 2; }
[ -f "$APK" ] || { echo "FAIL: no such APK: $APK"; exit 2; }

DEXDUMP="$BT/dexdump"
# Android build-tools use an .exe suffix on Windows. Git Bash often resolves it
# transparently, but CI shells are not required to; make the host choice explicit.
if [ ! -x "$DEXDUMP" ] && [ -x "$BT/dexdump.exe" ]; then
    DEXDUMP="$BT/dexdump.exe"
fi
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

# Assert a class/member exists in the exact shape used at runtime. Isolating a
# class and pairing each member name with its own type/prototype prevents an
# R8-strengthened return type from falsely passing a name-only check.
rc=0
class_block() {
    local desc="$1"
    awk -v d="Class descriptor  : '$desc'" '
        index($0, d) { inblk=1; next }
        inblk && /Class descriptor  :/ { exit }
        inblk { print }' "$DUMP"
}

check_class() {
    local desc="$1" block
    block=$(class_block "$desc")
    if [ -z "$block" ]; then
        echo "FAIL: class $desc not found in the dex at all (parse failure, or R8 removed the whole class)"
        rc=1
        return
    fi
    echo "  ok   $desc class"
}

class_superclass() {
    local desc="$1" block
    block=$(class_block "$desc")
    sed -n "s/^[[:space:]]*Superclass[[:space:]]*: '\([^']*\)'.*/\1/p" <<< "$block"
}

# Kotlin multifile facades in the locked stdlib are intentionally empty. Calls
# to their static owner resolve through the generated superclass chain, so a
# declaring-member-only check on the facade would be a false failure.
check_superclass_ancestor() {
    local child="$1" ancestor="$2" current="$1" parent steps=0
    while [ "$steps" -lt 32 ]; do
        parent=$(class_superclass "$current")
        if [ -z "$parent" ]; then
            echo "  FAIL $child superclass ancestor $ancestor  <-- hierarchy ended at $current"
            rc=1
            return
        fi
        if [ "$parent" = "$ancestor" ]; then
            echo "  ok   $child superclass ancestor $ancestor"
            return
        fi
        if [ "$parent" = "$current" ] || [ "$parent" = 'Ljava/lang/Object;' ]; then
            echo "  FAIL $child superclass ancestor $ancestor  <-- hierarchy ended at $parent"
            rc=1
            return
        fi
        current="$parent"
        steps=$((steps + 1))
    done
    echo "  FAIL $child superclass ancestor $ancestor  <-- hierarchy exceeded 32 levels"
    rc=1
}

check_direct_interface() {
    local desc="$1" interface="$2" block
    block=$(class_block "$desc")
    if [ -z "$block" ]; then
        echo "FAIL: class $desc not found while checking interface $interface"
        rc=1
        return
    fi
    if grep -Eq "^[[:space:]]*#[0-9]+[[:space:]]*: '$interface'[[:space:]]*$" <<< "$block"; then
        echo "  ok   $desc direct interface $interface"
        return
    fi
    echo "  FAIL $desc direct interface $interface  <-- external invoke-interface contract changed"
    rc=1
}

check_member() {
    local desc="$1" kind="$2" name="$3" expected_type="${4:-}" block
    block=$(class_block "$desc")
    if [ -z "$block" ]; then
        # Class absent entirely: a parse failure or a fully-shrunk class. Either
        # way this is NOT a pass — distinguishing it from "member missing" is
        # what stops a broken parser from reporting success.
        echo "FAIL: class $desc not found in the dex at all (parse failure, or R8 removed the whole class)"
        rc=1
        return
    fi
    if ! grep -q "name          : '$name'" <<< "$block"; then
        echo "  FAIL $desc $kind $name  <-- R8 removed a required runtime-boundary member"
        rc=1
        return
    fi
    if [ -n "$expected_type" ] && ! awk -v n="$name" -v t="$expected_type" '
        BEGIN { q = sprintf("%c", 39) }
        index($0, "name          : " q n q) { candidate=1; next }
        candidate && index($0, "type          : " q t q) { found=1; exit }
        candidate && index($0, "name          : " q) { candidate=0 }
        END { exit(found ? 0 : 1) }' <<< "$block"
    then
        echo "  FAIL $desc $kind $name $expected_type  <-- required member has the wrong field type/method prototype"
        rc=1
        return
    fi
    echo "  ok   $desc $kind $name${expected_type:+ $expected_type}"
}

echo "checking $(basename "$APK") for the members spektra_jni.cpp resolves by string"
for m in getFirst getSecond getThird; do check_member "Lkotlin/Triple;" method "$m" "()Ljava/lang/Object;"; done
for m in getFirst getSecond; do check_member "Lkotlin/Pair;" method "$m" "()Ljava/lang/Object;"; done

# The release instrumentation APK is compiled separately and calls these public
# APIs on the already-minified target APK. R8 cannot see that call edge and may
# otherwise inline/remove cancel(), producing a device-only NoSuchMethodError.
for token in \
    "Lcom/spectrafilm/pngwriter/PngCancellationToken;" \
    "Lcom/spectrafilm/tiffwriter/TiffCancellationToken;"
do
    check_member "$token" method "<init>" "()V"
    check_member "$token" method "cancel" "()V"
done

# App-owned ABI invoked from the separate release AndroidTest APK. A source keep
# rule is not evidence: assert the exact field types and method prototypes that
# the test dex resolves after the target APK has been minified.
check_member 'Lcom/spectrafilm/app/PendingExportBackend$DefaultImpls;' method 'pendingTokens' '(Lcom/spectrafilm/app/PendingExportBackend;)Ljava/util/Set;'
check_member 'Lcom/spectrafilm/app/EngineHolder;' field 'INSTANCE' 'Lcom/spectrafilm/app/EngineHolder;'
check_member 'Lcom/spectrafilm/app/EngineHolder;' method 'get' '(Landroid/content/Context;)Lcom/spectrafilm/engine/SpektraEngine;'
check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' field 'INSTANCE' 'Lcom/spectrafilm/app/ExportWorkRuntime;'
check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'getState' '()Lkotlinx/coroutines/flow/StateFlow;'
check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'launch' '(Landroid/content/Context;Lcom/spectrafilm/app/ExportFormat;JLkotlin/jvm/functions/Function1;)Ljava/lang/Long;'
check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'cancel' '(J)Z'
check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'claimFinished' '(J)Lcom/spectrafilm/app/ExportTerminalOutcome;'
check_member 'Lcom/spectrafilm/app/ExportRuntimeState$Finished;' method 'getRunId' '()J'
check_member 'Lcom/spectrafilm/app/ExportRuntimeState$Finished;' method 'getOutcome' '()Lcom/spectrafilm/app/ExportTerminalOutcome;'
check_member 'Lcom/spectrafilm/app/ExportRuntimeState$Running;' method 'getRunId' '()J'
check_member 'Lcom/spectrafilm/app/ExportTerminalOutcome$Success;' method '<init>' '(Lcom/spectrafilm/app/ExportFormat;JLandroid/graphics/Bitmap;JLcom/spectrafilm/app/ExportPhaseSnapshot;Ljava/lang/String;Ljava/lang/String;)V'
check_member 'Lcom/spectrafilm/app/ExportPhaseSnapshot;' method '<init>' '(JJJJJJ)V'
check_member 'Lcom/spectrafilm/app/ExportFormat;' field 'PNG16' 'Lcom/spectrafilm/app/ExportFormat;'
check_class 'Lcom/spectrafilm/app/ExportRuntimeState$Idle;'
check_class 'Lcom/spectrafilm/app/ExportTerminalOutcome$Cancelled;'

# Ticket #139's separate test APK has exactly one target-app bridge. Assert every descriptor the
# instrumentation resolves so a passing source keep rule can never hide a broken final APK.
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' field 'INSTANCE' 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'arm' '()V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'reset' '()V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'requestDestination' '(Ljava/lang/String;)J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'currentDestination' '()Ljava/lang/String;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'hostGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'checkpointGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'requestSourceProbe' '(Ljava/lang/String;)J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'currentProbeSource' '()Ljava/lang/String;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'sourceRetirementGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'liveEditorReadyGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'livePreviewGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'currentLivePreviewSource' '()Ljava/lang/String;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'previewCompletionEnteredGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'previewCompletionCleanupGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'overlayDraftGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'currentOverlayDraftTool' '()Ljava/lang/String;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'requestExportProbe' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'exportProbeHandledGeneration' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyRevokedExportProbe' '(J)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'prepareActivityProbe' '(Landroid/content/Context;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'prepareAuthorizedMaskActivityProbe' '(Landroid/content/Context;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyActivityCursor' '(Landroid/content/Context;Ljava/lang/String;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'expectActivityExportRun' '(J)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyOverlayDraft' '(JLjava/lang/String;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'prepareProcessDeathProbe' '(Landroid/content/Context;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'beginProcessDeathExportProbe' '(Landroid/content/Context;)J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyProcessDeathSeedAfterLaunch' '(Landroid/content/Context;J)Ljava/lang/String;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'abortProcessDeathExportProbe' '(J)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyProcessRecoveryBeforeLaunch' '(Landroid/content/Context;)Ljava/lang/String;'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyProcessRecoveryAfterLaunch' '(Landroid/content/Context;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifySourceProbeAfterSwitch' '(Landroid/content/Context;Ljava/lang/String;J)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyLiveEditorCursor' '(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'armCompletedPreviewProbe' '()J'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'releaseCompletedPreviewProbe' '(J)V'
check_member 'Lcom/spectrafilm/app/Ticket139EditorTestBridge;' method 'verifyCompletedPreviewProbe' '(J)V'

# Kotlin's inline runCatching/Result calls in the separately packaged release
# AndroidTest APK resolve this JVM ABI from the already-minified target APK.
# Those call edges are invisible to target-app R8. Checking the exact emitted
# members keeps this a physical-dex contract instead of a keep-rule assumption.
check_member 'Lkotlin/Result;' field 'Companion' 'Lkotlin/Result$Companion;'
check_member 'Lkotlin/Result;' method 'constructor-impl' '(Ljava/lang/Object;)Ljava/lang/Object;'
check_member 'Lkotlin/Result;' method 'exceptionOrNull-impl' '(Ljava/lang/Object;)Ljava/lang/Throwable;'
check_class 'Lkotlin/Result$Companion;'
check_class 'Lkotlin/Result$Failure;'
check_member 'Lkotlin/ResultKt;' method 'createFailure' '(Ljava/lang/Throwable;)Ljava/lang/Object;'
check_member 'Lkotlin/ResultKt;' method 'throwOnFailure' '(Ljava/lang/Object;)V'

# Complete non-platform runtime ABI referenced by the one release AndroidTest
# dex: 41 executable/prototype Kotlin/coroutines types, two fields and 35 methods when
# combined with Result above. The test APK defines none of these fallback types.
check_member 'Lkotlin/KotlinNothingValueException;' method '<init>' '()V'
check_superclass_ancestor 'Lkotlin/collections/CollectionsKt;' 'Lkotlin/collections/CollectionsKt__IterablesKt;'
check_superclass_ancestor 'Lkotlin/collections/CollectionsKt;' 'Lkotlin/collections/CollectionsKt__CollectionsKt;'
check_member 'Lkotlin/collections/CollectionsKt__IterablesKt;' method 'collectionSizeOrDefault' '(Ljava/lang/Iterable;I)I'
check_member 'Lkotlin/collections/CollectionsKt__CollectionsKt;' method 'throwCountOverflow' '()V'
check_member 'Lkotlin/collections/IntIterator;' method 'nextInt' '()I'
check_superclass_ancestor 'Lkotlin/coroutines/intrinsics/IntrinsicsKt;' 'Lkotlin/coroutines/intrinsics/IntrinsicsKt__IntrinsicsKt;'
check_member 'Lkotlin/coroutines/intrinsics/IntrinsicsKt__IntrinsicsKt;' method 'getCOROUTINE_SUSPENDED' '()Ljava/lang/Object;'
check_member 'Lkotlin/coroutines/jvm/internal/Boxing;' method 'boxLong' '(J)Ljava/lang/Long;'
check_member 'Lkotlin/jdk7/AutoCloseableKt;' method 'closeFinally' '(Ljava/lang/AutoCloseable;Ljava/lang/Throwable;)V'
check_superclass_ancestor 'Lkotlin/ranges/RangesKt;' 'Lkotlin/ranges/RangesKt___RangesKt;'
check_member 'Lkotlin/ranges/RangesKt___RangesKt;' method 'until' '(II)Lkotlin/ranges/IntRange;'
check_member 'Lkotlin/ranges/IntProgression;' method 'iterator' '()Ljava/util/Iterator;'
check_member 'Lkotlin/ranges/IntProgressionIterator;' method 'hasNext' '()Z'
check_member 'Lkotlin/ranges/IntProgressionIterator;' method 'nextInt' '()I'
check_member 'Lkotlinx/coroutines/AwaitKt;' method 'awaitAll' '(Ljava/util/Collection;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/BuildersKt;' method 'async$default' '(Lkotlinx/coroutines/CoroutineScope;Lkotlin/coroutines/CoroutineContext;Lkotlinx/coroutines/CoroutineStart;Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Lkotlinx/coroutines/Deferred;'
check_member 'Lkotlinx/coroutines/BuildersKt;' method 'launch$default' '(Lkotlinx/coroutines/CoroutineScope;Lkotlin/coroutines/CoroutineContext;Lkotlinx/coroutines/CoroutineStart;Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Lkotlinx/coroutines/Job;'
check_member 'Lkotlinx/coroutines/BuildersKt;' method 'runBlocking$default' '(Lkotlin/coroutines/CoroutineContext;Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/BuildersKt__BuildersKt;' method 'runBlocking$default' '(Lkotlin/coroutines/CoroutineContext;Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/BuildersKt__Builders_commonKt;' method 'async$default' '(Lkotlinx/coroutines/CoroutineScope;Lkotlin/coroutines/CoroutineContext;Lkotlinx/coroutines/CoroutineStart;Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Lkotlinx/coroutines/Deferred;'
check_member 'Lkotlinx/coroutines/BuildersKt__Builders_commonKt;' method 'launch$default' '(Lkotlinx/coroutines/CoroutineScope;Lkotlin/coroutines/CoroutineContext;Lkotlinx/coroutines/CoroutineStart;Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Lkotlinx/coroutines/Job;'
check_class 'Lkotlinx/coroutines/CompletableDeferred;'
check_direct_interface 'Lkotlinx/coroutines/CompletableDeferred;' 'Lkotlinx/coroutines/Deferred;'
check_member 'Lkotlinx/coroutines/Deferred;' method 'await' '(Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/CompletableDeferred;' method 'complete' '(Ljava/lang/Object;)Z'
check_member 'Lkotlinx/coroutines/CompletableDeferredKt;' method 'CompletableDeferred$default' '(Lkotlinx/coroutines/Job;ILjava/lang/Object;)Lkotlinx/coroutines/CompletableDeferred;'
check_member 'Lkotlinx/coroutines/CoroutineScopeKt;' method 'coroutineScope' '(Lkotlin/jvm/functions/Function2;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/DelayKt;' method 'awaitCancellation' '(Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/Dispatchers;' method 'getDefault' '()Lkotlinx/coroutines/CoroutineDispatcher;'
check_member 'Lkotlinx/coroutines/JobKt;' method 'cancelAndJoin' '(Lkotlinx/coroutines/Job;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/JobKt__JobKt;' method 'cancelAndJoin' '(Lkotlinx/coroutines/Job;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/TimeoutKt;' method 'withTimeout' '(JLkotlin/jvm/functions/Function2;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/flow/FlowKt;' method 'first' '(Lkotlinx/coroutines/flow/Flow;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/flow/FlowKt__ReduceKt;' method 'first' '(Lkotlinx/coroutines/flow/Flow;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlin/Unit;' field 'INSTANCE' 'Lkotlin/Unit;'
check_member 'Lkotlin/coroutines/jvm/internal/ContinuationImpl;' method '<init>' '(Lkotlin/coroutines/Continuation;)V'
check_member 'Lkotlin/coroutines/jvm/internal/SuspendLambda;' method '<init>' '(ILkotlin/coroutines/Continuation;)V'
check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'areEqual' '(Ljava/lang/Object;Ljava/lang/Object;)Z'
check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'checkNotNull' '(Ljava/lang/Object;)V'
check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'checkNotNull' '(Ljava/lang/Object;Ljava/lang/String;)V'
check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'checkNotNullExpressionValue' '(Ljava/lang/Object;Ljava/lang/String;)V'
check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'checkNotNullParameter' '(Ljava/lang/Object;Ljava/lang/String;)V'
check_member 'Lkotlinx/coroutines/flow/Flow;' method 'collect' '(Lkotlinx/coroutines/flow/FlowCollector;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/flow/FlowCollector;' method 'emit' '(Ljava/lang/Object;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'
check_member 'Lkotlinx/coroutines/flow/StateFlow;' method 'getValue' '()Ljava/lang/Object;'
for d in \
    'Lkotlin/coroutines/Continuation;' \
    'Lkotlin/coroutines/CoroutineContext;' \
    'Lkotlin/jvm/functions/Function0;' \
    'Lkotlin/jvm/functions/Function1;' \
    'Lkotlin/jvm/functions/Function2;' \
    'Lkotlin/jvm/internal/DefaultConstructorMarker;' \
    'Lkotlin/ranges/IntRange;' \
    'Lkotlinx/coroutines/CoroutineDispatcher;' \
    'Lkotlinx/coroutines/CoroutineScope;' \
    'Lkotlinx/coroutines/CoroutineStart;' \
    'Lkotlinx/coroutines/Deferred;' \
    'Lkotlinx/coroutines/Job;'
do
    check_class "$d"
done

if [ "$rc" -ne 0 ]; then
    echo
    echo "release-dex: FAILED — the minified APK is missing a required JNI/test boundary member."
    echo "Fix the matching keep rule in app/proguard-rules.pro before shipping."
    echo "spektra_jni.cpp also falls back to Pair/Triple backing fields, but do not rely on that alone."
    exit 1
fi
echo "release-dex: OK"
