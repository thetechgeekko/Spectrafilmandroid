package com.spectrafilm.engine

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LinearImageLifecycleTest {
    private fun image(onClose: () -> Unit): LinearImage =
        LinearImage.forTest(
            data = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            release = onClose,
        )

    @Test
    fun publicApiRequiresAnExplicitDataLease() {
        assertTrue(LinearImage::class.java.methods.none { it.name == "withDataLease" })
        assertEquals(
            DataLease::class.java,
            LinearImage::class.java.getDeclaredMethod("acquireDataLease").returnType,
        )
    }

    @Test
    fun closeDefersCleanupUntilActiveEngineLeaseEnds() {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val cleanupCount = AtomicInteger()
        val image = image { cleanupCount.incrementAndGet() }

        val worker = thread {
            image.acquireDataLease().use {
                entered.countDown()
                assertTrue(release.await(5, TimeUnit.SECONDS))
            }
        }

        assertTrue(entered.await(5, TimeUnit.SECONDS))
        image.close()
        assertEquals(0, cleanupCount.get())
        release.countDown()
        worker.join(5_000)

        assertFalse(worker.isAlive)
        assertEquals(1, cleanupCount.get())
    }

    @Test
    fun concurrentCloseRunsCleanupExactlyOnce() {
        val cleanupCount = AtomicInteger()
        val image = image { cleanupCount.incrementAndGet() }
        val start = CountDownLatch(1)
        val workers = List(16) {
            thread {
                start.await()
                repeat(100) { image.close() }
            }
        }

        start.countDown()
        workers.forEach { it.join(5_000) }

        assertTrue(workers.none { it.isAlive })
        assertEquals(1, cleanupCount.get())
    }

    @Test(expected = IllegalStateException::class)
    fun closedImageCannotAcquireAnotherEngineLease() {
        val image = image {}
        image.close()
        image.acquireDataLease()
    }

    @Test(expected = IllegalArgumentException::class)
    fun dataLeaseRejectsNonNativeByteOrder() {
        val nonNative = if (ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN) {
            ByteOrder.BIG_ENDIAN
        } else {
            ByteOrder.LITTLE_ENDIAN
        }
        val image = LinearImage(
            ByteBuffer.allocateDirect(12).order(nonNative),
            width = 1,
            height = 1,
        )
        image.acquireDataLease()
    }

    @Test
    fun eachLeaseHasAnIndependentCapturedLogicalWindow() {
        val callerView = ByteBuffer.allocateDirect(24).order(ByteOrder.nativeOrder())
        callerView.position(4)
        callerView.limit(16)
        val image = LinearImage(callerView, width = 1, height = 1)

        image.acquireDataLease().use { firstLease ->
            val first = firstLease.data
            assertEquals(4, first.position())
            assertEquals(16, first.limit())
            first.position(8)

            image.acquireDataLease().use { secondLease ->
                val second = secondLease.data
                assertEquals(4, second.position())
                assertEquals(16, second.limit())
                assertEquals(ByteOrder.nativeOrder(), second.order())
            }
        }

        // Mutating the caller's original view after construction must not move
        // the logical sample window captured by LinearImage.
        callerView.clear()
        image.acquireDataLease().use { laterLease ->
            val later = laterLease.data
            assertEquals(4, later.position())
            assertEquals(16, later.limit())
        }
    }

    @Test
    fun dataLeaseConstructionOomRollsBackRefcount() {
        val cleanupCount = AtomicInteger()
        val image = LinearImage.forTest(
            data = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            release = { cleanupCount.incrementAndGet() },
            dataLeaseFactory = DataLeaseFactory { _, _ ->
                throw OutOfMemoryError("DataLease construction")
            },
        )

        assertTrue(image.runCatching { acquireDataLease() }.exceptionOrNull() is OutOfMemoryError)
        image.close()

        assertEquals("phantom lease must not retain LinearImage", 1, cleanupCount.get())
    }
}
