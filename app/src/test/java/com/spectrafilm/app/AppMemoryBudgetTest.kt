package com.spectrafilm.app

import com.spectrafilm.engine.MemoryBudgetStage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class AppMemoryBudgetTest {
    @Test
    fun `physical ram selects bounded tiers without an ART heap input`() {
        assertEquals(DeviceMemoryTier.COMPACT, deviceMemoryPolicy(0L).tier)
        assertEquals(DeviceMemoryTier.COMPACT, deviceMemoryPolicy(4L * GIB).tier)
        assertEquals(DeviceMemoryTier.STANDARD, deviceMemoryPolicy(4L * GIB + 1L).tier)
        assertEquals(DeviceMemoryTier.STANDARD, deviceMemoryPolicy(6L * GIB).tier)
        assertEquals(DeviceMemoryTier.LARGE, deviceMemoryPolicy(6L * GIB + 1L).tier)
        assertEquals(DeviceMemoryTier.LARGE, deviceMemoryPolicy(8L * GIB).tier)
        assertEquals(DeviceMemoryTier.XLARGE, deviceMemoryPolicy(8L * GIB + 1L).tier)
    }

    @Test
    fun `reservation releases exactly once`() {
        val bridge = FakeBridge(nextToken = 41L)
        val reservation = JvmMemoryReservation.acquire(
            123L,
            MemoryBudgetStage.WRITER,
            bridge,
        )

        reservation.close()
        reservation.close()

        assertEquals(listOf(41L), bridge.released)
        assertEquals(123L, bridge.lastBytes)
        assertEquals(MemoryBudgetStage.WRITER, bridge.lastStage)
    }

    @Test
    fun `denial is typed and carries requested stage`() {
        val failure = assertThrows(MemoryBudgetDeniedException::class.java) {
            JvmMemoryReservation.acquire(
                456L,
                MemoryBudgetStage.SPATIAL,
                FakeBridge(nextToken = 0L),
            )
        }

        assertEquals(456L, failure.requestedBytes)
        assertEquals(MemoryBudgetStage.SPATIAL, failure.stage)
    }

    @Test
    fun `invalid reservation is rejected before bridge admission`() {
        val bridge = FakeBridge(nextToken = 1L)
        assertThrows(IllegalArgumentException::class.java) {
            JvmMemoryReservation.acquire(0L, MemoryBudgetStage.UNKNOWN, bridge)
        }
        assertTrue(bridge.lastBytes == null)
    }

    @Test
    fun `admitted token is released when reservation wrapper construction fails`() {
        val bridge = FakeBridge(nextToken = 73L)

        val failure = assertThrows(OutOfMemoryError::class.java) {
            JvmMemoryReservation.acquire(
                bytes = 789L,
                stage = MemoryBudgetStage.JNI_DIRECT_BUFFER,
                bridge = bridge,
                construct = { _, _, _, _ -> throw OutOfMemoryError("injected wrapper failure") },
            )
        }

        assertEquals("injected wrapper failure", failure.message)
        assertEquals(listOf(73L), bridge.released)
    }

    private class FakeBridge(private val nextToken: Long) : MemoryBudgetBridge {
        var lastBytes: Long? = null
        var lastStage: MemoryBudgetStage? = null
        val released = mutableListOf<Long>()

        override fun configure(limitBytes: Long) = Unit

        override fun reserve(bytes: Long, stage: MemoryBudgetStage): Long {
            lastBytes = bytes
            lastStage = stage
            return nextToken
        }

        override fun release(token: Long): Boolean {
            released += token
            return true
        }

        override fun snapshotJson(): String = "{}"
    }

    private companion object {
        const val GIB = 1_073_741_824L
    }
}
