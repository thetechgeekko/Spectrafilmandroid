/*
 * Spektrafilm for Android — narrow release-test ABI for ticket #139. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context
import androidx.annotation.Keep

/**
 * Fixed-fixture facade called by the separately shrunk release AndroidTest APK. Production code
 * uses the internal, default-off [Ticket139EditorProbe]; this class deliberately exposes only
 * scalar/context operations and no source URI, recipe, bitmap, flow or native handle.
 */
@Keep
object Ticket139EditorTestBridge {
    private inline fun <T> armed(operation: () -> T): T {
        Ticket139EditorProbe.requireArmed()
        return operation()
    }

    /** Explicitly arm the otherwise inert in-process probe before launching the target Activity. */
    @JvmStatic fun arm() = Ticket139EditorProbe.arm()

    /** Clear all probe state and restore the production-default disarmed state. */
    @JvmStatic fun reset() = Ticket139EditorProbe.reset()

    @JvmStatic fun requestDestination(destination: String): Long =
        armed { Ticket139EditorProbe.requestDestination(destination) }

    @JvmStatic fun requestSourceProbe(label: String): Long =
        armed { Ticket139EditorProbe.requestSourceProbe(label) }

    @JvmStatic fun requestExportProbe(): Long =
        armed { Ticket139EditorProbe.requestExportProbe() }

    @JvmStatic fun currentDestination(): String =
        armed { Ticket139EditorProbe.currentDestination() }

    @JvmStatic fun hostGeneration(): Long = armed { Ticket139EditorProbe.hostGeneration() }

    @JvmStatic fun checkpointGeneration(): Long =
        armed { Ticket139EditorProbe.checkpointGeneration() }

    @JvmStatic fun currentProbeSource(): String =
        armed { Ticket139EditorProbe.currentProbeSource() }

    @JvmStatic fun sourceRetirementGeneration(): Long =
        armed { Ticket139EditorProbe.sourceRetirementGeneration() }

    @JvmStatic fun exportProbeHandledGeneration(): Long =
        armed { Ticket139EditorProbe.exportProbeHandledGeneration() }

    @JvmStatic fun liveEditorReadyGeneration(): Long =
        armed { Ticket139EditorProbe.liveEditorReadyGeneration() }

    @JvmStatic fun livePreviewGeneration(): Long =
        armed { Ticket139EditorProbe.livePreviewGeneration() }

    @JvmStatic fun currentLivePreviewSource(): String =
        armed { Ticket139EditorProbe.currentLivePreviewSource() }

    @JvmStatic fun previewCompletionEnteredGeneration(): Long =
        armed { Ticket139EditorProbe.previewCompletionEnteredGeneration() }

    @JvmStatic fun previewCompletionCleanupGeneration(): Long =
        armed { Ticket139EditorProbe.previewCompletionCleanupGeneration() }

    @JvmStatic fun overlayDraftGeneration(): Long =
        armed { Ticket139EditorProbe.overlayDraftGeneration() }

    @JvmStatic fun currentOverlayDraftTool(): String =
        armed { Ticket139EditorProbe.currentOverlayDraftTool() }

    @JvmStatic fun expectActivityExportRun(runId: Long) =
        armed { Ticket139EditorProbe.expectActivityExportRun(runId) }

    @JvmStatic fun verifyOverlayDraft(generation: Long, expectedTool: String) =
        armed { Ticket139EditorProbe.verifyOverlayDraft(generation, expectedTool) }

    @JvmStatic fun prepareActivityProbe(context: Context) =
        armed { Ticket139EditorProbe.prepareActivityProbe(context) }

    @JvmStatic fun prepareAuthorizedMaskActivityProbe(context: Context) =
        armed { Ticket139EditorProbe.prepareAuthorizedMaskActivityProbe(context) }

    @JvmStatic fun verifyActivityCursor(context: Context, expectedExportPhase: String) =
        armed { Ticket139EditorProbe.verifyActivityCursor(context, expectedExportPhase) }

    @JvmStatic fun prepareProcessDeathProbe(context: Context) =
        armed { Ticket139EditorProbe.prepareProcessDeathProbe(context) }

    @JvmStatic fun beginProcessDeathExportProbe(context: Context): Long =
        armed { Ticket139EditorProbe.beginProcessDeathExportProbe(context) }

    @JvmStatic
    fun verifyProcessDeathSeedAfterLaunch(context: Context, runId: Long): String =
        armed { Ticket139EditorProbe.verifyProcessDeathSeedAfterLaunch(context, runId) }

    @JvmStatic fun abortProcessDeathExportProbe(runId: Long) =
        armed { Ticket139EditorProbe.abortProcessDeathExportProbe(runId) }

    @JvmStatic fun verifyProcessRecoveryBeforeLaunch(context: Context): String =
        armed { Ticket139EditorProbe.verifyProcessRecoveryBeforeLaunch(context) }

    @JvmStatic fun verifyProcessRecoveryAfterLaunch(context: Context) =
        armed { Ticket139EditorProbe.verifyProcessRecoveryAfterLaunch(context) }

    @JvmStatic
    fun verifyLiveEditorCursor(
        context: Context,
        expectedFixture: String,
        expectedExportPhase: String,
    ) = armed {
        Ticket139EditorProbe.verifyLiveEditorCursor(
            context,
            expectedFixture,
            expectedExportPhase,
        )
    }

    @JvmStatic fun armCompletedPreviewProbe(): Long =
        armed { Ticket139EditorProbe.armCompletedPreviewProbe() }

    @JvmStatic fun releaseCompletedPreviewProbe(sequence: Long) =
        armed { Ticket139EditorProbe.releaseCompletedPreviewProbe(sequence) }

    @JvmStatic fun verifyCompletedPreviewProbe(sequence: Long) =
        armed { Ticket139EditorProbe.verifyCompletedPreviewProbe(sequence) }

    @JvmStatic fun verifyRevokedExportProbe(sequence: Long) =
        armed { Ticket139EditorProbe.verifyRevokedExportProbe(sequence) }

    @JvmStatic
    fun verifySourceProbeAfterSwitch(
        context: Context,
        expectedLabel: String,
        retirementBefore: Long,
    ) = armed {
        Ticket139EditorProbe.verifySourceProbeAfterSwitch(
            context,
            expectedLabel,
            retirementBefore,
        )
    }
}
