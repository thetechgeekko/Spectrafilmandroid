package com.spectrafilm.libraw

import android.app.Activity
import android.app.Instrumentation
import android.os.Bundle
import java.lang.reflect.InvocationTargetException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Device-executed JNI boundary, cancellation, and native-owner regression gate. */
class RawDecoderBoundaryInstrumentation : Instrumentation() {

    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        start()
    }

    override fun onStart() {
        val evidence = Bundle()
        try {
            logicalDirectWindowDecodesOnlyRemainingBytes()
            invalidDirectCapacityReturnsTypedInputError()
            nonCanonicalNativeDirectWindowReturnsTypedInputError()
            preCancelledDecodeReturnsTypedCancellation()
            closeDefersRealNativeReleaseUntilDataLeaseReturns()
            concurrentCloseAndDecodeCleanupAreSafe()
            evidence.putString("stream", "OK (6 tests)")
            finish(Activity.RESULT_OK, evidence)
        } catch (failure: Throwable) {
            evidence.putString(
                "stream",
                "FAIL: ${failure.javaClass.name}: ${failure.message}\n" +
                    failure.stackTraceToString(),
            )
            finish(Activity.RESULT_CANCELED, evidence)
        }
    }

    private fun logicalDirectWindowDecodesOnlyRemainingBytes() {
        val dng = validUncompressedDng(64, 64)
        val input = ByteBuffer.allocateDirect(dng.size + 7).apply {
            put(byteArrayOf(91, 92, 93, 94))
            put(dng)
            put(byteArrayOf(95, 96, 97))
            position(4)
            limit(4 + dng.size)
        }

        RawDecoder.decodeToLinear(input).use { result ->
            check(result.width > 0 && result.height > 0)
            result.withDataLease { data ->
                check(data.remaining() == result.width * result.height * 3 * Float.SIZE_BYTES)
            }
        }
        check(input.position() == 4)
        check(input.limit() == 4 + dng.size)
    }

    private fun invalidDirectCapacityReturnsTypedInputError() {
        val nativeDecode = RawDecoder::class.java.getDeclaredMethod(
            "nativeDecodeBuffer",
            ByteBuffer::class.java,
            Int::class.javaPrimitiveType,
            Int::class.javaPrimitiveType,
            Double::class.javaPrimitiveType,
            Double::class.javaPrimitiveType,
            Boolean::class.javaPrimitiveType,
            Int::class.javaPrimitiveType,
            Long::class.javaPrimitiveType,
        ).apply { isAccessible = true }
        val error = try {
            nativeDecode.invoke(
                RawDecoder,
                ByteBuffer.allocateDirect(8),
                7,
                0,
                6504.0,
                1.0,
                false,
                0,
                0L,
            )
            error("mismatched direct-buffer capacity was accepted")
        } catch (wrapped: InvocationTargetException) {
            wrapped.cause ?: wrapped
        }
        check(error is RawDecodeException && error.status == DecodeStatus.INPUT)
    }

    private fun nonCanonicalNativeDirectWindowReturnsTypedInputError() {
        val nativeDecode = RawDecoder::class.java.getDeclaredMethod(
            "nativeDecodeBuffer",
            ByteBuffer::class.java,
            Int::class.javaPrimitiveType,
            Int::class.javaPrimitiveType,
            Double::class.javaPrimitiveType,
            Double::class.javaPrimitiveType,
            Boolean::class.javaPrimitiveType,
            Int::class.javaPrimitiveType,
            Long::class.javaPrimitiveType,
        ).apply { isAccessible = true }
        val nonCanonical = ByteBuffer.allocateDirect(8).apply {
            position(1)
            limit(8)
        }
        val error = try {
            nativeDecode.invoke(
                RawDecoder,
                nonCanonical,
                nonCanonical.remaining(),
                0,
                6504.0,
                1.0,
                false,
                0,
                0L,
            )
            error("non-canonical direct-buffer range was accepted by JNI")
        } catch (wrapped: InvocationTargetException) {
            wrapped.cause ?: wrapped
        }
        check(error is RawDecodeException && error.status == DecodeStatus.INPUT)
    }

    private fun preCancelledDecodeReturnsTypedCancellation() {
        RawDecoder.newCancellation().use { cancellation ->
            check(cancellation.cancel())
            val error = runCatching {
                RawDecoder.decodeToLinear(
                    validUncompressedDng(64, 64),
                    cancellation = cancellation,
                )
            }.exceptionOrNull()
            check(error is RawDecodeException && error.status == DecodeStatus.CANCELLED)
        }
    }

    private fun concurrentCloseAndDecodeCleanupAreSafe() {
        val dng = validUncompressedDng(64, 64)
        repeat(32) {
            val result = RawDecoder.decodeToLinear(dng)
            val ready = CountDownLatch(8)
            val start = CountDownLatch(1)
            val executor = Executors.newFixedThreadPool(8)
            val closes = List(8) {
                executor.submit {
                    ready.countDown()
                    start.await()
                    result.close()
                }
            }
            check(ready.await(5, TimeUnit.SECONDS))
            start.countDown()
            closes.forEach { it.get(5, TimeUnit.SECONDS) }
            executor.shutdown()
            check(executor.awaitTermination(5, TimeUnit.SECONDS))
            result.close()
            check(result.isClosed)
        }
    }

    private fun closeDefersRealNativeReleaseUntilDataLeaseReturns() {
        val result = RawDecoder.decodeToLinear(validUncompressedDng(64, 64))
        val leaseEntered = CountDownLatch(1)
        val closeRequested = CountDownLatch(1)
        val reader = Executors.newSingleThreadExecutor()
        val activeRead = reader.submit {
            result.withDataLease { data ->
                leaseEntered.countDown()
                check(closeRequested.await(5, TimeUnit.SECONDS))
                // This read happens after close() has linearized. It remains safe only
                // if the native allocation is retained until this lease returns.
                check(data.asFloatBuffer().get(0).isFinite())
            }
        }

        check(leaseEntered.await(5, TimeUnit.SECONDS))
        result.close()
        check(result.isClosed)
        closeRequested.countDown()
        activeRead.get(5, TimeUnit.SECONDS)
        reader.shutdown()
        check(reader.awaitTermination(5, TimeUnit.SECONDS))
        result.close()
    }

    private fun validUncompressedDng(width: Int, height: Int): ByteArray {
        require(width >= 22 && height >= 22)
        val strip = ByteArray(width * height * 2)
        repeat(width * height) { pixel ->
            val sample = (1024 + (pixel % width) * 512).coerceAtMost(65535)
            strip[pixel * 2] = sample.toByte()
            strip[pixel * 2 + 1] = (sample ushr 8).toByte()
        }
        data class Entry(val tag: Int, val type: Int, val count: Int, var value: Int)
        val entries = mutableListOf(
            Entry(0x00fe, 4, 1, 0),
            Entry(0x0100, 4, 1, width),
            Entry(0x0101, 4, 1, height),
            Entry(0x0102, 3, 1, 16),
            Entry(0x0103, 3, 1, 1),
            Entry(0x0106, 3, 1, 32803),
            Entry(0x0111, 4, 1, 0),
            Entry(0x0115, 3, 1, 1),
            Entry(0x0116, 4, 1, height),
            Entry(0x0117, 4, 1, strip.size),
            Entry(0x011c, 3, 1, 1),
            Entry(0x828d, 3, 2, 0x00020002),
            Entry(0x828e, 1, 4, 0x02010100),
            Entry(0xc612, 1, 4, 0x00000401),
            Entry(0xc613, 1, 4, 0x00000101),
            Entry(0xc61d, 4, 1, 65535),
            Entry(0xc65a, 3, 1, 21),
        ).sortedBy(Entry::tag)
        val ifdOffset = 8
        val stripOffset = ifdOffset + 2 + entries.size * 12 + 4
        entries.first { it.tag == 0x0111 }.value = stripOffset
        return ByteBuffer.allocate(stripOffset + strip.size)
            .order(ByteOrder.LITTLE_ENDIAN)
            .apply {
                put('I'.code.toByte())
                put('I'.code.toByte())
                putShort(42)
                putInt(ifdOffset)
                position(ifdOffset)
                putShort(entries.size.toShort())
                entries.forEach { entry ->
                    putShort(entry.tag.toShort())
                    putShort(entry.type.toShort())
                    putInt(entry.count)
                    putInt(entry.value)
                }
                putInt(0)
                put(strip)
            }
            .array()
    }
}
