package com.spectrafilm.libraw

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class LinearResultOwnershipTest {

    @Test
    fun `scoped borrow is internal Unit while explicit ownership lease stays public`() {
        val publicMethods = LinearResult::class.java.methods
        assertTrue(publicMethods.none { it.name == "withDataLease" })
        assertTrue(
            publicMethods.any {
                it.name == "acquireDataLease" &&
                    it.returnType == LinearResult.DataLease::class.java
            },
        )

        val scopedBorrow = LinearResult::class.java.declaredMethods.single {
            it.name.startsWith("withDataLease")
        }
        assertEquals(Void.TYPE, scopedBorrow.returnType)
    }

    @Test
    fun `close defers native release until active data lease returns`() {
        val releases = AtomicInteger()
        val result = LinearResult.forTest(
            data = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
                .order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            release = { releases.incrementAndGet() },
        )
        val leaseEntered = CountDownLatch(1)
        val allowLeaseReturn = CountDownLatch(1)
        val executor = Executors.newSingleThreadExecutor()

        val reader = executor.submit {
            result.withDataLease { leased ->
                leaseEntered.countDown()
                check(allowLeaseReturn.await(5, TimeUnit.SECONDS))
                leased.asFloatBuffer().put(0, 0.5f)
            }
        }

        assertTrue(leaseEntered.await(5, TimeUnit.SECONDS))
        result.close()
        assertTrue(result.isClosed)
        assertEquals(0, releases.get())
        assertThrows(IllegalStateException::class.java) {
            result.withDataLease { error("closed result exposed its buffer") }
        }

        allowLeaseReturn.countDown()
        reader.get(5, TimeUnit.SECONDS)
        executor.shutdown()
        assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS))
        assertEquals(1, releases.get())
    }

    @Test
    fun `each lease gets an independent native-order view of captured logical window`() {
        val source = ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder()).apply {
            position(4)
            limit(28)
        }
        val result = LinearResult(source, width = 1, height = 1)

        // Mutating the caller's view after construction must not change the result window.
        source.position(0)
        source.limit(source.capacity())

        result.withDataLease { first ->
            assertEquals(4, first.position())
            assertEquals(28, first.limit())
            assertEquals(ByteOrder.nativeOrder(), first.order())
            first.position(12)
        }
        result.withDataLease { second ->
            assertEquals(4, second.position())
            assertEquals(28, second.limit())
            assertEquals(ByteOrder.nativeOrder(), second.order())
        }
    }

    @Test
    fun `concurrent and repeated close releases native allocation exactly once`() {
        val releases = AtomicInteger()
        val result = LinearResult.forTest(
            data = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
                .order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            release = { releases.incrementAndGet() },
        )
        val ready = CountDownLatch(16)
        val start = CountDownLatch(1)
        val executor = Executors.newFixedThreadPool(16)

        repeat(16) {
            executor.execute {
                ready.countDown()
                start.await()
                result.close()
            }
        }
        assertTrue(ready.await(5, TimeUnit.SECONDS))
        start.countDown()
        executor.shutdown()
        assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS))
        result.close()

        assertTrue(result.isClosed)
        assertEquals(1, releases.get())
    }

    @Test
    fun `native allocation is released if result construction rejects metadata`() {
        val releases = AtomicInteger()
        val data = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())

        assertThrows(IllegalArgumentException::class.java) {
            LinearResult.fromNative(
                data = data,
                width = 0,
                height = 1,
                colorSpace = "ProPhoto RGB",
                release = { releases.incrementAndGet() },
            )
        }

        assertEquals(1, releases.get())
    }
}
