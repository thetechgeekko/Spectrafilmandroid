package com.spectrafilm.libraw

import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RawCoilCancellationTest {

    @Test
    fun `source cancellation propagates and releases cancellation generation once`() = runBlocking {
        val cancels = AtomicInteger()
        val releases = AtomicInteger()
        val token = RawDecodeCancellation.forTest(
            token = 51L,
            cancelNative = { cancels.incrementAndGet() },
            releaseNative = { releases.incrementAndGet() },
        )
        val expected = CancellationException("source read cancelled")

        val failure = runCatching {
            runRawCoilDecode<Unit>(cancellationFactory = { token }) {
                throw expected
            }
        }.exceptionOrNull()

        assertTrue(failure is CancellationException)
        assertEquals(expected.message, failure?.message)
        assertEquals(0, cancels.get())
        assertEquals(1, releases.get())
    }

    @Test
    fun `coroutine cancellation requests cooperative stop and releases generation once`() = runBlocking {
        val entered = CountDownLatch(1)
        val leaveNativeWork = CountDownLatch(1)
        val cancels = AtomicInteger()
        val releases = AtomicInteger()
        val lateDisposals = AtomicInteger()
        val token = RawDecodeCancellation.forTest(
            token = 52L,
            cancelNative = { cancels.incrementAndGet() },
            releaseNative = { releases.incrementAndGet() },
        )

        val job = launch(Dispatchers.Default) {
            runRawCoilDecode(
                cancellationFactory = { token },
                onLateResult = { lateDisposals.incrementAndGet() },
            ) {
                entered.countDown()
                check(leaveNativeWork.await(5, TimeUnit.SECONDS))
                "late result"
            }
        }

        assertTrue(entered.await(5, TimeUnit.SECONDS))
        job.cancel()
        assertTrue(token.isCancellationRequested)
        assertEquals(1, cancels.get())
        leaveNativeWork.countDown()
        job.cancelAndJoin()

        assertEquals(1, releases.get())
        assertEquals(1, lateDisposals.get())
    }

    @Test
    fun `preview pixel conversion observes cancellation and closes native result once`() {
        val nativeReleases = AtomicInteger()
        val result = LinearResult.forTest(
            data = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
                .order(ByteOrder.nativeOrder()),
            width = 1,
            height = 1,
            release = { nativeReleases.incrementAndGet() },
        )
        val cancellation = RawDecodeCancellation.forTest(
            token = 53L,
            cancelNative = {},
            releaseNative = {},
        )
        cancellation.cancel()

        val failure = assertThrows(RawDecodeException::class.java) {
            result.use { rawCoilPreviewPixels(it, cancellation) }
        }

        assertEquals(DecodeStatus.CANCELLED, failure.status)
        assertTrue(result.isClosed)
        assertEquals(1, nativeReleases.get())
        cancellation.close()
    }
}
