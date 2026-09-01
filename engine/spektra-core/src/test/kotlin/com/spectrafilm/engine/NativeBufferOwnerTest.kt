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

class NativeBufferOwnerTest {
    private val nonNativeOrder: ByteOrder
        get() = if (ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN) {
            ByteOrder.BIG_ENDIAN
        } else {
            ByteOrder.LITTLE_ENDIAN
        }

    @Test
    fun publicOwnerNeverReturnsItsNativeByteBufferOrFreeCapability() {
        val byteBufferReturningMethods = NativeBufferOwner::class.java.methods.filter {
            it.returnType == ByteBuffer::class.java
        }

        assertTrue(byteBufferReturningMethods.isEmpty())
        assertTrue(SimResult::class.java.methods.none { it.name == "freeDirectBuffer" })
    }

    @Test
    fun transferKeepsAllocationAliveUntilLinearImageCloses() {
        val released = AtomicInteger()
        val owner = NativeBufferOwner.forTest(
            ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
        ) { released.incrementAndGet() }

        var wrotePixels = false
        owner.acquireDataLease().use { lease ->
            val data = lease.data
            data.putFloat(0, 0.25f)
            wrotePixels = true
        }
        assertTrue(wrotePixels)

        val image = owner.transferToLinearImage(1, 1, "ProPhoto RGB")
        owner.close()
        assertEquals(0, released.get())

        try {
            owner.acquireDataLease()
            throw AssertionError("transfer must close the source owner")
        } catch (_: IllegalStateException) {
            // The transferred image is now the only path holding the active lease.
        }

        var observed = 0f
        image.acquireDataLease().use { lease -> observed = lease.data.getFloat(0) }
        assertEquals(0.25f, observed, 0.0f)
        image.close()
        assertEquals(1, released.get())
    }

    @Test
    fun closeDefersReleaseUntilScopedWriterReturns() {
        val entered = CountDownLatch(1)
        val releaseWriter = CountDownLatch(1)
        val released = AtomicInteger()
        val owner = NativeBufferOwner.forTest(
            ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
        ) { released.incrementAndGet() }

        val writer = thread {
            owner.acquireDataLease().use {
                entered.countDown()
                assertTrue(releaseWriter.await(5, TimeUnit.SECONDS))
            }
        }
        assertTrue(entered.await(5, TimeUnit.SECONDS))
        owner.close()
        assertEquals(0, released.get())

        releaseWriter.countDown()
        writer.join(5_000)
        assertFalse(writer.isAlive)
        assertEquals(1, released.get())
    }

    @Test
    fun ownerConstructionFailureReleasesItsAllocationExactlyOnce() {
        val released = AtomicInteger()

        try {
            NativeBufferOwner.forTest(ByteBuffer.allocate(12)) { released.incrementAndGet() }
            throw AssertionError("heap buffer must be rejected")
        } catch (_: IllegalArgumentException) {
            // Expected: the unpublished owner releases its allocation before failing.
        }

        assertEquals(1, released.get())
    }

    @Test
    fun nativeOwnedDirectBufferNormalizesJniViewToNativeByteOrder() {
        val released = AtomicInteger()
        val owner = NativeBufferOwner.forTest(
            ByteBuffer.allocateDirect(12).order(nonNativeOrder),
        ) { released.incrementAndGet() }

        owner.acquireDataLease().use { lease ->
            assertEquals(ByteOrder.nativeOrder(), lease.data.order())
            lease.data.putFloat(0, 0.25f)
            assertEquals(0.25f, lease.data.getFloat(0), 0.0f)
        }
        owner.close()

        assertEquals(1, released.get())
    }

    @Test
    fun dataLeaseConstructionOomRollsBackRefcount() {
        val released = AtomicInteger()
        val owner = NativeBufferOwner.forTest(
            data = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
            release = { released.incrementAndGet() },
            dataLeaseFactory = DataLeaseFactory { _, _ ->
                throw OutOfMemoryError("DataLease construction")
            },
        )

        assertTrue(owner.runCatching { acquireDataLease() }.exceptionOrNull() is OutOfMemoryError)
        owner.close()

        assertEquals("phantom lease must not retain native allocation", 1, released.get())
    }
}
