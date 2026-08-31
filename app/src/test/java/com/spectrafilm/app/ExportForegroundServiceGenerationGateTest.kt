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
}
