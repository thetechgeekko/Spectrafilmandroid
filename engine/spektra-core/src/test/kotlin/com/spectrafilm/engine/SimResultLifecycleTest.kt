package com.spectrafilm.engine

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SimResultLifecycleTest {
    private val nonNativeOrder: ByteOrder
        get() = if (ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN) {
            ByteOrder.BIG_ENDIAN
        } else {
            ByteOrder.LITTLE_ENDIAN
        }

    private fun result(onRelease: () -> Unit): SimResult =
        SimResult.forTest(
            data = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            colorSpace = ColorSpace.SRGB,
            renderId = 0L,
            release = { onRelease() },
        )

    @Test
    fun publicApiRequiresAnExplicitDataLease() {
        assertTrue(SimResult::class.java.methods.none { it.name == "withDataLease" })
        assertEquals(
            DataLease::class.java,
            SimResult::class.java.getDeclaredMethod("acquireDataLease").returnType,
        )
    }

    @Test
    fun closeWaitsForActiveReaderBeforeFreeingNativeBuffer() {
        val entered = CountDownLatch(1)
        val releaseReader = CountDownLatch(1)
        val released = AtomicInteger()
        val result = result { released.incrementAndGet() }

        val reader = thread {
            result.acquireDataLease().use {
                entered.countDown()
                assertTrue(releaseReader.await(5, TimeUnit.SECONDS))
            }
        }
        assertTrue(entered.await(5, TimeUnit.SECONDS))

        result.close()
        assertEquals(0, released.get())
        releaseReader.countDown()
        reader.join(5_000)

        assertFalse(reader.isAlive)
        assertEquals(1, released.get())
    }

    @Test
    fun concurrentCloseReleasesNativeBufferExactlyOnce() {
        val released = AtomicInteger()
        val result = result { released.incrementAndGet() }
        val start = CountDownLatch(1)
        val closers = List(16) {
            thread {
                start.await()
                repeat(100) { result.close() }
            }
        }

        start.countDown()
        closers.forEach { it.join(5_000) }

        assertTrue(closers.none { it.isAlive })
        assertEquals(1, released.get())
    }

    @Test
    fun constructionFailureReleasesNativeBufferExactlyOnce() {
        val released = AtomicInteger()

        try {
            SimResult.forTest(
                data = ByteBuffer.allocate(12).order(ByteOrder.nativeOrder()),
                width = 1,
                height = 1,
                colorSpace = ColorSpace.SRGB,
                renderId = 0L,
                release = { released.incrementAndGet() },
            )
            throw AssertionError("heap buffer must be rejected")
        } catch (_: IllegalArgumentException) {
            // Expected: the result was never published, so construction owns cleanup.
        }

        assertEquals(1, released.get())
    }

    @Test(expected = IllegalStateException::class)
    fun closedResultRejectsNewReaders() {
        val result = result {}
        result.close()
        result.acquireDataLease()
    }

    @Test
    fun nativeResultNormalizesJniViewToNativeByteOrder() {
        val released = AtomicInteger()
        val result = SimResult.forTest(
            data = ByteBuffer.allocateDirect(12).order(nonNativeOrder),
            width = 1,
            height = 1,
            colorSpace = ColorSpace.SRGB,
            renderId = 0L,
            release = { released.incrementAndGet() },
        )

        result.acquireDataLease().use { lease ->
            assertEquals(ByteOrder.nativeOrder(), lease.data.order())
            lease.data.putFloat(0, 0.5f)
            assertEquals(0.5f, lease.data.getFloat(0), 0.0f)
        }
        result.close()

        assertEquals(1, released.get())
    }

    @Test
    fun dataLeaseConstructionOomRollsBackRefcount() {
        val released = AtomicInteger()
        val result = SimResult.forTest(
            data = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            colorSpace = ColorSpace.SRGB,
            renderId = 0L,
            release = { released.incrementAndGet() },
            dataLeaseFactory = DataLeaseFactory { _, _ ->
                throw OutOfMemoryError("DataLease construction")
            },
        )

        assertTrue(result.runCatching { acquireDataLease() }.exceptionOrNull() is OutOfMemoryError)
        result.close()

        assertEquals("phantom lease must not retain SimResult", 1, released.get())
    }
}
