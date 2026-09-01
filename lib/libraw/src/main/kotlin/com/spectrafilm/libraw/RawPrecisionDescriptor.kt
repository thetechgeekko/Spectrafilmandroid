/*
 * Spektrafilm for Android -- versioned RAW precision/source descriptor.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
package com.spectrafilm.libraw

enum class RawSampleFormat(val code: Int) {
    UNKNOWN(0),
    UNSIGNED_INTEGER(1),
    FLOATING_POINT(2),
    ;

    internal companion object {
        fun requireCode(code: Int): RawSampleFormat =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW sample format $code")
    }
}

enum class RawByteOrder(val code: Int) {
    UNKNOWN(0),
    LITTLE_ENDIAN(1),
    BIG_ENDIAN(2),
    NOT_APPLICABLE(3),
    ;

    internal companion object {
        fun requireCode(code: Int): RawByteOrder =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW byte order $code")
    }
}

enum class RawPacking(val code: Int) {
    UNKNOWN(0),
    TIFF_PACKED_BITS(1),
    TIFF_WORD_16(2),
    TIFF_FLOAT(3),
    LOSSLESS_COMPRESSED(4),
    VENDOR_DEFINED(5),
    ;

    internal companion object {
        fun requireCode(code: Int): RawPacking =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW packing $code")
    }
}

enum class RawPixelLayout(val code: Int) {
    UNKNOWN(0),
    BAYER_2X2(1),
    XTRANS_6X6(2),
    LINEAR(3),
    LAYERED(4),
    CUSTOM_CFA(5),
    ;

    internal companion object {
        fun requireCode(code: Int): RawPixelLayout =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW pixel layout $code")
    }
}

enum class RawDecoderRoute(val code: Int) {
    LIBRAW_NATIVE(1),
    PLATFORM_DISPLAY_REFERRED(2),
    MANAGED_LINEAR(3),
    ;

    internal companion object {
        fun requireCode(code: Int): RawDecoderRoute =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW decoder route $code")
    }
}

enum class RawPostprocessRoute(val code: Int) {
    LIBRAW_ACES_TO_FLOAT32_PROPHOTO(1),
    PLATFORM_DISPLAY_REFERRED(2),
    MANAGED_FLOAT32(3),
    ;

    internal companion object {
        fun requireCode(code: Int): RawPostprocessRoute =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW postprocess route $code")
    }
}

enum class RawLinearSpace(val code: Int) {
    LINEAR_PROPHOTO_RGB(1),
    DISPLAY_REFERRED(2),
    CALLER_DEFINED_LINEAR(3),
    ;

    internal companion object {
        fun requireCode(code: Int): RawLinearSpace =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW linear-space code $code")
    }
}

enum class RawLevelProvenance(val code: Int) {
    UNKNOWN(0),
    DNG_METADATA(1),
    DECLARED_BITS_DEFAULT(2),
    DECODER_METADATA(3),
    ;

    internal companion object {
        fun requireCode(code: Int): RawLevelProvenance =
            entries.firstOrNull { it.code == code }
                ?: throw IllegalArgumentException("unknown RAW level provenance $code")
    }
}

/**
 * Immutable, versioned description of the source precision admitted by a decode.
 *
 * Native integer RAW levels are represented without averaging: effective black at
 * `(channel, row, column)` is [blackLevelCommon] + the matching
 * [blackLevelChannels] entry + the repeating [blackPattern] cell. A missing value
 * is `null`/an empty list and is distinct from a real zero code value.
 * [cfaPattern] has the explicit [cfaPatternRows] by [cfaPatternColumns]
 * geometry and uses LibRaw's bounded 0..3 CFA slots; slot 3 may be the second
 * green plane even when [colorChannels] is three.
 * DNG LinearRaw descriptor v1 is intentionally restricted to a 1x1 repeat with
 * one exact black and white value per three/four sample channel; its lists have
 * exactly that active channel count. CFA layouts retain four LibRaw level slots
 * so the second-green alias is not lost. A spatial
 * row-by-column-by-sample BlackLevel matrix is rejected until a versioned carrier
 * can preserve it without flattening.
 *
 * Construction is intentionally closed. [forPlatformDisplayReferredFallback]
 * creates a descriptor that can only identify itself as display-referred; callers
 * cannot promote a platform bitmap fallback to native RAW precision parity.
 */
class RawPrecisionDescriptor private constructor(
    val version: Int,
    val sampleFormat: RawSampleFormat,
    val declaredBitsPerSample: Int?,
    val effectiveBitsPerSample: Int?,
    val processedBitsPerSample: Int?,
    val byteOrder: RawByteOrder,
    val packing: RawPacking,
    val pixelLayout: RawPixelLayout,
    val colorChannels: Int?,
    val cfaFilterCode: Long?,
    val cfaPatternRows: Int,
    val cfaPatternColumns: Int,
    cfaPattern: List<Int>,
    val decoderRoute: RawDecoderRoute,
    val postprocessRoute: RawPostprocessRoute,
    val linearSpace: RawLinearSpace,
    val whiteLevelProvenance: RawLevelProvenance,
    val blackLevelProvenance: RawLevelProvenance,
    val halfSizeRequested: Boolean,
    val blackLevelCommon: Long?,
    blackLevelChannels: List<Long>,
    val blackPatternRows: Int,
    val blackPatternColumns: Int,
    blackPattern: List<Long>,
    whiteLevels: List<Long>,
    val baselineExposure: Float?,
    val linearResponseLimit: Float?,
    val containerCompression: Int?,
    val requestedMaxLongEdge: Int?,
    val outputSubsampleStep: Int,
    val displayColorSpace: String?,
) {
    val cfaPattern: List<Int> = cfaPattern.toList()
    val blackLevelChannels: List<Long> = blackLevelChannels.toList()
    val blackPattern: List<Long> = blackPattern.toList()
    val whiteLevels: List<Long> = whiteLevels.toList()

    companion object {
        const val CURRENT_VERSION: Int = 1

        private const val WORD_COUNT = 136
        private const val REAL_COUNT = 2
        private const val BLACK_PATTERN_CAPACITY = 64
        private const val CFA_PATTERN_CAPACITY = 36
        private const val BLACK_PATTERN_AT = 24
        private const val WHITE_LEVELS_AT = 88
        private const val CFA_PATTERN_COUNT_AT = 92
        private const val CFA_PATTERN_AT = 93
        private const val BASELINE_PRESENT_AT = 129
        private const val LINEAR_RESPONSE_PRESENT_AT = 130
        private const val COMPRESSION_AT = 131
        private const val REQUESTED_MAX_LONG_EDGE_AT = 132
        private const val OUTPUT_SUBSAMPLE_STEP_AT = 133
        private const val CFA_PATTERN_ROWS_AT = 134
        private const val CFA_PATTERN_COLUMNS_AT = 135

        internal fun fromNative(words: IntArray, reals: FloatArray): RawPrecisionDescriptor {
            require(words.size == WORD_COUNT) {
                "RAW descriptor ABI expected $WORD_COUNT words, got ${words.size}"
            }
            require(reals.size == REAL_COUNT) {
                "RAW descriptor ABI expected $REAL_COUNT reals, got ${reals.size}"
            }
            require(words[0] == CURRENT_VERSION) {
                "unsupported RAW descriptor version ${words[0]}"
            }

            val sampleFormat = RawSampleFormat.requireCode(words[1])
            require(sampleFormat == RawSampleFormat.UNSIGNED_INTEGER) {
                "native precision-parity result requires unsigned integer source"
            }
            val declaredBits = words[2].takeIf { it != 0 }
            require(declaredBits == null || declaredBits in 8..16)
            val effectiveBits = words[3].takeIf { it != 0 }
            require(effectiveBits != null && effectiveBits in 1..16)
            require(declaredBits == null || effectiveBits <= declaredBits)
            val processedBits = words[4].takeIf { it != 0 }
            require(processedBits == 8 || processedBits == 16)
            require(processedBits >= effectiveBits)

            val byteOrder = RawByteOrder.requireCode(words[5])
            val packing = RawPacking.requireCode(words[6])
            val layout = RawPixelLayout.requireCode(words[7])
            require(layout != RawPixelLayout.UNKNOWN)
            val colorChannels = words[8]
            require(colorChannels in 1..4)
            val activeLevelChannels =
                if (layout == RawPixelLayout.LINEAR) colorChannels else 4
            val route = RawDecoderRoute.requireCode(words[10])
            require(route == RawDecoderRoute.LIBRAW_NATIVE)
            val postprocess = RawPostprocessRoute.requireCode(words[11])
            require(postprocess == RawPostprocessRoute.LIBRAW_ACES_TO_FLOAT32_PROPHOTO)
            val linearSpace = RawLinearSpace.requireCode(words[12])
            require(linearSpace == RawLinearSpace.LINEAR_PROPHOTO_RGB)
            val whiteProvenance = RawLevelProvenance.requireCode(words[13])
            val blackProvenance = RawLevelProvenance.requireCode(words[14])
            require(whiteProvenance != RawLevelProvenance.UNKNOWN)
            require(blackProvenance != RawLevelProvenance.UNKNOWN)
            require(words[15] == 0 || words[15] == 1)

            val commonBlack = words[16].toUnsignedLong()
            val channelBlack = (17..20)
                .map { words[it].toUnsignedLong() }
                .take(activeLevelChannels)
            val blackRows = words[21]
            val blackColumns = words[22]
            val blackCount = words[23]
            require(blackRows in 0..8 && blackColumns in 0..8)
            require(blackCount in 0..BLACK_PATTERN_CAPACITY)
            require(
                (blackCount == 0 && blackRows == 0 && blackColumns == 0) ||
                    (blackRows > 0 && blackColumns > 0 &&
                        blackRows * blackColumns == blackCount),
            )
            val blackPattern = (0 until blackCount).map { index ->
                words[BLACK_PATTERN_AT + index].toUnsignedLong()
            }
            val whiteLevels = (0 until activeLevelChannels).map { index ->
                words[WHITE_LEVELS_AT + index].toUnsignedLong()
            }
            require(whiteLevels.all { it in 1L..65535L })
            val declaredMaximum = declaredBits?.let { bits ->
                if (bits == 16) 65535L else (1L shl bits) - 1L
            } ?: 65535L
            require(whiteLevels.all { it <= declaredMaximum })
            var largestSpan = 0L
            repeat(activeLevelChannels) { channel ->
                val cells = if (blackPattern.isEmpty()) listOf(0L) else blackPattern
                cells.forEach { repeatedBlack ->
                    val effectiveBlack = commonBlack + channelBlack[channel] + repeatedBlack
                    require(effectiveBlack < whiteLevels[channel])
                    largestSpan = maxOf(
                        largestSpan,
                        whiteLevels[channel] - effectiveBlack + 1L,
                    )
                }
            }
            require(effectiveBits == bitsRequired(largestSpan))

            val cfaCount = words[CFA_PATTERN_COUNT_AT]
            require(cfaCount in 0..CFA_PATTERN_CAPACITY)
            val cfaRows = words[CFA_PATTERN_ROWS_AT]
            val cfaColumns = words[CFA_PATTERN_COLUMNS_AT]
            require(cfaRows in 0..6 && cfaColumns in 0..6)
            require(
                (cfaCount == 0 && cfaRows == 0 && cfaColumns == 0) ||
                    (cfaRows > 0 && cfaColumns > 0 &&
                        cfaRows * cfaColumns == cfaCount),
            )
            when (layout) {
                RawPixelLayout.BAYER_2X2 -> require(
                    cfaRows == 2 && cfaColumns == 2 && cfaCount == 4,
                )
                RawPixelLayout.XTRANS_6X6 -> require(
                    cfaRows == 6 && cfaColumns == 6 && cfaCount == 36,
                )
                RawPixelLayout.LINEAR,
                RawPixelLayout.LAYERED,
                -> require(cfaRows == 0 && cfaColumns == 0 && cfaCount == 0)
                RawPixelLayout.CUSTOM_CFA -> require(
                    cfaRows > 0 && cfaColumns > 0 && cfaCount in 1..36,
                )
                RawPixelLayout.UNKNOWN -> error("validated above")
            }
            val cfaPattern = (0 until cfaCount).map { index ->
                words[CFA_PATTERN_AT + index].also { require(it in 0..3) }
            }
            val cfaFilterCode = words[9].toUnsignedLong()
            when (layout) {
                RawPixelLayout.BAYER_2X2 -> require(cfaFilterCode > 1000L)
                RawPixelLayout.XTRANS_6X6 -> require(cfaFilterCode == 9L)
                RawPixelLayout.CUSTOM_CFA -> require(cfaFilterCode in 1L..1000L)
                RawPixelLayout.LINEAR -> require(cfaFilterCode == 0L)
                RawPixelLayout.LAYERED -> Unit
                RawPixelLayout.UNKNOWN -> error("validated above")
            }

            require(words[BASELINE_PRESENT_AT] == 0 || words[BASELINE_PRESENT_AT] == 1)
            require(
                words[LINEAR_RESPONSE_PRESENT_AT] == 0 ||
                    words[LINEAR_RESPONSE_PRESENT_AT] == 1,
            )
            val baselineExposure = reals[0].takeIf { words[BASELINE_PRESENT_AT] == 1 }
            require(baselineExposure == null ||
                (baselineExposure.isFinite() && baselineExposure in -32.0f..32.0f))
            val linearResponse = reals[1].takeIf {
                words[LINEAR_RESPONSE_PRESENT_AT] == 1
            }
            require(linearResponse == null ||
                (linearResponse.isFinite() && linearResponse > 0.0f &&
                    linearResponse <= 1.0f))
            val compression = words[COMPRESSION_AT].takeIf { it >= 0 }
            require(compression == null || compression <= 0xffff)
            if (compression == null) {
                require(byteOrder == RawByteOrder.UNKNOWN)
                require(packing == RawPacking.VENDOR_DEFINED)
            } else {
                require(byteOrder == RawByteOrder.LITTLE_ENDIAN ||
                    byteOrder == RawByteOrder.BIG_ENDIAN)
                require(declaredBits != null)
                if (compression == 1) {
                    require(
                        packing == if (declaredBits == 16) {
                            RawPacking.TIFF_WORD_16
                        } else {
                            RawPacking.TIFF_PACKED_BITS
                        },
                    )
                } else {
                    require(packing == RawPacking.LOSSLESS_COMPRESSED)
                }
            }
            val requestedMaxLongEdge = words[REQUESTED_MAX_LONG_EDGE_AT]
                .takeIf { it != 0 }
            require(requestedMaxLongEdge == null || requestedMaxLongEdge > 0)
            val outputSubsampleStep = words[OUTPUT_SUBSAMPLE_STEP_AT]
            require(outputSubsampleStep > 0)
            require(requestedMaxLongEdge != null || outputSubsampleStep == 1)

            return RawPrecisionDescriptor(
                version = CURRENT_VERSION,
                sampleFormat = sampleFormat,
                declaredBitsPerSample = declaredBits,
                effectiveBitsPerSample = effectiveBits,
                processedBitsPerSample = processedBits,
                byteOrder = byteOrder,
                packing = packing,
                pixelLayout = layout,
                colorChannels = colorChannels,
                cfaFilterCode = cfaFilterCode,
                cfaPatternRows = cfaRows,
                cfaPatternColumns = cfaColumns,
                cfaPattern = cfaPattern,
                decoderRoute = route,
                postprocessRoute = postprocess,
                linearSpace = linearSpace,
                whiteLevelProvenance = whiteProvenance,
                blackLevelProvenance = blackProvenance,
                halfSizeRequested = words[15] == 1,
                blackLevelCommon = commonBlack,
                blackLevelChannels = channelBlack,
                blackPatternRows = blackRows,
                blackPatternColumns = blackColumns,
                blackPattern = blackPattern,
                whiteLevels = whiteLevels,
                baselineExposure = baselineExposure,
                linearResponseLimit = linearResponse,
                containerCompression = compression,
                requestedMaxLongEdge = requestedMaxLongEdge,
                outputSubsampleStep = outputSubsampleStep,
                displayColorSpace = null,
            )
        }

        /** A truthful descriptor for a platform bitmap/codec fallback. */
        fun forPlatformDisplayReferredFallback(colorSpace: String): RawPrecisionDescriptor {
            require(colorSpace.isNotBlank()) { "display fallback color space is blank" }
            return routeOnly(
                decoderRoute = RawDecoderRoute.PLATFORM_DISPLAY_REFERRED,
                postprocessRoute = RawPostprocessRoute.PLATFORM_DISPLAY_REFERRED,
                linearSpace = RawLinearSpace.DISPLAY_REFERRED,
                displayColorSpace = colorSpace,
            )
        }

        internal fun forManagedLinear(colorSpace: String): RawPrecisionDescriptor = routeOnly(
            decoderRoute = RawDecoderRoute.MANAGED_LINEAR,
            postprocessRoute = RawPostprocessRoute.MANAGED_FLOAT32,
            linearSpace = RawLinearSpace.CALLER_DEFINED_LINEAR,
            displayColorSpace = colorSpace,
        )

        private fun routeOnly(
            decoderRoute: RawDecoderRoute,
            postprocessRoute: RawPostprocessRoute,
            linearSpace: RawLinearSpace,
            displayColorSpace: String,
        ) = RawPrecisionDescriptor(
            version = CURRENT_VERSION,
            sampleFormat = RawSampleFormat.UNKNOWN,
            declaredBitsPerSample = null,
            effectiveBitsPerSample = null,
            processedBitsPerSample = null,
            byteOrder = RawByteOrder.NOT_APPLICABLE,
            packing = RawPacking.UNKNOWN,
            pixelLayout = RawPixelLayout.UNKNOWN,
            colorChannels = null,
            cfaFilterCode = null,
            cfaPatternRows = 0,
            cfaPatternColumns = 0,
            cfaPattern = emptyList(),
            decoderRoute = decoderRoute,
            postprocessRoute = postprocessRoute,
            linearSpace = linearSpace,
            whiteLevelProvenance = RawLevelProvenance.UNKNOWN,
            blackLevelProvenance = RawLevelProvenance.UNKNOWN,
            halfSizeRequested = false,
            blackLevelCommon = null,
            blackLevelChannels = emptyList(),
            blackPatternRows = 0,
            blackPatternColumns = 0,
            blackPattern = emptyList(),
            whiteLevels = emptyList(),
            baselineExposure = null,
            linearResponseLimit = null,
            containerCompression = null,
            requestedMaxLongEdge = null,
            outputSubsampleStep = 1,
            displayColorSpace = displayColorSpace,
        )

        private fun Int.toUnsignedLong(): Long = toLong() and 0xffff_ffffL

        private fun bitsRequired(distinctValues: Long): Int {
            require(distinctValues > 0L)
            return Long.SIZE_BITS - java.lang.Long.numberOfLeadingZeros(distinctValues - 1L)
        }
    }
}
