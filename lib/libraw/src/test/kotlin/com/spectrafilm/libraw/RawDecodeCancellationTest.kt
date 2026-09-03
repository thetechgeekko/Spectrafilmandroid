package com.spectrafilm.libraw

import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RawDecodeCancellationTest {

    @Test
    fun `cancel and close are idempotent and closed token fails typed`() {
        val cancels = AtomicInteger()
        val releases = AtomicInteger()
        val token = RawDecodeCancellation.forTest(
            token = 41L,
            cancelNative = { cancels.incrementAndGet() },
            releaseNative = { releases.incrementAndGet() },
        )

        assertTrue(token.cancel())
        assertTrue(token.cancel())
        token.close()
        token.close()

        assertEquals(1, cancels.get())
        assertEquals(1, releases.get())
        val error = assertThrows(RawDecodeException::class.java, token::tokenForDecode)
        assertEquals(DecodeStatus.CANCELLED, error.status)
        assertFalse(token.cancel())
    }

    @Test
    fun `concurrent close releases native cancellation generation once`() {
        val releases = AtomicInteger()
        val token = RawDecodeCancellation.forTest(
            token = 42L,
            cancelNative = {},
            releaseNative = { releases.incrementAndGet() },
        )
        val ready = CountDownLatch(16)
        val start = CountDownLatch(1)
        val executor = Executors.newFixedThreadPool(16)

        repeat(16) {
            executor.execute {
                ready.countDown()
                start.await()
                token.close()
            }
        }
        assertTrue(ready.await(5, TimeUnit.SECONDS))
        start.countDown()
        executor.shutdown()
        assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS))

        assertEquals(1, releases.get())
    }
}
