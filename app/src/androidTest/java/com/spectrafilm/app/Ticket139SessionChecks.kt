/*
 * Spektrafilm for Android — ticket #139 editor-session device checks. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context

/** Separate-APK, physical-device checks using only production session/render boundaries. */
object Ticket139SessionChecks {
    @JvmStatic
    fun prepareActivityProbe(context: Context) {
        Ticket139EditorTestBridge.arm()
        Ticket139EditorTestBridge.prepareActivityProbe(context)
    }

    @JvmStatic
    fun prepareAuthorizedMaskActivityProbe(context: Context) {
        Ticket139EditorTestBridge.arm()
        Ticket139EditorTestBridge.prepareAuthorizedMaskActivityProbe(context)
    }

    @JvmStatic
    fun verifyActivityCursor(context: Context, expectedExportPhase: String) =
        Ticket139EditorTestBridge.verifyActivityCursor(context, expectedExportPhase)

    @JvmStatic
    fun prepareProcessDeathProbe(context: Context) {
        Ticket139EditorTestBridge.arm()
        Ticket139EditorTestBridge.prepareProcessDeathProbe(context)
    }

    @JvmStatic
    fun beginProcessDeathExportProbe(context: Context): Long =
        Ticket139EditorTestBridge.beginProcessDeathExportProbe(context)

    @JvmStatic
    fun verifyProcessDeathSeedAfterLaunch(context: Context, runId: Long): String =
        Ticket139EditorTestBridge.verifyProcessDeathSeedAfterLaunch(context, runId)

    @JvmStatic
    fun abortProcessDeathExportProbe(runId: Long) =
        Ticket139EditorTestBridge.abortProcessDeathExportProbe(runId)

    @JvmStatic
    fun armProductionSourceSwitch(context: Context) {
        check(providerCall(context, Ticket139SourceProvider.METHOD_ARM)) {
            "ticket #139 source provider did not arm"
        }
    }

    @JvmStatic
    fun revokeProductionSources(context: Context) {
        check(providerCall(context, Ticket139SourceProvider.METHOD_REVOKE)) {
            "ticket #139 source provider did not revoke"
        }
    }

    @JvmStatic
    fun verifyProductionSourceSwitch(context: Context, retirementBefore: Long) =
        Ticket139EditorTestBridge.verifySourceProbeAfterSwitch(context, "B", retirementBefore)

    @JvmStatic
    fun verifyProcessRecoveryBeforeLaunch(context: Context): String {
        Ticket139EditorTestBridge.arm()
        return Ticket139EditorTestBridge.verifyProcessRecoveryBeforeLaunch(context)
    }

    @JvmStatic
    fun verifyProcessRecoveryAfterLaunch(context: Context) =
        Ticket139EditorTestBridge.verifyProcessRecoveryAfterLaunch(context)

    private fun providerCall(context: Context, method: String): Boolean =
        context.contentResolver.call(
            Ticket139SourceProvider.SOURCE_A,
            method,
            context.packageName,
            null,
        )?.getBoolean(Ticket139SourceProvider.KEY_SUCCESS, false) == true

}
