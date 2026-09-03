package com.spectrafilm.tiffwriter

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TiffWriterInputTest {
    @Test
    fun directFloatInputUsesOnlyTheLogicalPositionToLimitRange() {
        val source = ByteBuffer.allocateDirect(20).order(ByteOrder.LITTLE_ENDIAN)
        source.put(byteArrayOf(99, 98, 97, 96, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 95, 94, 93, 92))
        source.position(4)
        source.limit(16)

        val packed = packedTiffBuffer(source, width = 1, height = 1, bytesPerSample = 4)

        val actual = ByteArray(packed.remaining()).also { packed.duplicate().get(it) }
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), actual)
        assertTrue(packed.isDirect)
        assertEquals(4, source.position())
        assertEquals(16, source.limit())
    }

    @Test
    fun heapFloatInputIsCopiedFromOnlyTheLogicalWindow() {
        val source = ByteBuffer.wrap(
            byteArrayOf(99, 98, 97, 96, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 95, 94, 93, 92),
        )
        source.position(4)
        source.limit(16)

        val packed = packedTiffBuffer(source, width = 1, height = 1, bytesPerSample = 4)

        val actual = ByteArray(packed.remaining()).also { packed.duplicate().get(it) }
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), actual)
        assertTrue(packed.isDirect)
        assertEquals(4, source.position())
        assertEquals(16, source.limit())
    }

    @Test
    fun malformedAndMisalignedRangesAreRejectedBeforeNativeCode() {
        assertThrows(IllegalArgumentException::class.java) {
            packedTiffBuffer(
                ByteBuffer.allocateDirect(11), width = 1, height = 1, bytesPerSample = 4,
            )
        }

        val misaligned = ByteBuffer.allocateDirect(14).apply {
            position(2)
            limit(14)
        }
        assertThrows(IllegalArgumentException::class.java) {
            packedTiffBuffer(misaligned, width = 1, height = 1, bytesPerSample = 4)
        }
    }

    @Test
    fun overflowingDimensionsAreRejectedBeforeAllocation() {
        assertThrows(IllegalArgumentException::class.java) {
            checkedTiffByteCount(Int.MAX_VALUE, Int.MAX_VALUE, bytesPerSample = 4)
        }
    }

    @Test
    fun invalidOutputPathsAreRejectedBeforeNativeCode() {
        assertThrows(IllegalArgumentException::class.java) {
            TiffWriter.write(shortArrayOf(1, 2, 3), 1, 1, "")
        }
        assertThrows(IllegalArgumentException::class.java) {
            TiffWriter.write(shortArrayOf(1, 2, 3), 1, 1, "bad\u0000path.tiff")
        }
    }

    @Test
    fun cancellationIsAtomicIdempotentAndObservedBeforeJni() {
        val token = TiffCancellationToken()
        assertFalse(token.isCancelled)
        token.cancel()
        token.cancel()
        assertTrue(token.isCancelled)

        assertThrows(CancellationException::class.java) {
            TiffWriter.write(
                rgb16 = shortArrayOf(1, 2, 3),
                width = 1,
                height = 1,
                outPath = "unused.tiff",
                cancellation = token,
            )
        }
    }
}
