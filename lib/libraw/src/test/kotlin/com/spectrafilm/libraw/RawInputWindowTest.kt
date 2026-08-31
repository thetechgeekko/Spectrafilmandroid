package com.spectrafilm.libraw

import java.nio.ByteBuffer
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RawInputWindowTest {

    @Test
    fun `direct input exposes only position through limit without mutating caller`() {
        val input = ByteBuffer.allocateDirect(5).apply {
            put(byteArrayOf(90, 11, 12, 13, 91))
            position(1)
            limit(4)
        }

        val window = RawInputWindow.directRemaining(input)
        val actual = ByteArray(window.remaining()).also(window.duplicate()::get)

        assertTrue(window.isDirect)
        assertEquals(0, window.position())
        assertEquals(3, window.capacity())
        assertArrayEquals(byteArrayOf(11, 12, 13), actual)
        assertEquals(1, input.position())
        assertEquals(4, input.limit())
    }

    @Test
    fun `heap input copies only position through limit into a direct window`() {
        val input = ByteBuffer.wrap(byteArrayOf(90, 21, 22, 23, 91)).apply {
            position(1)
            limit(4)
        }

        val window = RawInputWindow.directRemaining(input)
        val actual = ByteArray(window.remaining()).also(window.duplicate()::get)

        assertTrue(window.isDirect)
        assertArrayEquals(byteArrayOf(21, 22, 23), actual)
        assertEquals(1, input.position())
        assertEquals(4, input.limit())
    }
}
