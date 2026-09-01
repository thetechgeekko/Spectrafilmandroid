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
            nativePrecisionDescriptorCrossesJniAtomically()
            oversizedBlackRepeatReturnsPrecisionMetadataInsteadOfOom()
            invalidDirectCapacityReturnsTypedInputError()
            nonCanonicalNativeDirectWindowReturnsTypedInputError()
            preCancelledDecodeReturnsTypedCancellation()
            closeDefersRealNativeReleaseUntilDataLeaseReturns()
            concurrentCloseAndDecodeCleanupAreSafe()
            evidence.putString("stream", "OK (8 tests)")
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

    private fun nativePrecisionDescriptorCrossesJniAtomically() {
        RawDecoder.decodeToLinear(validUncompressedDng(64, 64)).use { result ->
            val source = result.precisionDescriptor
            check(source.version == RawPrecisionDescriptor.CURRENT_VERSION)
            check(source.sampleFormat == RawSampleFormat.UNSIGNED_INTEGER)
            check(source.declaredBitsPerSample == 16)
            check(source.effectiveBitsPerSample == 16)
            check(source.processedBitsPerSample == 16)
            check(source.byteOrder == RawByteOrder.LITTLE_ENDIAN)
            check(source.packing == RawPacking.TIFF_WORD_16)
            check(source.pixelLayout == RawPixelLayout.BAYER_2X2)
            check(source.colorChannels == 3)
            // CFA metadata retains LibRaw's four physical level slots so the
            // second-green plane remains observable even for three colors.
            check(source.blackLevelChannels.size == 4)
            check(source.whiteLevels.size == 4)
            check(source.cfaPatternRows == 2)
            check(source.cfaPatternColumns == 2)
            check(source.cfaPattern.size == 4)
            check(source.decoderRoute == RawDecoderRoute.LIBRAW_NATIVE)
            check(source.postprocessRoute ==
                RawPostprocessRoute.LIBRAW_ACES_TO_FLOAT32_PROPHOTO)
            check(source.linearSpace == RawLinearSpace.LINEAR_PROPHOTO_RGB)
            check(source.whiteLevelProvenance == RawLevelProvenance.DNG_METADATA)
            check(source.blackLevelProvenance ==
                RawLevelProvenance.DECLARED_BITS_DEFAULT)
            check(source.baselineExposure == null)
            check(source.linearResponseLimit == null)
            check(!source.halfSizeRequested)
            check(source.requestedMaxLongEdge == null)
            check(source.outputSubsampleStep == 1)
            check(source.whiteLevels.all { it == 65535L })
        }

        RawDecoder.decodeToLinear(validThreePlaneLinearDng(64, 64)).use { result ->
            val source = result.precisionDescriptor
            check(source.pixelLayout == RawPixelLayout.LINEAR)
            check(source.colorChannels == 3)
            check(source.declaredBitsPerSample == 16)
            check(source.effectiveBitsPerSample == 2)
            check(source.blackLevelChannels == listOf(4092L, 4093L, 4094L))
            check(source.whiteLevels == listOf(4095L, 4095L, 4095L))
        }
    }

    private fun oversizedBlackRepeatReturnsPrecisionMetadataInsteadOfOom() {
        val error = runCatching {
            RawDecoder.decodeToLinear(validUncompressedDng(64, 64, blackRepeatColumns = 64))
        }.exceptionOrNull()
        check(error is RawDecodeException)
        check(error.status == DecodeStatus.PRECISION_METADATA) {
            "descriptor validation was remapped to ${error.status}"
        }
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

    private fun validUncompressedDng(
        width: Int,
        height: Int,
        blackRepeatColumns: Int? = null,
    ): ByteArray {
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
        )
        if (blackRepeatColumns != null) {
            require(blackRepeatColumns == 64)
            entries += Entry(0xc619, 3, 2, (blackRepeatColumns shl 16) or 1)
            entries += Entry(0xc61a, 4, blackRepeatColumns, 0)
        }
        val sortedEntries = entries.sortedBy(Entry::tag)
        val ifdOffset = 8
        val blackPatternOffset = ifdOffset + 2 + sortedEntries.size * 12 + 4
        val blackPatternBytes = (blackRepeatColumns ?: 0) * Int.SIZE_BYTES
        val stripOffset = blackPatternOffset + blackPatternBytes
        sortedEntries.first { it.tag == 0x0111 }.value = stripOffset
        sortedEntries.firstOrNull { it.tag == 0xc61a }?.value = blackPatternOffset
        return ByteBuffer.allocate(stripOffset + strip.size)
            .order(ByteOrder.LITTLE_ENDIAN)
            .apply {
                put('I'.code.toByte())
                put('I'.code.toByte())
                putShort(42)
                putInt(ifdOffset)
                position(ifdOffset)
                putShort(sortedEntries.size.toShort())
                sortedEntries.forEach { entry ->
                    putShort(entry.tag.toShort())
                    putShort(entry.type.toShort())
                    putInt(entry.count)
                    putInt(entry.value)
                }
                putInt(0)
                repeat(blackRepeatColumns ?: 0) { putInt(0) }
                put(strip)
            }
            .array()
    }

    /**
     * Three-plane LinearRaw fixture whose fourth carrier slot is deliberately
     * absent. The active spans are 3, 2, and 1 codes, so their effective
     * precision is two bits; a synthetic zero-valued fourth black slot would
     * incorrectly inflate that result to twelve bits.
     */
    private fun validThreePlaneLinearDng(width: Int, height: Int): ByteArray {
        require(width >= 22 && height >= 22)
        val samplesPerPixel = 3
        val blackLevels = intArrayOf(4092, 4093, 4094)
        val whiteLevels = intArrayOf(4095, 4095, 4095)
        val strip = ByteBuffer.allocate(width * height * samplesPerPixel * 2)
            .order(ByteOrder.LITTLE_ENDIAN)
            .apply {
                repeat(height) {
                    repeat(width) { column ->
                        val band = (column * 5 / width).coerceAtMost(4)
                        repeat(samplesPerPixel) { channel ->
                            val black = blackLevels[channel]
                            val white = whiteLevels[channel]
                            val span = white - black
                            val sample = when (band) {
                                0 -> black
                                1 -> black + 1
                                2 -> black + span / 2
                                3 -> white - 1
                                else -> white
                            }
                            putShort(sample.toShort())
                        }
                    }
                }
            }
            .array()
        val cameraModel = "Spektrafilm LinearRaw fixture\u0000"
            .toByteArray(Charsets.US_ASCII)

        data class Entry(val tag: Int, val type: Int, val count: Int, var value: Int)
        val entries = mutableListOf(
            Entry(0x00fe, 4, 1, 0),
            Entry(0x0100, 4, 1, width),
            Entry(0x0101, 4, 1, height),
            Entry(0x0102, 3, samplesPerPixel, 0),
            Entry(0x0103, 3, 1, 1),
            Entry(0x0106, 3, 1, 34892),
            Entry(0x0111, 4, 1, 0),
            Entry(0x0112, 3, 1, 1),
            Entry(0x0115, 3, 1, samplesPerPixel),
            Entry(0x0116, 4, 1, height),
            Entry(0x0117, 4, 1, strip.size),
            Entry(0x011c, 3, 1, 1),
            Entry(0x0153, 3, samplesPerPixel, 0),
            Entry(0xc612, 1, 4, 0x00000401),
            Entry(0xc613, 1, 4, 0x00000101),
            Entry(0xc614, 2, cameraModel.size, 0),
            Entry(0xc619, 3, 2, 0x00010001),
            Entry(0xc61a, 4, samplesPerPixel, 0),
            Entry(0xc61d, 4, samplesPerPixel, 0),
            Entry(0xc621, 10, samplesPerPixel * 3, 0),
            Entry(0xc622, 10, samplesPerPixel * 3, 0),
            Entry(0xc65a, 3, 1, 21),
            Entry(0xc65b, 3, 1, 17),
        ).sortedBy(Entry::tag)

        val ifdOffset = 8
        var payloadAt = ifdOffset + 2 + entries.size * 12 + 4
        val bitsAt = payloadAt.also { payloadAt += samplesPerPixel * 2 }
        val sampleFormatAt = payloadAt.also { payloadAt += samplesPerPixel * 2 }
        val blackAt = payloadAt.also { payloadAt += samplesPerPixel * 4 }
        val whiteAt = payloadAt.also { payloadAt += samplesPerPixel * 4 }
        val cameraModelAt = payloadAt.also { payloadAt += cameraModel.size }
        val colorMatrix1At = payloadAt.also { payloadAt += samplesPerPixel * 3 * 8 }
        val colorMatrix2At = payloadAt.also { payloadAt += samplesPerPixel * 3 * 8 }
        val stripAt = payloadAt

        entries.first { it.tag == 0x0102 }.value = bitsAt
        entries.first { it.tag == 0x0111 }.value = stripAt
        entries.first { it.tag == 0x0153 }.value = sampleFormatAt
        entries.first { it.tag == 0xc614 }.value = cameraModelAt
        entries.first { it.tag == 0xc61a }.value = blackAt
        entries.first { it.tag == 0xc61d }.value = whiteAt
        entries.first { it.tag == 0xc621 }.value = colorMatrix1At
        entries.first { it.tag == 0xc622 }.value = colorMatrix2At

        return ByteBuffer.allocate(stripAt + strip.size)
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

                position(bitsAt)
                repeat(samplesPerPixel) { putShort(16) }
                position(sampleFormatAt)
                repeat(samplesPerPixel) { putShort(1) }
                position(blackAt)
                blackLevels.forEach { putInt(it) }
                position(whiteAt)
                whiteLevels.forEach { putInt(it) }
                position(cameraModelAt)
                put(cameraModel)
                for (matrixAt in intArrayOf(colorMatrix1At, colorMatrix2At)) {
                    position(matrixAt)
                    repeat(samplesPerPixel) { plane ->
                        repeat(3) { xyz ->
                            putInt(if (plane == xyz) 1 else 0)
                            putInt(1)
                        }
                    }
                }
                position(stripAt)
                put(strip)
            }
            .array()
    }
}
