/*
 * Spektrafilm for Android — exact release-candidate instrumentation smoke.
 * GPL-3.0-only.
 */
package com.spectrafilm.app;

import android.Manifest;
import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** Platform-only runner: no unpinned AndroidX/JUnit dependency enters the gate APK. */
public final class ReleaseCandidateSmokeInstrumentation extends Instrumentation {
    private static final String TARGET_PACKAGE = "com.spectrafilm.app";
    private static final String ROUTE_PREFIX = "LibRaw Android distribution route: ";
    private static final String ARG_TICKET170_PHASE = "ticket170_phase";
    private static final String ARG_TICKET139_PHASE = "ticket139_phase";
    private static final String ARG_TICKET158_RAW_URI = "ticket158_raw_uri";
    private static final String ARG_TICKET158_RAW_PATH = "ticket158_raw_path";
    private static final String ARG_TICKET158_REPEATS = "ticket158_repeats";
    private static final String ARG_TICKET158_EXPECTED_SHA256 =
            "ticket158_expected_sha256";
    private static final String ARG_TICKET158_EXPLORATORY =
            "ticket158_exploratory";
    private static final String ARG_TICKET141_WIDTH = "ticket141_width";
    private static final String ARG_TICKET141_HEIGHT = "ticket141_height";
    private static final String ARG_TICKET141_MASKS = "ticket141_masks";
    private static final String ARG_TICKET141_REPEATS = "ticket141_repeats";
    private static final String ARG_TICKET141_FORCE_DENIAL = "ticket141_force_denial";
    private static final String TICKET170_PHASE_SEED = "seed";
    private static final String TICKET170_PHASE_RECOVER = "recover";
    private static final String TICKET139_PHASE_ACTIVITY = "activity";
    private static final String TICKET139_PHASE_SEED = "seed";
    private static final String TICKET139_PHASE_RECOVER = "recover";
    private static final String ARG_TICKET181_PHASE = "ticket181_phase";
    private static final String TICKET181_PHASE_A11Y = "a11y";
    private static final String ARG_TICKET177_PHASE = "ticket177_phase";
    private static final String TICKET177_PHASE_BENCH = "bench";
    private static final String ARG_TICKET177_CORPUS = "ticket177_corpus";
    private static final String ARG_TICKET177_SOURCE = "ticket177_source";
    private static final String ARG_TICKET177_RUNS = "ticket177_runs";
    private static final String ARG_TICKET177_CELLS = "ticket177_cells";
    private static final String ARG_TICKET177_EXPECT_APP_SHA256 =
            "ticket177_expect_app_sha256";

    private Bundle arguments = Bundle.EMPTY;

    @Override
    public void onCreate(Bundle arguments) {
        this.arguments = arguments == null ? Bundle.EMPTY : new Bundle(arguments);
        super.onCreate(arguments);
        start();
    }

    @Override
    public void onStart() {
        final Bundle results = new Bundle();
        try {
            final String phase = arguments.getString(ARG_TICKET170_PHASE, "");
            final String ticket139Phase = arguments.getString(ARG_TICKET139_PHASE, "");
            require(phase.isEmpty() || ticket139Phase.isEmpty(),
                    "ticket #139 and #170 phases are mutually exclusive");
            final String ticket181Phase = arguments.getString(ARG_TICKET181_PHASE, "");
            require(ticket181Phase.isEmpty() || (phase.isEmpty() && ticket139Phase.isEmpty()),
                    "ticket #181 phase is mutually exclusive with ticket #139 and #170 phases");
            final String ticket177Phase = arguments.getString(ARG_TICKET177_PHASE, "");
            require(ticket177Phase.isEmpty()
                            || (phase.isEmpty() && ticket139Phase.isEmpty()
                                    && ticket181Phase.isEmpty()),
                    "ticket #177 phase is mutually exclusive with the other ticket phases");
            final String rawUri = arguments.getString(ARG_TICKET158_RAW_URI, "");
            final String rawPath = arguments.getString(ARG_TICKET158_RAW_PATH, "");
            final boolean ticket141Mode = arguments.containsKey(ARG_TICKET141_WIDTH)
                    || arguments.containsKey(ARG_TICKET141_FORCE_DENIAL);
            if (ticket141Mode) {
                require(phase.isEmpty() && ticket139Phase.isEmpty() && ticket181Phase.isEmpty()
                                && rawUri.isEmpty() && rawPath.isEmpty(),
                        "ticket #141 mode is mutually exclusive with other ticket modes");
                final int width = Integer.parseInt(
                        arguments.getString(ARG_TICKET141_WIDTH, ""));
                final int height = Integer.parseInt(
                        arguments.getString(ARG_TICKET141_HEIGHT, ""));
                final int masks = Integer.parseInt(
                        arguments.getString(ARG_TICKET141_MASKS, "4"));
                final int repeats = Integer.parseInt(
                        arguments.getString(ARG_TICKET141_REPEATS, "2"));
                final boolean forceDenial = Boolean.parseBoolean(
                        arguments.getString(ARG_TICKET141_FORCE_DENIAL, "false"));
                results.putString(
                        "stream",
                        forceDenial
                                ? Ticket141MaskMemoryChecks.runForcedDenial(
                                        width, height, masks)
                                : Ticket141MaskMemoryChecks.run(
                                        getTargetContext(), width, height, masks, repeats));
            } else if (!rawUri.isEmpty() || !rawPath.isEmpty()) {
                require(rawUri.isEmpty() || rawPath.isEmpty(),
                        "provide only one ticket #158 RAW source");
                require(ticket181Phase.isEmpty(),
                        "ticket #181 phase is mutually exclusive with ticket #158 RAW mode");
                final int repeats = Integer.parseInt(
                        arguments.getString(ARG_TICKET158_REPEATS, "3"));
                require(repeats >= 1 && repeats <= 10,
                        "ticket158_repeats must be in [1,10]");
                final String suppliedDigest = arguments.getString(
                        ARG_TICKET158_EXPECTED_SHA256, "").trim();
                final String exploratoryArgument = arguments.getString(
                        ARG_TICKET158_EXPLORATORY, "false");
                require("true".equalsIgnoreCase(exploratoryArgument)
                                || "false".equalsIgnoreCase(exploratoryArgument),
                        "ticket158_exploratory must be true or false");
                final boolean exploratory =
                        Boolean.parseBoolean(exploratoryArgument);
                require(suppliedDigest.isEmpty() == exploratory,
                        "provide a pinned ticket158_expected_sha256, or explicitly "
                                + "select ticket158_exploratory=true, but not both");
                require(suppliedDigest.isEmpty() || isSha256(suppliedDigest),
                        "ticket158_expected_sha256 must be exactly 64 hex digits");
                final String expectedDigest = suppliedDigest.toLowerCase(Locale.ROOT);

                boolean adoptedShellIdentity = false;
                if (!rawUri.isEmpty()) {
                    if (Build.VERSION.SDK_INT >= 33) {
                        getUiAutomation().adoptShellPermissionIdentity(
                                Manifest.permission.READ_MEDIA_IMAGES);
                        adoptedShellIdentity = true;
                    } else if (Build.VERSION.SDK_INT >= 29) {
                        getUiAutomation().adoptShellPermissionIdentity(
                                Manifest.permission.READ_EXTERNAL_STORAGE);
                        adoptedShellIdentity = true;
                    } else {
                        final Context target = getTargetContext();
                        final boolean storageGranted = target.getPackageManager()
                                .checkPermission(
                                        Manifest.permission.READ_EXTERNAL_STORAGE,
                                        target.getPackageName())
                                == PackageManager.PERMISSION_GRANTED;
                        final boolean uriGranted = target.checkUriPermission(
                                Uri.parse(rawUri), Process.myPid(), Process.myUid(),
                                Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                == PackageManager.PERMISSION_GRANTED;
                        require(storageGranted || uriGranted,
                                "content URI needs an already-granted read permission "
                                        + "below API 29");
                    }
                }
                try {
                    results.putString(
                            "stream",
                            Ticket158RawDecodeChecks.run(
                                    getTargetContext(), rawUri, rawPath, repeats,
                                    expectedDigest, exploratory));
                } finally {
                    if (adoptedShellIdentity) {
                        getUiAutomation().dropShellPermissionIdentity();
                    }
                }
            } else if (TICKET170_PHASE_SEED.equals(phase)) {
                final String token = StorageReliabilityChecks.seedProcessDeathProbe(
                        getTargetContext());
                results.putString(
                        "stream",
                        "TICKET170_PROCESS_DEATH_SEED: PASS token=" + token + "\n");
            } else if (TICKET170_PHASE_RECOVER.equals(phase)) {
                final String token = StorageReliabilityChecks.recoverProcessDeathProbe(
                        getTargetContext());
                results.putString(
                        "stream",
                        "TICKET170_PROCESS_DEATH_RECOVER: PASS token=" + token + "\n");
            } else if (TICKET139_PHASE_SEED.equals(ticket139Phase)) {
                final String token = runTicket139ProcessSeed();
                results.putString(
                        "stream",
                        "TICKET139_PROCESS_DEATH_SEED: PASS " + token + "\n");
            } else if (TICKET139_PHASE_RECOVER.equals(ticket139Phase)) {
                final String token = runTicket139ProcessRecovery();
                results.putString(
                        "stream",
                        "TICKET139_PROCESS_DEATH_RECOVER: PASS " + token + "\n");
            } else if (TICKET139_PHASE_ACTIVITY.equals(ticket139Phase)) {
                final String nativeEvidence = runTicket139ActivityChecks();
                results.putString(
                        "stream",
                        "TICKET139_NAVIGATION_RECREATION: PASS "
                                + "(Settings/About/Diagnostics/film-curves/print-curves)\n"
                                + "TICKET139_RAPID_SOURCE_NATIVE_CLOSE: PASS "
                                + nativeEvidence + "\n"
                                + "TICKET139_EXPORT_TERMINAL_EXACTLY_ONCE: PASS\n");
            } else if (TICKET177_PHASE_BENCH.equals(ticket177Phase)) {
                final int runs = Integer.parseInt(
                        arguments.getString(ARG_TICKET177_RUNS, "1"));
                require(runs >= 1 && runs <= 25, "ticket177_runs must be in [1,25]");
                final String corpus = arguments.getString(ARG_TICKET177_CORPUS, "");
                final String source = arguments.getString(ARG_TICKET177_SOURCE, "");
                require(!corpus.isEmpty() && !source.isEmpty(),
                        "ticket177_corpus and ticket177_source are required");
                final String expectedApp = arguments.getString(
                        ARG_TICKET177_EXPECT_APP_SHA256, "").trim().toLowerCase(Locale.ROOT);
                require(isSha256(expectedApp),
                        "ticket177_expect_app_sha256 must be exactly 64 hex digits");
                final String stream = Ticket177BenchmarkChecks.run(
                        getTargetContext(), corpus, source, runs,
                        arguments.getString(ARG_TICKET177_CELLS, ""), expectedApp);
                results.putString("stream", stream);
                if (!stream.contains("TICKET177_BENCH: PASS\n")) {
                    finish(Activity.RESULT_CANCELED, results);
                    return;
                }
            } else if (TICKET181_PHASE_A11Y.equals(ticket181Phase)) {
                final String stream = runTicket181AccessibilityChecks();
                results.putString("stream", stream);
                if (!stream.contains("TICKET181_ACCESSIBILITY: PASS\n")) {
                    finish(Activity.RESULT_CANCELED, results);
                    return;
                }
            } else if (phase.isEmpty() && ticket139Phase.isEmpty() && ticket181Phase.isEmpty()
                    && ticket177Phase.isEmpty()) {
                runCandidateChecks();
                results.putString(
                        "stream",
                        "TICKET174_OUTPUT_DESCRIPTOR: PASS "
                                + "(SDR JPEG/PNG, PNG16, TIFF16, TIFF32F; blocked Ultra HDR)\n"
                                +
                        "TICKET170_INJECTED_FAILURES: PASS "
                                + "(deterministic ENOSPC and interrupted-close fakes)\n"
                                + "TICKET172_ACTIVITY_RECREATION: PASS "
                                + "(native result lifetime survived recreation; publication claimed exactly once)\n"
                                + "RELEASE_CANDIDATE_INSTRUMENTATION: PASS\n");
            } else {
                throw new IllegalArgumentException(
                        !ticket177Phase.isEmpty()
                                ? "unsupported ticket177_phase: " + ticket177Phase
                                : !ticket181Phase.isEmpty()
                                ? "unsupported ticket181_phase: " + ticket181Phase
                                : ticket139Phase.isEmpty()
                                ? "unsupported ticket170_phase: " + phase
                                : "unsupported ticket139_phase: " + ticket139Phase);
            }
            finish(Activity.RESULT_OK, results);
        } catch (Throwable failure) {
            final String phase = arguments.getString(ARG_TICKET170_PHASE, "");
            final String ticket139Phase = arguments.getString(ARG_TICKET139_PHASE, "");
            final boolean ticket181Mode =
                    !arguments.getString(ARG_TICKET181_PHASE, "").isEmpty();
            final boolean rawMode =
                    !arguments.getString(ARG_TICKET158_RAW_URI, "").isEmpty()
                            || !arguments.getString(
                                    ARG_TICKET158_RAW_PATH, "").isEmpty();
            final boolean ticket141Mode = arguments.containsKey(ARG_TICKET141_WIDTH)
                    || arguments.containsKey(ARG_TICKET141_FORCE_DENIAL);
            final String marker = ticket141Mode
                    ? "TICKET141_MASK_MEMORY"
                    : rawMode
                    ? "TICKET158_RAW_RELEASE_R8"
                    : ticket181Mode
                    ? "TICKET181_ACCESSIBILITY"
                    : !ticket139Phase.isEmpty()
                            ? "TICKET139_" + ticket139Phase.toUpperCase(Locale.ROOT)
                    : phase.isEmpty()
                            ? "RELEASE_CANDIDATE_INSTRUMENTATION"
                            : "TICKET170_PROCESS_DEATH_"
                                    + phase.toUpperCase(Locale.ROOT);
            results.putString(
                    "stream",
                    marker + ": FAIL\n"
                            + Log.getStackTraceString(failure)
                            + "\n");
            finish(Activity.RESULT_CANCELED, results);
        }
    }

    /** Ticket #181: platform-only a11y scan over the seeded live editor (ticket #139 route). */
    private String runTicket181AccessibilityChecks() throws Exception {
        final Context targetContext = getTargetContext();
        Ticket139SessionChecks.prepareActivityProbe(targetContext);
        final Intent launch = targetContext.getPackageManager()
                .getLaunchIntentForPackage(TARGET_PACKAGE);
        require(launch != null, "launcher intent missing");
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        final Activity activity = startActivitySync(launch);
        require(activity != null, "MainActivity did not launch for ticket #181");
        try {
            waitForTicket139Destination("EDITOR", 20_000L);
            waitForTicket139LiveReadyAfter(0L, 45_000L);
            waitForTicket139CheckpointAfter(0L, 45_000L);
            waitForTicket139OverlayDraftAfter(0L, "CROP", 60_000L);
            return Ticket181AccessibilityChecks.run(this, activity);
        } finally {
            runOnMainSync(() -> {
                if (!activity.isFinishing()) activity.finish();
            });
            Ticket139EditorTestBridge.reset();
        }
    }

    private String runTicket139ActivityChecks() throws Exception {
        final Context targetContext = getTargetContext();
        Ticket139SessionChecks.prepareActivityProbe(targetContext);
        final Intent launch = targetContext.getPackageManager()
                .getLaunchIntentForPackage(TARGET_PACKAGE);
        require(launch != null, "launcher intent missing");
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        final Activity activity = startActivitySync(launch);
        require(activity != null, "MainActivity did not launch for ticket #139");
        Activity recreated = null;
        long recreationProbe = 0L;
        try {
            waitForTicket139Destination("EDITOR", 20_000L);
            waitForTicket139LiveReadyAfter(0L, 45_000L);
            waitForTicket139CheckpointAfter(0L, 45_000L);
            final long initialCropDraft = waitForTicket139OverlayDraftAfter(
                    0L, "CROP", 60_000L);
            Ticket139EditorTestBridge.verifyOverlayDraft(initialCropDraft, "CROP");
            Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "ACTIVITY", "IDLE");

            final String[] destinations = new String[] {
                    "SETTINGS", "ABOUT", "DIAGNOSTICS", "CURVES_FILM", "CURVES_PRINT"
            };
            for (String destination : destinations) {
                Ticket139EditorTestBridge.requestDestination(destination);
                waitForTicket139Destination(destination, 15_000L);
                // Leaving the editor checkpoints synchronously; the store read flushes the
                // latest-only writer before validating the complete cursor.
                Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
                final long readyBeforeEntry =
                        Ticket139EditorTestBridge.liveEditorReadyGeneration();
                final long cropDraftBeforeEntry =
                        Ticket139EditorTestBridge.overlayDraftGeneration();
                Ticket139EditorTestBridge.requestDestination("EDITOR");
                waitForTicket139Destination("EDITOR", 15_000L);
                waitForTicket139LiveReadyAfter(readyBeforeEntry, 30_000L);
                final long reenteredCropDraft = waitForTicket139OverlayDraftAfter(
                        cropDraftBeforeEntry, "CROP", 45_000L);
                Ticket139EditorTestBridge.verifyOverlayDraft(reenteredCropDraft, "CROP");
                Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
                Ticket139EditorTestBridge.verifyLiveEditorCursor(
                        targetContext, "ACTIVITY", "IDLE");
            }

            final long oldHostGeneration = Ticket139EditorTestBridge.hostGeneration();
            final long readyBeforeRecreation =
                    Ticket139EditorTestBridge.liveEditorReadyGeneration();
            final long cropDraftBeforeRecreation =
                    Ticket139EditorTestBridge.overlayDraftGeneration();
            recreationProbe = StorageReliabilityChecks.beginActivityRecreationExportProbe(
                    targetContext);
            Ticket139EditorTestBridge.expectActivityExportRun(recreationProbe);
            final ActivityMonitor recreationMonitor = addMonitor(
                    "com.spectrafilm.app.MainActivity", null, false);
            try {
                runOnMainSync(activity::recreate);
                recreated = waitForMonitorWithTimeout(recreationMonitor, 20_000L);
                require(recreated != null, "ticket #139 Activity recreation timed out");
                require(recreated != activity, "ticket #139 recreation reused old Activity");
                waitForTicket139HostAfter(oldHostGeneration, 20_000L);
                waitForTicket139Destination("EDITOR", 20_000L);
                waitForTicket139LiveReadyAfter(readyBeforeRecreation, 45_000L);
                final long recreatedCropDraft = waitForTicket139OverlayDraftAfter(
                        cropDraftBeforeRecreation, "CROP", 60_000L);
                Ticket139EditorTestBridge.verifyOverlayDraft(recreatedCropDraft, "CROP");
                Ticket139SessionChecks.verifyActivityCursor(targetContext, "RUNNING");
                Ticket139EditorTestBridge.verifyLiveEditorCursor(
                        targetContext, "ACTIVITY", "RUNNING");
                final long checkpointBeforeTerminal =
                        Ticket139EditorTestBridge.checkpointGeneration();
                StorageReliabilityChecks.completeActivityRecreationExportProbe(
                        targetContext, recreationProbe);
                recreationProbe = 0L;
                waitForTicket139CheckpointAfter(checkpointBeforeTerminal, 20_000L);
                Ticket139SessionChecks.verifyActivityCursor(targetContext, "SUCCESS");
                Ticket139EditorTestBridge.verifyLiveEditorCursor(
                        targetContext, "ACTIVITY", "SUCCESS");
            } finally {
                removeMonitor(recreationMonitor);
            }

            runTicket139ProductionSourceSwitch(targetContext);

            final Activity completedRecreated = recreated;
            runOnMainSync(() -> {
                if (completedRecreated != null && !completedRecreated.isFinishing()) {
                    completedRecreated.finish();
                }
                if (!activity.isFinishing()) activity.finish();
            });
            waitForIdleSync();
            runTicket139AuthorizedMaskDraftChecks(targetContext);
            return "completeOracle=true cropCleanDraft=true maskCleanDraft=true productionSwitch=B "
                    + "cancelObserved=true realSettleGate=true cleanupExact=true";
        } finally {
            if (recreationProbe != 0L) {
                StorageReliabilityChecks.abortActivityRecreationExportProbe(recreationProbe);
            }
            final Activity finalRecreated = recreated;
            runOnMainSync(() -> {
                if (finalRecreated != null && !finalRecreated.isFinishing()) {
                    finalRecreated.finish();
                }
                if (!activity.isFinishing()) activity.finish();
            });
            Ticket139EditorTestBridge.reset();
        }
    }

    private void runTicket139AuthorizedMaskDraftChecks(Context targetContext) throws Exception {
        Ticket139SessionChecks.prepareAuthorizedMaskActivityProbe(targetContext);
        final Intent launch = targetContext.getPackageManager()
                .getLaunchIntentForPackage(TARGET_PACKAGE);
        require(launch != null, "launcher intent missing for authorized mask phase");
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        final Activity activity = startActivitySync(launch);
        require(activity != null, "MainActivity did not launch for authorized mask phase");
        Activity recreated = null;
        try {
            waitForTicket139Destination("EDITOR", 20_000L);
            waitForTicket139LiveReadyAfter(0L, 45_000L);
            waitForTicket139CheckpointAfter(0L, 45_000L);
            // Prove the replacement fixture reached both authorities before waiting on the
            // preview-backed overlay. If this fails, the problem is checkpoint replacement; if
            // it passes but the draft does not appear, the remaining fault is render/composition.
            Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "ACTIVITY", "IDLE");
            final long initialMaskDraft = waitForTicket139OverlayDraftAfter(
                    0L, "MASK_GEOMETRY", 60_000L);
            Ticket139EditorTestBridge.verifyOverlayDraft(initialMaskDraft, "MASK_GEOMETRY");
            Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "ACTIVITY", "IDLE");

            Ticket139EditorTestBridge.requestDestination("SETTINGS");
            waitForTicket139Destination("SETTINGS", 15_000L);
            Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
            final long readyBeforeEntry = Ticket139EditorTestBridge.liveEditorReadyGeneration();
            final long draftBeforeEntry = Ticket139EditorTestBridge.overlayDraftGeneration();
            Ticket139EditorTestBridge.requestDestination("EDITOR");
            waitForTicket139Destination("EDITOR", 15_000L);
            waitForTicket139LiveReadyAfter(readyBeforeEntry, 30_000L);
            final long reenteredMaskDraft = waitForTicket139OverlayDraftAfter(
                    draftBeforeEntry, "MASK_GEOMETRY", 45_000L);
            Ticket139EditorTestBridge.verifyOverlayDraft(reenteredMaskDraft, "MASK_GEOMETRY");
            Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "ACTIVITY", "IDLE");

            final long hostBeforeRecreation = Ticket139EditorTestBridge.hostGeneration();
            final long readyBeforeRecreation =
                    Ticket139EditorTestBridge.liveEditorReadyGeneration();
            final long draftBeforeRecreation =
                    Ticket139EditorTestBridge.overlayDraftGeneration();
            final ActivityMonitor monitor = addMonitor(
                    "com.spectrafilm.app.MainActivity", null, false);
            try {
                runOnMainSync(activity::recreate);
                recreated = waitForMonitorWithTimeout(monitor, 20_000L);
                require(recreated != null, "authorized mask Activity recreation timed out");
                require(recreated != activity, "authorized mask recreation reused old Activity");
                waitForTicket139HostAfter(hostBeforeRecreation, 20_000L);
                waitForTicket139Destination("EDITOR", 20_000L);
                waitForTicket139LiveReadyAfter(readyBeforeRecreation, 45_000L);
                final long recreatedMaskDraft = waitForTicket139OverlayDraftAfter(
                        draftBeforeRecreation, "MASK_GEOMETRY", 60_000L);
                Ticket139EditorTestBridge.verifyOverlayDraft(
                        recreatedMaskDraft, "MASK_GEOMETRY");
                Ticket139SessionChecks.verifyActivityCursor(targetContext, "IDLE");
                Ticket139EditorTestBridge.verifyLiveEditorCursor(
                        targetContext, "ACTIVITY", "IDLE");
            } finally {
                removeMonitor(monitor);
            }
        } finally {
            final Activity finalRecreated = recreated;
            runOnMainSync(() -> {
                if (finalRecreated != null && !finalRecreated.isFinishing()) {
                    finalRecreated.finish();
                }
                if (!activity.isFinishing()) activity.finish();
            });
            waitForIdleSync();
        }
    }

    private String runTicket139ProcessRecovery() throws Exception {
        final Context targetContext = getTargetContext();
        Ticket139EditorTestBridge.reset();
        final String token = Ticket139SessionChecks.verifyProcessRecoveryBeforeLaunch(
                targetContext);
        final Intent launch = targetContext.getPackageManager()
                .getLaunchIntentForPackage(TARGET_PACKAGE);
        require(launch != null, "launcher intent missing");
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        final Activity activity = startActivitySync(launch);
        require(activity != null, "MainActivity did not launch for process recovery");
        try {
            waitForTicket139Destination("EDITOR", 20_000L);
            waitForTicket139LiveReadyAfter(0L, 45_000L);
            waitForTicket139CheckpointAfter(0L, 45_000L);
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "PROCESS", "RECONCILING");
            Ticket139SessionChecks.verifyProcessRecoveryAfterLaunch(targetContext);
            return token;
        } finally {
            runOnMainSync(() -> {
                if (!activity.isFinishing()) activity.finish();
            });
            Ticket139EditorTestBridge.reset();
        }
    }

    private String runTicket139ProcessSeed() throws Exception {
        final Context targetContext = getTargetContext();
        Ticket139SessionChecks.prepareProcessDeathProbe(targetContext);
        final Intent launch = targetContext.getPackageManager()
                .getLaunchIntentForPackage(TARGET_PACKAGE);
        require(launch != null, "launcher intent missing");
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        final Activity activity = startActivitySync(launch);
        require(activity != null, "MainActivity did not launch for process seed");
        long runId = 0L;
        boolean armedForHostKill = false;
        try {
            waitForTicket139Destination("EDITOR", 20_000L);
            waitForTicket139LiveReadyAfter(0L, 45_000L);
            waitForTicket139CheckpointAfter(0L, 45_000L);
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "PROCESS", "IDLE");
            final long revokedExportProbe = Ticket139EditorTestBridge.requestExportProbe();
            waitForTicket139ExportProbe(revokedExportProbe, 10_000L);
            Ticket139EditorTestBridge.verifyRevokedExportProbe(revokedExportProbe);
            final long checkpointBeforeRunning =
                    Ticket139EditorTestBridge.checkpointGeneration();
            runId = Ticket139SessionChecks.beginProcessDeathExportProbe(targetContext);
            waitForTicket139CheckpointAfter(checkpointBeforeRunning, 20_000L);
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "PROCESS", "RUNNING");
            final String token = Ticket139SessionChecks.verifyProcessDeathSeedAfterLaunch(
                    targetContext, runId);
            armedForHostKill = true;
            // Deliberately leave the real Activity + process-owned RUNNING export alive. The host
            // force-stops this exact process before invoking ticket139_phase=recover.
            return token;
        } finally {
            if (!armedForHostKill) {
                if (runId != 0L) {
                    Ticket139SessionChecks.abortProcessDeathExportProbe(runId);
                }
                runOnMainSync(() -> {
                    if (!activity.isFinishing()) activity.finish();
                });
                Ticket139EditorTestBridge.reset();
            }
        }
    }

    private void runTicket139ProductionSourceSwitch(Context targetContext) {
        long completedPreviewProbe = 0L;
        boolean completedPreviewReleased = false;
        Ticket139SessionChecks.armProductionSourceSwitch(targetContext);
        try {
            final long retirementBefore =
                    Ticket139EditorTestBridge.sourceRetirementGeneration();
            final long previewBeforeA = Ticket139EditorTestBridge.livePreviewGeneration();
            Ticket139EditorTestBridge.requestSourceProbe("A");
            waitForTicket139ProbeSource("A", 30_000L);
            waitForTicket139LivePreviewAfter(previewBeforeA, "A", 45_000L);

            // A is now genuinely live. Force one cache-miss settle and hold its completed real
            // SimResult at EditorScreen's production completion-before-publication boundary.
            completedPreviewProbe = Ticket139EditorTestBridge.armCompletedPreviewProbe();
            waitForTicket139PreviewEntered(completedPreviewProbe, 45_000L);
            final long previewBeforeB = Ticket139EditorTestBridge.livePreviewGeneration();
            final long checkpointBeforeB =
                    Ticket139EditorTestBridge.checkpointGeneration();
            Ticket139EditorTestBridge.requestSourceProbe("B");
            waitForTicket139ProbeSource("B", 30_000L);
            waitForTicket139LivePreviewAfter(previewBeforeB, "B", 45_000L);

            Ticket139EditorTestBridge.releaseCompletedPreviewProbe(completedPreviewProbe);
            completedPreviewReleased = true;
            waitForTicket139PreviewCleanup(completedPreviewProbe, 30_000L);
            waitForTicket139CheckpointAfter(checkpointBeforeB, 30_000L);
            Ticket139SessionChecks.verifyProductionSourceSwitch(
                    targetContext, retirementBefore);
            Ticket139EditorTestBridge.verifyLiveEditorCursor(
                    targetContext, "SOURCE_B", "SUCCESS");
            Ticket139EditorTestBridge.verifyCompletedPreviewProbe(completedPreviewProbe);
        } finally {
            if (completedPreviewProbe != 0L && !completedPreviewReleased) {
                Ticket139EditorTestBridge.releaseCompletedPreviewProbe(completedPreviewProbe);
            }
            Ticket139SessionChecks.revokeProductionSources(targetContext);
        }
    }

    private void waitForTicket139Destination(String expected, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (expected.equals(Ticket139EditorTestBridge.currentDestination())) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError(
                "ticket #139 destination timed out: expected=" + expected
                        + " actual=" + Ticket139EditorTestBridge.currentDestination());
    }

    private void waitForTicket139ProbeSource(String expected, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (expected.equals(Ticket139EditorTestBridge.currentProbeSource())) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError(
                "ticket #139 source probe timed out: expected=" + expected
                        + " actual=" + Ticket139EditorTestBridge.currentProbeSource());
    }

    private void waitForTicket139ExportProbe(long expected, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.exportProbeHandledGeneration() >= expected) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError("ticket #139 revoked-source export probe timed out");
    }

    private void waitForTicket139CheckpointAfter(long previous, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.checkpointGeneration() > previous) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError("ticket #139 editor checkpoint timed out");
    }

    private void waitForTicket139LiveReadyAfter(long previous, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.liveEditorReadyGeneration() > previous) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError("ticket #139 live editor readiness timed out");
    }

    private long waitForTicket139OverlayDraftAfter(
            long previous, String expectedTool, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            final long generation = Ticket139EditorTestBridge.overlayDraftGeneration();
            if (generation > previous
                    && expectedTool.equals(
                            Ticket139EditorTestBridge.currentOverlayDraftTool())) {
                return generation;
            }
            SystemClock.sleep(25L);
        }
        throw new AssertionError(
                "ticket #139 overlay draft timed out: expected=" + expectedTool
                        + " actual=" + Ticket139EditorTestBridge.currentOverlayDraftTool());
    }

    private void waitForTicket139LivePreviewAfter(
            long previous, String expectedSource, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.livePreviewGeneration() > previous
                    && expectedSource.equals(
                            Ticket139EditorTestBridge.currentLivePreviewSource())) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError(
                "ticket #139 live preview timed out: expected=" + expectedSource
                        + " actual=" + Ticket139EditorTestBridge.currentLivePreviewSource());
    }

    private void waitForTicket139PreviewEntered(long sequence, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.previewCompletionEnteredGeneration() >= sequence) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError("ticket #139 real completed preview never reached barrier");
    }

    private void waitForTicket139PreviewCleanup(long sequence, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.previewCompletionCleanupGeneration() >= sequence) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError("ticket #139 stale preview cleanup timed out");
    }

    private void waitForTicket139HostAfter(long previous, long timeoutMillis) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMillis;
        while (SystemClock.elapsedRealtime() < deadline) {
            waitForIdleSync();
            if (Ticket139EditorTestBridge.hostGeneration() > previous) return;
            SystemClock.sleep(25L);
        }
        throw new AssertionError("ticket #139 recreated editor host timed out");
    }

    private void runCandidateChecks() throws Exception {
        final Context targetContext = getTargetContext();
        require(TARGET_PACKAGE.equals(targetContext.getPackageName()), "wrong target package");

        final String notice = readAsset(targetContext, "legal/spektrafilm/NOTICE.md");
        final boolean unresolved = notice.contains(ROUTE_PREFIX + "UNRESOLVED.");
        final boolean lgpl = notice.contains(ROUTE_PREFIX + "LGPL-2.1-only.");
        require(unresolved ^ lgpl, "NOTICE must carry exactly one supported canonical route claim");
        final String gpl = readAsset(targetContext, "legal/spektrafilm/LICENSE.GPL-3.0");
        require(gpl.contains("GNU GENERAL PUBLIC LICENSE"), "GPL asset is incomplete");

        OutputContractInstrumentationChecks.run(targetContext);
        StorageReliabilityChecks.run(targetContext);

        final Intent launch = targetContext.getPackageManager()
                .getLaunchIntentForPackage(TARGET_PACKAGE);
        require(launch != null, "launcher intent missing");
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);

        final Activity activity = startActivitySync(launch);
        require(activity != null, "MainActivity did not launch");
        require(
                "com.spectrafilm.app.MainActivity".equals(activity.getClass().getName()),
                "unexpected launch activity: " + activity.getClass().getName());
        waitForIdleSync();

        final long recreationProbe =
                StorageReliabilityChecks.beginActivityRecreationExportProbe(targetContext);
        final ActivityMonitor recreationMonitor = addMonitor(
                "com.spectrafilm.app.MainActivity", null, false);
        Activity recreated = null;
        try {
            runOnMainSync(activity::recreate);
            recreated = waitForMonitorWithTimeout(recreationMonitor, 15_000L);
            require(recreated != null, "MainActivity recreation timed out");
            require(recreated != activity, "Activity.recreate reused the old instance");
            waitForIdleSync();
            StorageReliabilityChecks.completeActivityRecreationExportProbe(
                    targetContext, recreationProbe);
            waitForIdleSync();
        } finally {
            removeMonitor(recreationMonitor);
            StorageReliabilityChecks.abortActivityRecreationExportProbe(recreationProbe);
            final Activity finalRecreated = recreated;
            runOnMainSync(() -> {
                if (finalRecreated != null && !finalRecreated.isFinishing()) {
                    finalRecreated.finish();
                }
                if (!activity.isFinishing()) activity.finish();
            });
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static boolean isSha256(String digest) {
        if (digest.length() != 64) return false;
        for (int index = 0; index < digest.length(); index++) {
            final char value = digest.charAt(index);
            final boolean decimal = value >= '0' && value <= '9';
            final boolean lower = value >= 'a' && value <= 'f';
            final boolean upper = value >= 'A' && value <= 'F';
            if (!decimal && !lower && !upper) return false;
        }
        return true;
    }

    private static String readAsset(Context context, String path) throws Exception {
        try (InputStream input = context.getAssets().open(path);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            final byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }
}
