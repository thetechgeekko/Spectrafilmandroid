package com.spectrafilm.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class ImageBufferSizingTest {

    @Test
    fun rgbFloatByteCount_rejectsDimensionOverflowBeforeAllocation() {
        try {
            checkedRgbFloatByteCount(Int.MAX_VALUE, Int.MAX_VALUE, "bitmap conversion")
            fail("overflowing image dimensions must be rejected")
        } catch (failure: IllegalArgumentException) {
            assertTrue(failure.message.orEmpty().contains("overflow", ignoreCase = true))
        }
    }

    @Test
    fun rgbFloatWindow_rejectsTruncationBeforeExportRead() {
        val truncated = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder()).apply {
            position(4)
            limit(12)
        }

        try {
            checkedRgbFloatWindow(truncated, width = 1, height = 1, label = "export")
            fail("two remaining floats cannot represent one RGB pixel")
        } catch (failure: IllegalArgumentException) {
            assertTrue(failure.message.orEmpty().contains("buffer", ignoreCase = true))
        }
    }
}
