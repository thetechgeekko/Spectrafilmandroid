package com.spectrafilm.pngwriter

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class PngWriterInputTest {
    @Test
    fun directInputUsesOnlyTheLogicalPositionToLimitRange() {
        val source = ByteBuffer.allocateDirect(10).order(ByteOrder.LITTLE_ENDIAN)
        source.put(byteArrayOf(99, 98, 1, 2, 3, 4, 5, 6, 97, 96))
        source.position(2)
        source.limit(8)

        val packed = packedPngBuffer(source, width = 1, height = 1, bytesPerSample = 2)

        val actual = ByteArray(packed.remaining()).also { packed.duplicate().get(it) }
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6), actual)
        assertTrue(packed.isDirect)
        assertEquals(2, source.position())
        assertEquals(8, source.limit())
    }

    @Test
    fun heapInputIsCopiedFromOnlyTheLogicalWindow() {
        val source = ByteBuffer.wrap(byteArrayOf(99, 98, 1, 2, 3, 4, 5, 6, 97, 96))
        source.position(2)
        source.limit(8)

        val packed = packedPngBuffer(source, width = 1, height = 1, bytesPerSample = 2)

        val actual = ByteArray(packed.remaining()).also { packed.duplicate().get(it) }
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6), actual)
        assertTrue(packed.isDirect)
        assertEquals(2, source.position())
        assertEquals(8, source.limit())
    }

    @Test
    fun malformedAndMisalignedRangesAreRejectedBeforeNativeCode() {
        assertThrows(IllegalArgumentException::class.java) {
            packedPngBuffer(
                ByteBuffer.allocateDirect(5), width = 1, height = 1, bytesPerSample = 2,
            )
        }

        val misaligned = ByteBuffer.allocateDirect(8).apply {
            position(1)
            limit(7)
        }
        assertThrows(IllegalArgumentException::class.java) {
            packedPngBuffer(misaligned, width = 1, height = 1, bytesPerSample = 2)
        }
    }

    @Test
    fun overflowingDimensionsAreRejectedBeforeAllocation() {
        assertThrows(IllegalArgumentException::class.java) {
            checkedPngByteCount(Int.MAX_VALUE, Int.MAX_VALUE, bytesPerSample = 2)
        }
    }

    @Test
    fun invalidOutputPathsAreRejectedBeforeNativeCode() {
        assertThrows(IllegalArgumentException::class.java) {
            PngWriter.write(shortArrayOf(1, 2, 3), 1, 1, "")
        }
        assertThrows(IllegalArgumentException::class.java) {
            PngWriter.write(shortArrayOf(1, 2, 3), 1, 1, "bad\u0000path.png")
        }
    }

    @Test
    fun cancellationIsAtomicIdempotentAndObservedBeforeJni() {
        val token = PngCancellationToken()
        assertFalse(token.isCancelled)
        token.cancel()
        token.cancel()
        assertTrue(token.isCancelled)

        assertThrows(CancellationException::class.java) {
            PngWriter.write(
                rgb16 = shortArrayOf(1, 2, 3),
                width = 1,
                height = 1,
                outPath = "unused.png",
                cancellation = token,
            )
        }
    }
}
