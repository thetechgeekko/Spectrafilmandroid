"""Offline structural regression tests for the fail-closed release contract."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
CI = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
RELEASE = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
WORKFLOW_README = (ROOT / ".github/workflows/README.md").read_text(encoding="utf-8")
APP_BUILD = (ROOT / "app/build.gradle.kts").read_text(encoding="utf-8")
PROGUARD_RULES = (ROOT / "app/proguard-rules.pro").read_text(encoding="utf-8")
RUNNER = (
    ROOT
    / "app/src/androidTest/java/com/spectrafilm/app/ReleaseCandidateSmokeInstrumentation.java"
).read_text(encoding="utf-8")
PUBLISHER = (ROOT / "tools/release/github_release.py").read_text(encoding="utf-8")
NATIVE_SAFETY_RUNNER = ROOT / "tools/release/run_native_safety.sh"
R8_DEX_CHECK = (ROOT / "tools/r8_check/check_release_dex.sh").read_text(
    encoding="utf-8"
)
ENGINE_BOUNDARY_RUNNER = (
    ROOT
    / "engine/spektra-core/src/androidTest/kotlin/com/spectrafilm/engine/EngineBoundaryInstrumentationTest.kt"
).read_text(encoding="utf-8")
RELEASE_DEVICE_GATE = ROOT / "tools/android/run_release_device_gate.ps1"
# The emulator action runs each `script:` line in its own `sh -c`, so the
# smoke bodies live in files; the contract below inspects job + script.
EMULATOR_SMOKE = (ROOT / "tools/ci/emulator_smoke.sh").read_text(encoding="utf-8")
RELEASE_DEVICE_SMOKE = (ROOT / "tools/ci/release_device_smoke.sh").read_text(
    encoding="utf-8"
)
SPEKTRA_CPP = (ROOT / "engine/spektra-core/src/main/cpp/spektra.cpp").read_text(
    encoding="utf-8"
)


def workflow_job_block(workflow: str, name: str) -> str:
    marker = f"  {name}:\n"
    start = workflow.index(marker)
    match = re.search(r"(?m)^  [a-z0-9][a-z0-9-]*:\n", workflow[start + len(marker) :])
    end = len(workflow) if match is None else start + len(marker) + match.start()
    return workflow[start:end]


def job_block(name: str) -> str:
    return workflow_job_block(RELEASE, name)


def step_block(job: str, name: str) -> str:
    marker = f"      - name: {name}\n"
    start = job.index(marker)
    match = re.search(r"(?m)^      - (?:name:|uses:)", job[start + len(marker) :])
    end = len(job) if match is None else start + len(marker) + match.start()
    return job[start:end]


class ReleaseWorkflowContractTest(unittest.TestCase):
    def test_local_release_device_gate_has_explicit_artifact_or_build_interface(self) -> None:
        self.assertTrue(
            RELEASE_DEVICE_GATE.is_file(),
            "the reusable Windows release-device gate must exist",
        )
        gate = RELEASE_DEVICE_GATE.read_text(encoding="utf-8")
        for contract in (
            "#Requires -Version 5.1",
            "[CmdletBinding(DefaultParameterSetName = 'Artifacts')]",
            "[Parameter(Mandatory = $true, ParameterSetName = 'Artifacts')]",
            "[string]$AppApk",
            "[string]$TestApk",
            "[Parameter(Mandatory = $true, ParameterSetName = 'Build')]",
            "[switch]$Build",
            ":app:assembleRelease",
            ":app:assembleReleaseAndroidTest",
            "--offline",
            "--no-daemon",
        ):
            self.assertIn(contract, gate)

        self.assertIn("function Invoke-Native", gate)
        self.assertIn("$nativeOutput = @()", gate)
        self.assertIn("$exitCode = $null", gate)
        self.assertIn(
            "Remove-Variable LASTEXITCODE -Scope Local -ErrorAction SilentlyContinue",
            gate,
        )
        self.assertIn("$global:LASTEXITCODE = $null", gate)
        self.assertIn("$previousErrorActionPreference = $ErrorActionPreference", gate)
        self.assertIn("$ErrorActionPreference = 'Continue'", gate)
        self.assertIn("$ErrorActionPreference = $previousErrorActionPreference", gate)
        self.assertIn("$exitCode = $global:LASTEXITCODE", gate)
        self.assertIn("if ($null -eq $exitCode)", gate)
        self.assertIn("if ($exitCode -ne 0)", gate)
        self.assertEqual(1, gate.count("& $FilePath @Arguments 2>&1"))
        self.assertNotIn("Start-Process", gate)

    def test_local_release_device_gate_preserves_installed_data_and_signer_identity(self) -> None:
        self.assertTrue(RELEASE_DEVICE_GATE.is_file())
        gate = RELEASE_DEVICE_GATE.read_text(encoding="utf-8")
        for contract in (
            "function Get-SingleSignerSha256",
            "if ($matches.Count -ne 1)",
            "candidate app and test APK signers differ",
            "function Assert-InstalledSignerCompatible",
            "preinstall-target-base.apk",
            "preinstall-test-base.apk",
            "@('install', '--no-streaming', '-r', $AppApk)",
            "@('install', '--no-streaming', '-r', $TestApk)",
            "pm', 'list', 'instrumentation",
            "com.spectrafilm.app.test/com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation",
        ):
            self.assertIn(contract, gate)

        target_precheck = gate.index(
            "Assert-InstalledSignerCompatible $TargetPackage $candidateAppSigner"
        )
        test_precheck = gate.index(
            "Assert-InstalledSignerCompatible $TestPackage $candidateTestSigner"
        )
        first_install = gate.index("@('install', '--no-streaming', '-r', $AppApk)")
        self.assertLess(target_precheck, first_install)
        self.assertLess(test_precheck, first_install)
        self.assertIn("function Remove-ExistingOutput", gate)
        self.assertIn("Remove-ExistingOutput $Destination", gate)
        self.assertNotIn("uninstall", gate.lower())
        self.assertNotIn("pm', 'clear", gate.lower())

    def test_local_release_device_gate_replays_and_proves_installed_bytes(self) -> None:
        self.assertTrue(RELEASE_DEVICE_GATE.is_file())
        gate = RELEASE_DEVICE_GATE.read_text(encoding="utf-8")
        for contract in (
            "@('shell', 'am', 'instrument', '-w', '-r')",
            "foreach ($pass in 1..2)",
            "TICKET174_OUTPUT_DESCRIPTOR: PASS",
            "TICKET170_INJECTED_FAILURES: PASS",
            "TICKET172_ACTIVITY_RECREATION: PASS",
            "RELEASE_CANDIDATE_INSTRUMENTATION: PASS",
            "INSTRUMENTATION_CODE: -1",
            "function Assert-OutputLine",
            "Assert-OutputLine $instrumentOutput 'INSTRUMENTATION_CODE: -1' $Label",
            "instrumentation output contains FAIL",
            "ticket170_phase', 'seed'",
            "TICKET170_PROCESS_DEATH_SEED: PASS token=",
            "am', 'force-stop', $TargetPackage",
            "ticket170_phase', 'recover'",
            "TICKET170_PROCESS_DEATH_RECOVER: PASS token=",
            "process-death recovery token changed",
            "ticket181_phase', 'a11y'",
            "TICKET181_ACCESSIBILITY: PASS",
            "files/ticket181",
            "installed-target-base.apk",
            "installed-test-base.apk",
            "installed target APK hash differs from candidate",
            "installed test APK hash differs from candidate",
            "installed target APK signer differs from candidate",
            "installed test APK signer differs from candidate",
        ):
            self.assertIn(contract, gate)


    def test_release_instrumentation_writer_tokens_survive_r8(self) -> None:
        for token, descriptor in (
            (
                "com.spectrafilm.pngwriter.PngCancellationToken",
                "Lcom/spectrafilm/pngwriter/PngCancellationToken;",
            ),
            (
                "com.spectrafilm.tiffwriter.TiffCancellationToken",
                "Lcom/spectrafilm/tiffwriter/TiffCancellationToken;",
            ),
        ):
            self.assertIn(f"-keep class {token} {{ *; }}", PROGUARD_RULES)
            self.assertIn(descriptor, R8_DEX_CHECK)
        self.assertIn('check_member "$token" method "<init>" "()V"', R8_DEX_CHECK)
        self.assertIn('check_member "$token" method "cancel" "()V"', R8_DEX_CHECK)

    def test_release_instrumentation_kotlin_result_abi_survives_r8(self) -> None:
        for result_type in (
            "kotlin.Result",
            "kotlin.Result$Companion",
            "kotlin.Result$Failure",
            "kotlin.ResultKt",
        ):
            self.assertIn(f"-keep class {result_type} {{ *; }}", PROGUARD_RULES)

        for exact_check in (
            "check_member 'Lkotlin/Result;' field 'Companion' "
            "'Lkotlin/Result$Companion;'",
            "check_member 'Lkotlin/Result;' method 'constructor-impl' "
            "'(Ljava/lang/Object;)Ljava/lang/Object;'",
            "check_member 'Lkotlin/Result;' method 'exceptionOrNull-impl' "
            "'(Ljava/lang/Object;)Ljava/lang/Throwable;'",
            "check_class 'Lkotlin/Result$Companion;'",
            "check_class 'Lkotlin/Result$Failure;'",
            "check_member 'Lkotlin/ResultKt;' method 'createFailure' "
            "'(Ljava/lang/Throwable;)Ljava/lang/Object;'",
            "check_member 'Lkotlin/ResultKt;' method 'throwOnFailure' "
            "'(Ljava/lang/Object;)V'",
        ):
            self.assertIn(exact_check, R8_DEX_CHECK)

    def test_release_dex_gate_checks_exact_native_tuple_prototypes(self) -> None:
        self.assertIn(
            'check_member "Lkotlin/Triple;" method "$m" "()Ljava/lang/Object;"',
            R8_DEX_CHECK,
        )
        self.assertIn(
            'check_member "Lkotlin/Pair;" method "$m" "()Ljava/lang/Object;"',
            R8_DEX_CHECK,
        )

    def test_release_instrumentation_app_owned_abi_survives_r8(self) -> None:
        for boundary_type in (
            "com.spectrafilm.app.EngineHolder",
            "com.spectrafilm.app.ExportWorkRuntime",
            "com.spectrafilm.app.ExportRuntimeState",
            "com.spectrafilm.app.ExportRuntimeState$*",
            "com.spectrafilm.app.ExportTerminalOutcome",
            "com.spectrafilm.app.ExportTerminalOutcome$*",
            "com.spectrafilm.app.ExportPhaseSnapshot",
            "com.spectrafilm.app.ExportFormat",
            "com.spectrafilm.app.PendingExportBackend$DefaultImpls",
        ):
            self.assertIn(f"-keep class {boundary_type} {{ *; }}", PROGUARD_RULES)

        for exact_check in (
            "check_member 'Lcom/spectrafilm/app/PendingExportBackend$DefaultImpls;' "
            "method 'pendingTokens' "
            "'(Lcom/spectrafilm/app/PendingExportBackend;)Ljava/util/Set;'",
            "check_member 'Lcom/spectrafilm/app/EngineHolder;' field 'INSTANCE' "
            "'Lcom/spectrafilm/app/EngineHolder;'",
            "check_member 'Lcom/spectrafilm/app/EngineHolder;' method 'get' "
            "'(Landroid/content/Context;)Lcom/spectrafilm/engine/SpektraEngine;'",
            "check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' field 'INSTANCE' "
            "'Lcom/spectrafilm/app/ExportWorkRuntime;'",
            "check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'getState' "
            "'()Lkotlinx/coroutines/flow/StateFlow;'",
            "check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'launch' "
            "'(Landroid/content/Context;Lcom/spectrafilm/app/ExportFormat;J"
            "Lkotlin/jvm/functions/Function1;)Ljava/lang/Long;'",
            "check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method 'cancel' '(J)Z'",
            "check_member 'Lcom/spectrafilm/app/ExportWorkRuntime;' method "
            "'claimFinished' '(J)Lcom/spectrafilm/app/ExportTerminalOutcome;'",
            "check_member 'Lcom/spectrafilm/app/ExportRuntimeState$Finished;' method "
            "'getRunId' '()J'",
            "check_member 'Lcom/spectrafilm/app/ExportRuntimeState$Finished;' method "
            "'getOutcome' '()Lcom/spectrafilm/app/ExportTerminalOutcome;'",
            "check_member 'Lcom/spectrafilm/app/ExportRuntimeState$Running;' method "
            "'getRunId' '()J'",
            # #162 added publishedUri + publishedMimeType, so the constructor the
            # release AndroidTest dex resolves now carries two trailing Strings.
            "check_member 'Lcom/spectrafilm/app/ExportTerminalOutcome$Success;' method "
            "'<init>' '(Lcom/spectrafilm/app/ExportFormat;JLandroid/graphics/Bitmap;J"
            "Lcom/spectrafilm/app/ExportPhaseSnapshot;Ljava/lang/String;Ljava/lang/String;)V'",
            "check_member 'Lcom/spectrafilm/app/ExportPhaseSnapshot;' method '<init>' "
            "'(JJJJJJ)V'",
            "check_member 'Lcom/spectrafilm/app/ExportFormat;' field 'PNG16' "
            "'Lcom/spectrafilm/app/ExportFormat;'",
            "check_class 'Lcom/spectrafilm/app/ExportRuntimeState$Idle;'",
            "check_class 'Lcom/spectrafilm/app/ExportTerminalOutcome$Cancelled;'",
        ):
            self.assertIn(exact_check, R8_DEX_CHECK)

    def test_release_instrumentation_runtime_abi_survives_r8(self) -> None:
        for kind, runtime_type in (
            ("class", "kotlin.KotlinNothingValueException"),
            ("class", "kotlin.collections.IntIterator"),
            ("class", "kotlin.coroutines.jvm.internal.Boxing"),
            ("class", "kotlin.jdk7.AutoCloseableKt"),
            ("class", "kotlin.ranges.IntRange"),
            ("class", "kotlinx.coroutines.AwaitKt"),
            ("interface", "kotlinx.coroutines.CompletableDeferred"),
            ("class", "kotlinx.coroutines.CompletableDeferredKt"),
            ("interface", "kotlinx.coroutines.CoroutineScope"),
            ("class", "kotlinx.coroutines.CoroutineScopeKt"),
            ("interface", "kotlinx.coroutines.Deferred"),
            ("class", "kotlinx.coroutines.DelayKt"),
            ("class", "kotlinx.coroutines.Dispatchers"),
            ("interface", "kotlinx.coroutines.Job"),
            ("class", "kotlinx.coroutines.TimeoutKt"),
            ("class", "kotlinx.coroutines.CoroutineDispatcher"),
            ("class", "kotlinx.coroutines.CoroutineStart"),
            ("interface", "kotlinx.coroutines.flow.Flow"),
            ("interface", "kotlinx.coroutines.flow.FlowCollector"),
            ("interface", "kotlinx.coroutines.flow.StateFlow"),
        ):
            self.assertIn(f"-keep {kind} {runtime_type} {{ *; }}", PROGUARD_RULES)

        for exact_rule in (
            "-keep class kotlin.collections.CollectionsKt",
            "-keep class kotlin.collections.CollectionsKt__IterablesKt {",
            "public static int collectionSizeOrDefault(java.lang.Iterable, int);",
            "-keep class kotlin.collections.CollectionsKt__CollectionsKt {",
            "public static void throwCountOverflow();",
            "-keep class kotlin.coroutines.intrinsics.IntrinsicsKt",
            "-keep class kotlin.coroutines.intrinsics.IntrinsicsKt__IntrinsicsKt {",
            "public static java.lang.Object getCOROUTINE_SUSPENDED();",
            "-keep class kotlin.ranges.RangesKt",
            "-keep class kotlin.ranges.RangesKt___RangesKt {",
            "public static kotlin.ranges.IntRange until(int, int);",
            "-keep class kotlin.ranges.IntProgression {",
            "public java.util.Iterator iterator();",
            "-keep class kotlin.ranges.IntProgressionIterator {",
            "public boolean hasNext();",
            "public int nextInt();",
            "-keep class kotlinx.coroutines.BuildersKt {",
            "-keep class kotlinx.coroutines.BuildersKt__BuildersKt {",
            "-keep class kotlinx.coroutines.BuildersKt__Builders_commonKt {",
            "-keep class kotlinx.coroutines.JobKt {",
            "-keep class kotlinx.coroutines.JobKt__JobKt {",
            "-keep class kotlinx.coroutines.flow.FlowKt {",
            "-keep class kotlinx.coroutines.flow.FlowKt__ReduceKt {",
        ):
            self.assertIn(exact_rule, PROGUARD_RULES)

        for forbidden_broad_rule in (
            "kotlin.collections.CollectionsKt**",
            "kotlin.coroutines.intrinsics.IntrinsicsKt**",
            "kotlin.ranges.RangesKt**",
            "kotlinx.coroutines.BuildersKt**",
            "kotlinx.coroutines.flow.FlowKt**",
        ):
            self.assertNotIn(forbidden_broad_rule, PROGUARD_RULES)

        self.assertNotIn("printf '%s\\n' \"$block\" |", R8_DEX_CHECK)
        self.assertIn('DEXDUMP="$BT/dexdump.exe"', R8_DEX_CHECK)

        for exact_check in (
            "check_member 'Lkotlin/KotlinNothingValueException;' method '<init>' '()V'",
            "check_superclass_ancestor 'Lkotlin/collections/CollectionsKt;' "
            "'Lkotlin/collections/CollectionsKt__IterablesKt;'",
            "check_superclass_ancestor 'Lkotlin/collections/CollectionsKt;' "
            "'Lkotlin/collections/CollectionsKt__CollectionsKt;'",
            "check_member 'Lkotlin/collections/CollectionsKt__IterablesKt;' method "
            "'collectionSizeOrDefault' '(Ljava/lang/Iterable;I)I'",
            "check_member 'Lkotlin/collections/CollectionsKt__CollectionsKt;' method "
            "'throwCountOverflow' '()V'",
            "check_member 'Lkotlin/collections/IntIterator;' method 'nextInt' '()I'",
            "check_superclass_ancestor 'Lkotlin/coroutines/intrinsics/IntrinsicsKt;' "
            "'Lkotlin/coroutines/intrinsics/IntrinsicsKt__IntrinsicsKt;'",
            "check_member 'Lkotlin/coroutines/intrinsics/IntrinsicsKt__IntrinsicsKt;' method "
            "'getCOROUTINE_SUSPENDED' '()Ljava/lang/Object;'",
            "check_member 'Lkotlin/coroutines/jvm/internal/Boxing;' method 'boxLong' "
            "'(J)Ljava/lang/Long;'",
            "check_member 'Lkotlin/jdk7/AutoCloseableKt;' method 'closeFinally' "
            "'(Ljava/lang/AutoCloseable;Ljava/lang/Throwable;)V'",
            "check_superclass_ancestor 'Lkotlin/ranges/RangesKt;' "
            "'Lkotlin/ranges/RangesKt___RangesKt;'",
            "check_member 'Lkotlin/ranges/RangesKt___RangesKt;' method 'until' "
            "'(II)Lkotlin/ranges/IntRange;'",
            "check_member 'Lkotlin/ranges/IntProgression;' method 'iterator' "
            "'()Ljava/util/Iterator;'",
            "check_member 'Lkotlin/ranges/IntProgressionIterator;' method 'hasNext' '()Z'",
            "check_member 'Lkotlin/ranges/IntProgressionIterator;' method 'nextInt' '()I'",
            "check_member 'Lkotlinx/coroutines/AwaitKt;' method 'awaitAll' "
            "'(Ljava/util/Collection;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/BuildersKt;' method 'async$default' "
            "'(Lkotlinx/coroutines/CoroutineScope;Lkotlin/coroutines/CoroutineContext;"
            "Lkotlinx/coroutines/CoroutineStart;Lkotlin/jvm/functions/Function2;I"
            "Ljava/lang/Object;)Lkotlinx/coroutines/Deferred;'",
            "check_member 'Lkotlinx/coroutines/BuildersKt;' method 'launch$default' "
            "'(Lkotlinx/coroutines/CoroutineScope;Lkotlin/coroutines/CoroutineContext;"
            "Lkotlinx/coroutines/CoroutineStart;Lkotlin/jvm/functions/Function2;I"
            "Ljava/lang/Object;)Lkotlinx/coroutines/Job;'",
            "check_member 'Lkotlinx/coroutines/BuildersKt;' method "
            "'runBlocking$default' '(Lkotlin/coroutines/CoroutineContext;"
            "Lkotlin/jvm/functions/Function2;ILjava/lang/Object;)Ljava/lang/Object;'",
            "check_class 'Lkotlinx/coroutines/CompletableDeferred;'",
            "check_direct_interface 'Lkotlinx/coroutines/CompletableDeferred;' "
            "'Lkotlinx/coroutines/Deferred;'",
            "check_member 'Lkotlinx/coroutines/Deferred;' method 'await' "
            "'(Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/CompletableDeferred;' method 'complete' "
            "'(Ljava/lang/Object;)Z'",
            "check_member 'Lkotlinx/coroutines/CompletableDeferredKt;' method "
            "'CompletableDeferred$default' '(Lkotlinx/coroutines/Job;I"
            "Ljava/lang/Object;)Lkotlinx/coroutines/CompletableDeferred;'",
            "check_member 'Lkotlinx/coroutines/CoroutineScopeKt;' method "
            "'coroutineScope' '(Lkotlin/jvm/functions/Function2;"
            "Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/DelayKt;' method 'awaitCancellation' "
            "'(Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/Dispatchers;' method 'getDefault' "
            "'()Lkotlinx/coroutines/CoroutineDispatcher;'",
            "check_member 'Lkotlinx/coroutines/JobKt;' method 'cancelAndJoin' "
            "'(Lkotlinx/coroutines/Job;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/JobKt__JobKt;' method 'cancelAndJoin' "
            "'(Lkotlinx/coroutines/Job;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/TimeoutKt;' method 'withTimeout' "
            "'(JLkotlin/jvm/functions/Function2;Lkotlin/coroutines/Continuation;)"
            "Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/flow/FlowKt;' method 'first' "
            "'(Lkotlinx/coroutines/flow/Flow;Lkotlin/coroutines/Continuation;)"
            "Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/flow/FlowKt__ReduceKt;' method 'first' "
            "'(Lkotlinx/coroutines/flow/Flow;Lkotlin/coroutines/Continuation;)"
            "Ljava/lang/Object;'",
            "check_member 'Lkotlin/Unit;' field 'INSTANCE' 'Lkotlin/Unit;'",
            "check_member 'Lkotlin/coroutines/jvm/internal/ContinuationImpl;' method "
            "'<init>' '(Lkotlin/coroutines/Continuation;)V'",
            "check_member 'Lkotlin/coroutines/jvm/internal/SuspendLambda;' method "
            "'<init>' '(ILkotlin/coroutines/Continuation;)V'",
            "check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'areEqual' "
            "'(Ljava/lang/Object;Ljava/lang/Object;)Z'",
            "check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'checkNotNull' "
            "'(Ljava/lang/Object;)V'",
            "check_member 'Lkotlin/jvm/internal/Intrinsics;' method 'checkNotNull' "
            "'(Ljava/lang/Object;Ljava/lang/String;)V'",
            "check_member 'Lkotlin/jvm/internal/Intrinsics;' method "
            "'checkNotNullExpressionValue' '(Ljava/lang/Object;Ljava/lang/String;)V'",
            "check_member 'Lkotlin/jvm/internal/Intrinsics;' method "
            "'checkNotNullParameter' '(Ljava/lang/Object;Ljava/lang/String;)V'",
            "check_member 'Lkotlinx/coroutines/flow/Flow;' method 'collect' "
            "'(Lkotlinx/coroutines/flow/FlowCollector;Lkotlin/coroutines/Continuation;)"
            "Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/flow/FlowCollector;' method 'emit' "
            "'(Ljava/lang/Object;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;'",
            "check_member 'Lkotlinx/coroutines/flow/StateFlow;' method 'getValue' "
            "'()Ljava/lang/Object;'",
        ):
            self.assertIn(exact_check, R8_DEX_CHECK)

        for descriptor in (
            "Lkotlin/coroutines/Continuation;",
            "Lkotlin/coroutines/CoroutineContext;",
            "Lkotlin/jvm/functions/Function0;",
            "Lkotlin/jvm/functions/Function1;",
            "Lkotlin/jvm/functions/Function2;",
            "Lkotlin/jvm/internal/DefaultConstructorMarker;",
            "Lkotlin/ranges/IntRange;",
            "Lkotlinx/coroutines/CoroutineDispatcher;",
            "Lkotlinx/coroutines/CoroutineScope;",
            "Lkotlinx/coroutines/CoroutineStart;",
            "Lkotlinx/coroutines/Deferred;",
            "Lkotlinx/coroutines/Job;",
        ):
            self.assertIn(f"'{descriptor}'", R8_DEX_CHECK)

    def test_shared_native_safety_runner_has_exact_suite_inventory(self) -> None:
        self.assertTrue(NATIVE_SAFETY_RUNNER.is_file())
        runner = NATIVE_SAFETY_RUNNER.read_text(encoding="utf-8")
        manifest = re.search(
            r"readonly SUITE_SPECS=\(\n(?P<body>.*?)\n\)",
            runner,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(manifest)
        actual = tuple(
            re.findall(r'^\s*"([^"]+)"\s*$', manifest.group("body"), flags=re.MULTILINE)
        )
        self.assertEqual(
            (
                "asan-ubsan|engine-jni-safety-helpers|engine-jni-safety",
                "asan-ubsan|engine-c-cancellation-abi|engine-c-cancellation",
                "asan-ubsan|engine-parallel-exception-containment|engine-parallel-exceptions",
                "asan-ubsan|engine-tc-lut-cache-bounded-lru|engine-tc-lut-cache",
                "asan-ubsan|engine-json-profile-hostile-inputs|engine-json-profile",
                "asan-ubsan|engine-npy-hostile-inputs|engine-npy",
                "asan-ubsan|png-writer-hostile-jni-helpers|png-writer",
                "asan-ubsan|tiff-writer-hostile-jni-helpers|tiff-writer",
                "tsan|engine-c-cancellation-race|engine-c-cancellation",
                "tsan|engine-jni-allocation-registry-race|engine-jni-safety",
                "tsan|engine-parallel-exception-race|engine-parallel-exceptions",
                "tsan|engine-tc-lut-cache-race|engine-tc-lut-cache",
                "tsan|png-writer-cancellation-race|png-writer",
                "tsan|tiff-writer-cancellation-race|tiff-writer",
            ),
            actual,
        )
        self.assertEqual(8, sum(row.startswith("asan-ubsan|") for row in actual))
        self.assertEqual(6, sum(row.startswith("tsan|") for row in actual))
        self.assertIn('"$ENGINE_CPP/tests/test_npy_lut.cpp"', runner)
        self.assertIn('"$ENGINE_CPP/io/npy_lut.cpp"', runner)
        self.assertIn('"$ENGINE_CPP/tests/test_json_profile.cpp"', runner)
        self.assertIn('"$ENGINE_CPP/profiles/profile.cpp"', runner)
        self.assertIn('"$ENGINE_CPP/runtime/print_digest.cpp"', runner)
        self.assertIn('"$ENGINE_CPP/tests/test_tc_lut_cache.cpp"', runner)
        self.assertIn('"$ENGINE_CPP/runtime/tc_lut_cache.cpp"', runner)
        self.assertIn(
            '"$ENGINE_CPP/../assets/spektra/luts/spectral_upsampling/irradiance_xy_tc.npy"',
            runner,
        )
        self.assertIn('for spec in "${SUITE_SPECS[@]}"', runner)
        self.assertIn('run_suite "$spec"', runner)
        self.assertEqual(1, runner.count('"${command[@]}"'))
        self.assertEqual(1, runner.count('\n  "$output" "${run_args[@]}"\n'))
        self.assertIn('"$ENGINE_CPP/../assets/spektra"', runner)
        self.assertIn('"$ENGINE_CPP/tests/scan_portra_input_rgb.f64"', runner)
        for required in (
            "-fsanitize=address,undefined",
            "-fsanitize=thread",
            "tests/test_jni_safety.cpp",
            "tests/test_cancellation_api.cpp",
            "tests/test_parallel_exceptions.cpp",
            "tests/test_tc_lut_cache.cpp",
            "tests/test_png_writer.cpp",
            "tests/test_tiff_writer.cpp",
        ):
            self.assertIn(required, runner)
        self.assertIn("NATIVE_SAFETY_RUNNER: PASS", runner)
        self.assertNotIn("spektra_jni.cpp", runner)
        self.assertNotIn("|| true", runner)
        for documented_count in (
            "exact fourteen-suite host inventory",
            "locked eight\nASan+UBSan plus six TSan inventory",
            "same committed fourteen-suite ASan+UBSan/TSan runner",
        ):
            self.assertIn(documented_count, WORKFLOW_README)
        self.assertIn("bounded tc_lut-cache lifetime/concurrency", CI)

    def test_engine_rejects_wrong_spectra_lut_shape_before_cache_publication(self) -> None:
        bounded_read = (
            "spk_read_asset(eng, kSpectraLutRel, buf,\n"
            "                            spk::kMaxNpyFileBytes)"
        )
        validation = (
            "eng->spectra_lut.shape != std::vector<int>({192, 192, 81}) ||\n"
            "            eng->spectra_lut.data.size() != 2'985'984u"
        )
        self.assertIn(bounded_read, SPEKTRA_CPP)
        self.assertIn(validation, SPEKTRA_CPP)
        self.assertIn("spektra: spectra LUT shape must be 192x192x81", SPEKTRA_CPP)
        self.assertLess(SPEKTRA_CPP.index(bounded_read), SPEKTRA_CPP.index("spk::parse_npy"))
        self.assertLess(SPEKTRA_CPP.index(validation), SPEKTRA_CPP.index("eng->lut_loaded = true;"))

    def test_ci_and_release_delegate_native_safety_to_the_same_runner(self) -> None:
        ci_qualifier = workflow_job_block(CI, "native-safety-writers")
        release_qualifier = job_block("qualify-native-safety")
        invocation = "bash tools/release/run_native_safety.sh"
        for qualifier in (ci_qualifier, release_qualifier):
            self.assertEqual(1, qualifier.count(invocation))
            self.assertNotRegex(qualifier, r"(?m)^    if:")
            self.assertNotIn("continue-on-error", qualifier)
            self.assertNotIn(f"{invocation} || true", qualifier)
            self.assertNotIn("-fsanitize=", qualifier)
            self.assertNotIn("test_jni_safety.cpp", qualifier)
            self.assertNotIn("test_cancellation_api.cpp", qualifier)
        self.assertIn("JNI safety helpers + engine C cancellation ABI", CI)
        self.assertIn("JNI safety helpers + engine C cancellation ABI", RELEASE)

    def test_ci_json_profile_gates_are_fail_closed_and_bounded(self) -> None:
        qualifier = workflow_job_block(CI, "native-safety-writers")
        self.assertIn("--no-tests=error", qualifier)
        self.assertIn("-R '^spektra\\.json-profile\\.hostile-inputs$'", qualifier)
        for bound in (
            "-runs=1000",
            "-max_total_time=30",
            "-max_len=1048577",
            "-rss_limit_mb=1024",
            "-timeout=10",
        ):
            self.assertEqual(1, qualifier.count(bound))

    def test_engine_boundary_instrumentation_is_a_recurring_ci_gate(self) -> None:
        android = workflow_job_block(CI, "android")
        assemble = step_block(android, "Assemble debug APK")
        self.assertIn(":engine:spektra-core:assembleDebugAndroidTest", assemble)
        self.assertIn("spektra-core-debug-androidTest.apk", android)

        emulator = workflow_job_block(CI, "android-emulator")
        self.assertNotRegex(emulator, r"(?m)^    if:")
        self.assertIn("script: bash tools/ci/emulator_smoke.sh", emulator)
        self.assertIn("set -euo pipefail", EMULATOR_SMOKE)
        emulator += EMULATOR_SMOKE
        self.assertNotIn("github.event_name == 'workflow_dispatch'", emulator)
        self.assertNotIn("continue-on-error: true", emulator)
        self.assertEqual(
            1,
            emulator.count(
                "reactivecircus/android-emulator-runner@"
                "4c44018e59b437e86cdfc41da381398f93ed8808"
            ),
        )
        self.assertNotIn("actions/cache", emulator)
        self.assertNotIn("Create AVD", emulator)
        self.assertIn("api-level: 35", emulator)
        self.assertIn("target: google_apis", emulator)
        self.assertIn("Spektrafilm-engine-boundary-debug-androidTest", emulator)
        self.assertIn(
            "com.spectrafilm.engine.test/com.spectrafilm.engine.EngineBoundaryInstrumentation",
            emulator,
        )
        self.assertIn("grep -Fq 'ENGINE_BOUNDARY_INSTRUMENTATION: PASS'", emulator)
        self.assertIn("grep -Fq 'ENGINE_BOUNDARY_INSTRUMENTATION: FAIL'", emulator)
        self.assertIn("grep -Fx 'INSTRUMENTATION_CODE: -1'", emulator)
        self.assertIn("ENGINE_INSTRUMENT_STATUS=${PIPESTATUS[0]}", emulator)
        self.assertIn("ENGINE_BOUNDARY_INSTRUMENTATION: PASS", ENGINE_BOUNDARY_RUNNER)
        self.assertNotIn("RED:", ENGINE_BOUNDARY_RUNNER)
        self.assertNotIn("not-implemented", ENGINE_BOUNDARY_RUNNER)

    def test_engine_boundary_result_is_hash_and_provenance_bound_in_release(self) -> None:
        candidate = job_block("build-release-candidate")
        assemble = step_block(candidate, "Assemble exact-candidate instrumentation APK")
        self.assertIn(":engine:spektra-core:assembleDebugAndroidTest", assemble)
        self.assertIn("spektra-core-debug-androidTest.apk", assemble)
        self.assertIn("com.spectrafilm.engine.EngineBoundaryInstrumentation", assemble)
        self.assertIn("engine-boundary-debug-androidTest.apk", candidate)

        create = step_block(candidate, "Stage hash-bound candidate and compliance materials")
        protected = job_block("sign-and-publish")
        verify = step_block(protected, "Verify candidate identity, gates, and transport hashes")
        for block in (create, verify):
            self.assertIn("--engine-instrumentation-apk", block)

        device = step_block(
            protected,
            "Run exact signed candidate instrumentation and API smoke",
        )
        self.assertIn("script: bash tools/ci/release_device_smoke.sh", device)
        self.assertIn("set -euo pipefail", RELEASE_DEVICE_SMOKE)
        device += RELEASE_DEVICE_SMOKE
        self.assertIn("adb install --no-streaming -r candidate/engine-boundary-debug-androidTest.apk", device)
        self.assertIn(
            "com.spectrafilm.engine.test/com.spectrafilm.engine.EngineBoundaryInstrumentation",
            device,
        )
        self.assertIn("ENGINE_INSTRUMENT_STATUS=${PIPESTATUS[0]}", device)
        self.assertIn("grep -Fq 'ENGINE_BOUNDARY_INSTRUMENTATION: PASS'", device)
        self.assertIn("grep -Fq 'ENGINE_BOUNDARY_INSTRUMENTATION: FAIL'", device)
        self.assertIn("grep -Fx 'INSTRUMENTATION_CODE: -1'", device)
        self.assertIn("engine-boundary-instrumentation-api35.txt", device)

        provenance = step_block(protected, "Stage signed release artifacts and provenance")
        self.assertIn("engine-boundary-instrumentation-api35.txt", provenance)
        self.assertIn("engine_boundary_instrumentation_apk_sha256=%s", provenance)
        self.assertIn("engine_boundary_instrumentation_evidence_sha256=%s", provenance)
        self.assertIn("gate.engine-boundary-instrumentation-api35=PASS", provenance)

    def test_ci_and_release_jvm_gates_cover_every_shipping_native_module(self) -> None:
        ci_tests = step_block(workflow_job_block(CI, "android"), "Run JVM unit tests")
        release_tests = step_block(
            job_block("build-release-candidate"),
            "Run release JVM tests and lint",
        )
        for task in (
            ":app:testDebugUnitTest",
            ":lib:libraw:testDebugUnitTest",
            ":engine:spektra-core:testDebugUnitTest",
            ":lib:pngwriter:testDebugUnitTest",
            ":lib:tiffwriter:testDebugUnitTest",
        ):
            self.assertEqual(1, ci_tests.count(task), task)
        for task in (
            ":app:testReleaseUnitTest",
            ":lib:libraw:testReleaseUnitTest",
            ":engine:spektra-core:testReleaseUnitTest",
            ":lib:pngwriter:testReleaseUnitTest",
            ":lib:tiffwriter:testReleaseUnitTest",
        ):
            self.assertEqual(1, release_tests.count(task), task)

    def test_exact_candidate_runs_every_required_gate_before_publication(self) -> None:
        candidate = job_block("build-release-candidate")
        protected = job_block("sign-and-publish")
        self.assertIn(
            "needs: [resolve-release, qualify-libraw, qualify-native-safety]",
            candidate,
        )
        self.assertIn(
            "needs: [resolve-release, qualify-libraw, qualify-native-safety, "
            "build-release-candidate]",
            protected,
        )

        candidate_steps = (
            "Reject stale LibRaw qualification from a prior run attempt",
            "Reject stale native safety qualification from a prior run attempt",
            "Run O2 exact engine parity",
            "Run shipping-flags exact engine parity",
            "Run release JVM tests and lint",
            "Assemble unsigned minified release candidate",
            "Assemble exact-candidate instrumentation APK",
            "Verify R8 and 16 KiB candidate invariants",
            "Stage hash-bound candidate and compliance materials",
            "Upload immutable unsigned candidate",
        )
        for left, right in zip(candidate_steps, candidate_steps[1:]):
            self.assertLess(candidate.index(f"- name: {left}"), candidate.index(f"- name: {right}"))

        protected_steps = (
            "Download and verify candidate archive by exact artifact ID and digest",
            "Verify candidate identity, gates, and transport hashes",
            "Sign exact candidate and instrumentation APK",
            "Verify production certificate on both APKs",
            "Verify signed R8, legal, and 16 KiB invariants",
            "Run exact signed candidate instrumentation and API smoke",
            "Stage signed release artifacts and provenance",
            "Attach verified set to GitHub Release",
        )
        for left, right in zip(protected_steps, protected_steps[1:]):
            self.assertLess(protected.index(f"- name: {left}"), protected.index(f"- name: {right}"))
        self.assertIn("script: bash tools/ci/release_device_smoke.sh", protected)
        protected += RELEASE_DEVICE_SMOKE
        for marker in (
            "am instrument -w",
            "INSTRUMENTATION_CODE: -1",
            "RELEASE_CANDIDATE_INSTRUMENTATION: PASS",
            "installed-base.apk",
            "cmp signed/app-release.apk build/device-smoke/installed-base.apk",
        ):
            self.assertIn(marker, protected)

    def test_libraw_qualification_cannot_be_reused_across_run_attempts(self) -> None:
        qualifier = job_block("qualify-libraw")
        candidate = job_block("build-release-candidate")
        for output in (
            "run_id: ${{ steps.qualification-receipt.outputs.run_id }}",
            "run_attempt: ${{ steps.qualification-receipt.outputs.run_attempt }}",
            "source_sha: ${{ steps.qualification-receipt.outputs.source_sha }}",
        ):
            self.assertIn(output, qualifier)
        receipt = step_block(qualifier, "Emit current-attempt LibRaw qualification receipt")
        self.assertIn('echo "run_id=$GITHUB_RUN_ID"', receipt)
        self.assertIn('echo "run_attempt=$GITHUB_RUN_ATTEMPT"', receipt)
        self.assertIn('echo "source_sha=$RELEASE_SHA"', receipt)
        stale = step_block(
            candidate,
            "Reject stale LibRaw qualification from a prior run attempt",
        )
        for binding in (
            "needs.qualify-libraw.outputs.run_id",
            "needs.qualify-libraw.outputs.run_attempt",
            "needs.qualify-libraw.outputs.source_sha",
            '"$QUALIFIED_RUN_ATTEMPT" != "$GITHUB_RUN_ATTEMPT"',
            '"$QUALIFIED_SOURCE_SHA" != "$RELEASE_SHA"',
        ):
            self.assertIn(binding, stale)

    def test_native_safety_qualification_is_run_attempt_bound_and_attested(self) -> None:
        qualifier = job_block("qualify-native-safety")
        candidate = job_block("build-release-candidate")
        for output in (
            "run_id: ${{ steps.qualification-receipt.outputs.run_id }}",
            "run_attempt: ${{ steps.qualification-receipt.outputs.run_attempt }}",
            "source_sha: ${{ steps.qualification-receipt.outputs.source_sha }}",
        ):
            self.assertIn(output, qualifier)
        receipt = step_block(
            qualifier,
            "Emit current-attempt native safety qualification receipt",
        )
        self.assertIn('echo "run_id=$GITHUB_RUN_ID"', receipt)
        self.assertIn('echo "run_attempt=$GITHUB_RUN_ATTEMPT"', receipt)
        self.assertIn('echo "source_sha=$RELEASE_SHA"', receipt)
        stale = step_block(
            candidate,
            "Reject stale native safety qualification from a prior run attempt",
        )
        for binding in (
            "needs.qualify-native-safety.outputs.run_id",
            "needs.qualify-native-safety.outputs.run_attempt",
            "needs.qualify-native-safety.outputs.source_sha",
            '"$QUALIFIED_RUN_ATTEMPT" != "$GITHUB_RUN_ATTEMPT"',
            '"$QUALIFIED_SOURCE_SHA" != "$RELEASE_SHA"',
        ):
            self.assertIn(binding, stale)
        attestation = step_block(
            candidate,
            "Stage hash-bound candidate and compliance materials",
        )
        self.assertIn(
            "printf 'PASS\\n' > build/release-gates/native-safety-sanitizers",
            attestation,
        )

    def test_candidate_download_is_exact_id_and_rerun_bound(self) -> None:
        candidate = job_block("build-release-candidate")
        protected = job_block("sign-and-publish")
        self.assertIn("artifact_id: ${{ steps.candidate-upload.outputs.artifact-id }}", candidate)
        self.assertIn(
            "artifact_digest: ${{ steps.candidate-upload.outputs.artifact-digest }}",
            candidate,
        )
        upload = step_block(candidate, "Upload immutable unsigned candidate")
        self.assertIn("id: candidate-upload", upload)
        self.assertIn("${{ github.run_id }}-${{ github.run_attempt }}", upload)

        download = step_block(
            protected,
            "Download and verify candidate archive by exact artifact ID and digest",
        )
        self.assertIn(
            "actions/artifacts/${CANDIDATE_ARTIFACT_ID}",
            download,
        )
        self.assertIn('"$API/zip" -o build/candidate-artifact.zip', download)
        self.assertIn("sha256sum build/candidate-artifact.zip", download)
        self.assertIn('if [ "$ACTUAL_DIGEST" != "$EXPECTED_DIGEST" ]', download)
        self.assertIn('"digest": f"sha256:{expected_digest}"', download)
        self.assertIn('os.environ["GITHUB_RUN_ID"]', download)
        self.assertIn("EXPECTED_WORKFLOW_SHA: ${{ github.sha }}", download)
        self.assertIn('os.environ["EXPECTED_WORKFLOW_SHA"]', download)
        self.assertNotIn(
            'workflow_run.get("head_sha") != os.environ["RELEASE_SHA"]',
            download,
        )
        self.assertIn("${{ github.run_id }}-${{ github.run_attempt }}", download)
        self.assertNotIn("actions/download-artifact", protected)
        identity = step_block(protected, "Validate exact candidate artifact identity")
        self.assertIn("CANDIDATE_ARTIFACT_DIGEST", identity)
        self.assertIn("^[0-9]+$", identity)

        provenance = step_block(protected, "Stage signed release artifacts and provenance")
        for binding in (
            "run_id=%s",
            "run_attempt=%s",
            "candidate_artifact_id=%s",
            "candidate_artifact_digest=%s",
            "unsigned_apk_sha256=%s",
            "signed_apk_sha256=%s",
            "installed_base_apk_sha256=%s",
        ):
            self.assertIn(binding, provenance)

    def test_candidate_and_release_retain_full_provenance(self) -> None:
        required_candidate_files = (
            "app-release-gate-androidTest.apk",
            "engine-boundary-debug-androidTest.apk",
            "r8-mapping.txt",
            "native-debug-symbols.zip",
            "gradle-locks.zip",
            "release-runtime-classpath.txt",
            "app.spdx.json",
            "libraw.spdx.json",
            "RELEASE_GATE_ATTESTATION.txt",
        )
        for name in required_candidate_files:
            self.assertGreaterEqual(RELEASE.count(name), 2, name)
        for lockfile in (
            "settings-gradle.lockfile",
            "app/gradle.lockfile",
            "engine/spektra-core/gradle.lockfile",
            "lib/libraw/gradle.lockfile",
            "lib/pngwriter/gradle.lockfile",
            "lib/tiffwriter/gradle.lockfile",
        ):
            self.assertGreaterEqual(RELEASE.count(lockfile), 2, lockfile)
        for released_suffix in (
            "-app.spdx.json",
            "-engine-boundary-androidTest.apk",
            "-engine-boundary-instrumentation.txt",
            "-libraw.spdx.json",
            "-r8-mapping.txt",
            "-native-debug-symbols.zip",
            "-gradle-locks.zip",
            "-release-runtime-classpath.txt",
            "-provenance.txt",
        ):
            self.assertIn(released_suffix, RELEASE)
        self.assertIn('debugSymbolLevel = "FULL"', APP_BUILD)
        self.assertIn("lockMode.set(LockMode.STRICT)", (ROOT / "build.gradle.kts").read_text())
        self.assertIn('tasks.register("writeReleaseRuntimeClasspath")', APP_BUILD)
        self.assertIn(".toSortedSet()", APP_BUILD)
        runtime_step = step_block(
            job_block("build-release-candidate"),
            "Capture locked release runtime classpath",
        )
        self.assertEqual(2, runtime_step.count(":app:writeReleaseRuntimeClasspath"))
        self.assertIn(
            "cmp build/release-runtime-classpath.first.txt "
            "build/release-runtime-classpath.txt",
            runtime_step,
        )
        self.assertNotIn(":app:dependencies", runtime_step)

    def test_attestation_is_created_and_reverified_not_trusted_as_text(self) -> None:
        candidate = job_block("build-release-candidate")
        protected = job_block("sign-and-publish")
        create = step_block(candidate, "Stage hash-bound candidate and compliance materials")
        verify = step_block(protected, "Verify candidate identity, gates, and transport hashes")
        self.assertIn("gate_attestation.py create", create)
        self.assertIn("--marker-dir build/release-gates", create)
        self.assertIn("gate_attestation.py verify", verify)
        for block in (create, verify):
            for binding in (
                '--version "$VERSION"',
                '--source-sha "$RELEASE_SHA"',
                '--run-id "$GITHUB_RUN_ID"',
                '--run-attempt "$GITHUB_RUN_ATTEMPT"',
            ):
                self.assertIn(binding, block)

    def test_secret_scope_cleans_key_and_protected_job_never_builds(self) -> None:
        protected = job_block("sign-and-publish")
        sign = step_block(protected, "Sign exact candidate and instrumentation APK")
        cleanup = step_block(protected, "Verify signing material cleanup")
        self.assertNotIn("./gradlew", protected)
        self.assertIn("actions: read", protected)
        self.assertIn("contents: read", protected)
        self.assertNotIn("contents: write", protected)
        self.assertEqual(1, protected.count("secrets.RELEASE_GITHUB_TOKEN"))
        self.assertIn("trap cleanup_key EXIT", sign)
        self.assertIn("--out signed/app-release.apk", sign)
        self.assertIn("--out signed/app-release-gate-androidTest.apk", sign)
        self.assertIn("if: always()", cleanup)
        self.assertIn('test ! -e "$KEYSTORE_FILE"', cleanup)
        self.assertIn("git ls-files --others --exclude-standard", cleanup)
        self.assertLess(protected.index("Sign exact candidate"), protected.index("Verify signing material cleanup"))

    def test_release_variant_and_runner_have_no_debug_fallback(self) -> None:
        release_block = re.search(
            r"buildTypes\s*\{.*?release\s*\{(?P<body>.*?)^\s*}\s*^\s*}",
            APP_BUILD,
            flags=re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(release_block)
        self.assertNotIn('getByName("debug")', release_block.group("body"))
        self.assertIn('testBuildType = "release"', APP_BUILD)
        self.assertIn("void onCreate(Bundle arguments)", RUNNER)
        self.assertIn("super.onCreate(arguments);", RUNNER)
        self.assertIn("start();", RUNNER)

    def test_publisher_requires_immutable_releases_and_exact_ids(self) -> None:
        protected = job_block("sign-and-publish")
        publish = step_block(protected, "Attach verified set to GitHub Release")
        self.assertIn("secrets.RELEASE_GITHUB_TOKEN", publish)
        self.assertIn("tools/release/github_release.py", publish)
        self.assertIn("/immutable-releases", PUBLISHER)
        self.assertIn("/releases/{release_id}", PUBLISHER)
        self.assertIn("/releases/assets/{asset_id}", PUBLISHER)
        self.assertIn('release.get("draft") is not True', PUBLISHER)
        self.assertIn('release.get("immutable") is not True', PUBLISHER)
        self.assertNotIn("gh release delete", RELEASE + PUBLISHER)
        self.assertNotIn("gh release upload", RELEASE + PUBLISHER)


if __name__ == "__main__":
    unittest.main()
