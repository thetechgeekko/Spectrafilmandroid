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
    private static final String ARG_TICKET158_RAW_URI = "ticket158_raw_uri";
    private static final String ARG_TICKET158_RAW_PATH = "ticket158_raw_path";
    private static final String ARG_TICKET158_REPEATS = "ticket158_repeats";
    private static final String ARG_TICKET158_EXPECTED_SHA256 =
            "ticket158_expected_sha256";
    private static final String ARG_TICKET158_EXPLORATORY =
            "ticket158_exploratory";
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
            final String rawUri = arguments.getString(ARG_TICKET158_RAW_URI, "");
            final String rawPath = arguments.getString(ARG_TICKET158_RAW_PATH, "");
            if (!rawUri.isEmpty() || !rawPath.isEmpty()) {
                require(rawUri.isEmpty() || rawPath.isEmpty(),
                        "provide only one ticket #158 RAW source");
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
            final boolean rawMode =
                    !arguments.getString(ARG_TICKET158_RAW_URI, "").isEmpty()
                            || !arguments.getString(
                                    ARG_TICKET158_RAW_PATH, "").isEmpty();
            final String marker = rawMode
                    ? "TICKET158_RAW_RELEASE_R8"
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
