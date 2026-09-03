package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import com.spectrafilm.libraw.DecodeStatus
import com.spectrafilm.libraw.RawDecodeException
import com.spectrafilm.libraw.RawInputLimits
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class EngineHelpersSizingTest {
    @Test(expected = IllegalArgumentException::class)
    fun syntheticImageRejectsSinglePixelRamp() {
        syntheticLinearImage(1)
    }

    @Test(expected = IllegalArgumentException::class)
    fun checkedRgbFloatBytesRejectsIntegerOverflow() {
        checkedRgbFloatBytes(50_000, 50_000)
    }

    @Test
    fun cropRejectsTruncatedLogicalSourceWindow() {
        val source = LinearImage(
            ByteBuffer.allocateDirect(47).order(ByteOrder.nativeOrder()),
            width = 2,
            height = 2,
        )

        val failure = runCatching {
            cropLinearImageRect(source, 0.5f, 0.5f, 2, 2)
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
        assertTrue(failure?.message.orEmpty().contains("truncated"))
    }

    @Test
    fun uriRawInputAcceptsTheCanonicalEncodedByteCeiling() {
        assertEquals(
            RawInputLimits.MAX_ENCODED_BYTES,
            checkedRawUriInputCapacity(RawInputLimits.MAX_ENCODED_BYTES.toLong()),
        )
    }

    @Test
    fun uriRawInputRejectsDeclaredSizeAboveCeilingBeforeAllocation() {
        val failure = assertThrows(RawDecodeException::class.java) {
            checkedRawUriInputCapacity(RawInputLimits.MAX_ENCODED_BYTES.toLong() + 1L)
        }

        assertEquals(DecodeStatus.INPUT, failure.status)
    }
}
