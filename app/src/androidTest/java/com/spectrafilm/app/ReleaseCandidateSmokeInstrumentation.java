/*
 * Spektrafilm for Android — exact release-candidate instrumentation smoke.
 * GPL-3.0-only.
 */
package com.spectrafilm.app;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Platform-only runner: no unpinned AndroidX/JUnit dependency enters the gate APK. */
public final class ReleaseCandidateSmokeInstrumentation extends Instrumentation {
    private static final String TARGET_PACKAGE = "com.spectrafilm.app";
    private static final String ROUTE_PREFIX = "LibRaw Android distribution route: ";
    private static final String ARG_TICKET170_PHASE = "ticket170_phase";
    private static final String TICKET170_PHASE_SEED = "seed";
    private static final String TICKET170_PHASE_RECOVER = "recover";

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
            if (TICKET170_PHASE_SEED.equals(phase)) {
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
            } else if (phase.isEmpty()) {
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
                        "unsupported ticket170_phase: " + phase);
            }
            finish(Activity.RESULT_OK, results);
        } catch (Throwable failure) {
            final String phase = arguments.getString(ARG_TICKET170_PHASE, "");
            final String marker = phase.isEmpty()
                    ? "RELEASE_CANDIDATE_INSTRUMENTATION"
                    : "TICKET170_PROCESS_DEATH_" + phase.toUpperCase();
            results.putString(
                    "stream",
                    marker + ": FAIL\n"
                            + Log.getStackTraceString(failure)
                            + "\n");
            finish(Activity.RESULT_CANCELED, results);
        }
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
