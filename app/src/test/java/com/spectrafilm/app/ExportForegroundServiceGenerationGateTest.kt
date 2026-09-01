/*
 * Spektrafilm for Android — foreground-service generation race regressions. GPLv3.
 */
package com.spectrafilm.app

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ExportForegroundServiceGenerationGateTest {
    @Test
    fun completionForOlderExportCannotStopServiceProtectingNewerExport() {
        val gate = ExportForegroundServiceGenerationGate()

        gate.recordStart(41L)
        gate.recordStart(42L)

        assertFalse(gate.mayStop(41L))
        assertTrue(gate.mayStop(42L))
    }

    @Test
    fun delayedOlderStartCannotRollGenerationBackward() {
        val gate = ExportForegroundServiceGenerationGate()

        gate.recordStart(42L)
        gate.recordStart(41L)

        assertFalse(gate.mayStop(41L))
        assertTrue(gate.mayStop(42L))
    }

    @Test
    fun matchingRapidCompletionMayStopAfterForegroundCommandIsHandled() {
        val gate = ExportForegroundServiceGenerationGate()

        gate.recordStart(7L)

        assertTrue(gate.mayStop(7L))
    }

    // --- service-owned stop decision (#153): the service observes the runtime state and
    // stops itself; these pin the pure decision against every runtime state shape. ---

    @Test
    fun serviceKeepsRunningWhileItsTrackedExportIsStillRunning() {
        val gate = ExportForegroundServiceGenerationGate()
        gate.recordStart(7L)

        assertFalse(
            exportForegroundStopDecision(
                ExportRuntimeState.Running(7L, ExportFormat.PNG16),
                gate,
            ),
        )
    }

    @Test
    fun serviceStopsOnceTheTrackedExportReachesItsRetainedTerminal() {
        val gate = ExportForegroundServiceGenerationGate()
        gate.recordStart(7L)

        val finished = ExportRuntimeState.Finished(
            7L,
            ExportTerminalOutcome.Cancelled(ExportFormat.PNG16, 0L, 1L),
        )
        assertTrue(exportForegroundStopDecision(finished, gate))
        assertTrue(exportForegroundStopDecision(ExportRuntimeState.Idle, gate))
    }

    @Test
    fun terminalOfOlderExportCannotStopServiceProtectingNewerExport() {
        val gate = ExportForegroundServiceGenerationGate()
        gate.recordStart(7L)
        gate.recordStart(8L)

        // Newer export still running: its own Running state keeps the service alive,
        // and the older generation's retained terminal must not stop it either.
        assertFalse(
            exportForegroundStopDecision(
                ExportRuntimeState.Running(8L, ExportFormat.PNG16),
                gate,
            ),
        )
        assertFalse(
            exportForegroundStopDecision(
                ExportRuntimeState.Finished(
                    7L,
                    ExportTerminalOutcome.Cancelled(ExportFormat.PNG16, 0L, 1L),
                ),
                gate,
            ),
        )
    }
}
