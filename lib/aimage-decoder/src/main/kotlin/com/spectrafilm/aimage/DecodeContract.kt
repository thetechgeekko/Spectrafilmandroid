/*
 * Spektrafilm for Android — bounded AImageDecoder experiment contract.
 * GPL-3.0-only.
 */
package com.spectrafilm.aimage

import java.util.Locale

const val AIMAGE_MAX_ENCODED_BYTES: Long = 128L * 1024L * 1024L

enum class AImageInputKind {
    JPEG,
    PNG,
    GIF,
    WEBP,
    BMP,
    ICO,
    WBMP,
    HEIF,
    DNG,
}

enum class AImagePixelFormat(
    val androidBitmapFormat: Int,
    val bytesPerPixel: Int,
) {
    RGBA_8888(androidBitmapFormat = 1, bytesPerPixel = 4),
    RGBA_F16(androidBitmapFormat = 9, bytesPerPixel = 8),
}

/** Numeric values are stable NDK ADataSpace ABI constants. */
object AImageDataSpace {
    const val UNKNOWN = 0
    const val SRGB = 142_671_872
    const val SRGB_LINEAR = 138_477_568
    const val SCRGB_LINEAR = 406_913_024
    const val SCRGB = 411_107_328
    const val DISPLAY_P3 = 143_261_696
    const val BT2020 = 147_193_856
    const val BT2020_PQ = 163_971_072
    const val BT2020_HLG = 168_165_376
}

/**
 * Opaque immutable result of one policy-enforced native probe. The sealed
 * interface has no constructor and cannot be implemented outside this module,
 * so consumers cannot relabel DNG state or forge a probe wire before decode.
 */
sealed interface AImageHeader {
    val width: Int
    val height: Int
    val platformDefaultFormat: Int
    val alphaFlags: Int
    val dataSpace: Int
    val encodedByteCount: Long
    val inputKind: AImageInputKind
    val platformMime: String

    /** DNG here is explicitly a display-referred fallback, never RAW development. */
    val displayReferredDngFallback: Boolean

}

private data class OpaqueAImageHeader(
    override val width: Int,
    override val height: Int,
    override val platformDefaultFormat: Int,
    override val alphaFlags: Int,
    override val dataSpace: Int,
    override val encodedByteCount: Long,
    override val inputKind: AImageInputKind,
    override val platformMime: String,
    val nativeWire: String,
    val declaredMimePolicy: String,
    override val displayReferredDngFallback: Boolean,
    val allowDngFallbackPolicy: Boolean,
) : AImageHeader

@JvmSynthetic
internal fun AImageHeader.nativeProbeWire(): String =
    (this as OpaqueAImageHeader).nativeWire

@JvmSynthetic
internal fun AImageHeader.normalizedDeclaredMimePolicy(): String =
    (this as OpaqueAImageHeader).declaredMimePolicy

@JvmSynthetic
internal fun AImageHeader.originalDngFallbackPolicy(): Boolean =
    (this as OpaqueAImageHeader).allowDngFallbackPolicy

@JvmSynthetic
internal fun createAImageHeader(
    width: Int,
    height: Int,
    platformDefaultFormat: Int,
    alphaFlags: Int,
    dataSpace: Int,
    encodedByteCount: Long,
    inputKind: AImageInputKind,
    platformMime: String,
    nativeWire: String,
    declaredMimePolicy: String,
    displayReferredDngFallback: Boolean,
    allowDngFallbackPolicy: Boolean,
): AImageHeader = OpaqueAImageHeader(
    width = width,
    height = height,
    platformDefaultFormat = platformDefaultFormat,
    alphaFlags = alphaFlags,
    dataSpace = dataSpace,
    encodedByteCount = encodedByteCount,
    inputKind = inputKind,
    platformMime = platformMime,
    nativeWire = nativeWire,
    declaredMimePolicy = declaredMimePolicy,
    displayReferredDngFallback = displayReferredDngFallback,
    allowDngFallbackPolicy = allowDngFallbackPolicy,
)

/** Immutable sealed plan; consumers cannot construct or implement a bypass. */
sealed interface AImageDecodePlan {
    val header: AImageHeader
    val targetWidth: Int
    val targetHeight: Int
    val pixelFormat: AImagePixelFormat
    val outputDataSpace: Int
    val setOutputDataSpace: Boolean
    val requireUnpremultiplied: Boolean
    val rowStrideBytes: Int
    val pixelByteCount: Int
    val linearProPhotoByteCount: Int

    val canConvertToLinearProPhoto: Boolean
        get() = linearProPhotoByteCount > 0
}

private data class OpaqueAImageDecodePlan(
    override val header: AImageHeader,
    override val targetWidth: Int,
    override val targetHeight: Int,
    override val pixelFormat: AImagePixelFormat,
    override val outputDataSpace: Int,
    override val setOutputDataSpace: Boolean,
    override val requireUnpremultiplied: Boolean,
    override val rowStrideBytes: Int,
    override val pixelByteCount: Int,
    override val linearProPhotoByteCount: Int,
) : AImageDecodePlan

@JvmSynthetic
internal fun createAImageDecodePlan(
    header: AImageHeader,
    targetWidth: Int,
    targetHeight: Int,
    pixelFormat: AImagePixelFormat,
    outputDataSpace: Int,
    setOutputDataSpace: Boolean,
    requireUnpremultiplied: Boolean,
    rowStrideBytes: Int,
    pixelByteCount: Int,
    linearProPhotoByteCount: Int,
): AImageDecodePlan = OpaqueAImageDecodePlan(
    header = header,
    targetWidth = targetWidth,
    targetHeight = targetHeight,
    pixelFormat = pixelFormat,
    outputDataSpace = outputDataSpace,
    setOutputDataSpace = setOutputDataSpace,
    requireUnpremultiplied = requireUnpremultiplied,
    rowStrideBytes = rowStrideBytes,
    pixelByteCount = pixelByteCount,
    linearProPhotoByteCount = linearProPhotoByteCount,
)

internal const val AIMAGE_PROBE_WIRE_VERSION = "sfaimage.probe.v1"
internal const val AIMAGE_DECODE_WIRE_VERSION = "sfaimage.decode.v1"

internal fun parseAImageProbeWire(
    wire: String,
    declaredMime: String?,
    allowDisplayReferredDngFallback: Boolean,
): AImageHeader {
    val fields = wire.split('\t')
    require(fields.size == 9 && fields[0] == AIMAGE_PROBE_WIRE_VERSION) {
        "unsupported AImageDecoder probe response"
    }
    val kind = runCatching {
        AImageInputKind.valueOf(fields[7].uppercase(Locale.ROOT))
    }.getOrElse { throw IllegalArgumentException("invalid AImageDecoder input kind", it) }
    val width = fields[1].toIntOrNull()
        ?: throw IllegalArgumentException("invalid AImageDecoder width")
    val height = fields[2].toIntOrNull()
        ?: throw IllegalArgumentException("invalid AImageDecoder height")
    val encodedBytes = fields[6].toLongOrNull()
        ?: throw IllegalArgumentException("invalid AImageDecoder encoded length")
    require(width > 0 && height > 0) { "invalid AImageDecoder geometry ${width}x$height" }
    require(encodedBytes in 1..AIMAGE_MAX_ENCODED_BYTES) {
        "AImageDecoder encoded length is outside the bounded contract"
    }
    val mime = fields[8].lowercase(Locale.ROOT)
    require(mime.isNotEmpty() && '\t' !in mime) { "invalid AImageDecoder MIME" }
    val normalizedDeclaredMime = declaredMime?.lowercase(Locale.ROOT).orEmpty()
    require(normalizedDeclaredMime.length <= 127 &&
        normalizedDeclaredMime.none { it == '\t' || it == '\r' || it == '\n' }
    ) { "invalid declared AImageDecoder MIME" }
    val dngFallback = kind == AImageInputKind.DNG
    require(!dngFallback ||
        (allowDisplayReferredDngFallback &&
            normalizedDeclaredMime == "image/x-adobe-dng")
    ) { "DNG probe lacks the explicit display-referred fallback policy" }
    return createAImageHeader(
        width = width,
        height = height,
        platformDefaultFormat = fields[3].toIntOrNull()
            ?: throw IllegalArgumentException("invalid AImageDecoder bitmap format"),
        alphaFlags = fields[4].toIntOrNull()
            ?: throw IllegalArgumentException("invalid AImageDecoder alpha flags"),
        dataSpace = fields[5].toIntOrNull()
            ?: throw IllegalArgumentException("invalid AImageDecoder dataspace"),
        encodedByteCount = encodedBytes,
        inputKind = kind,
        platformMime = mime,
        nativeWire = wire,
        declaredMimePolicy = normalizedDeclaredMime,
        displayReferredDngFallback = dngFallback,
        allowDngFallbackPolicy = allowDisplayReferredDngFallback,
    )
}

/**
 * Select an explicit output contract. Only platform-sRGB RGBA_8888 may enter the
 * app's existing inverse-sRGB -> linear-ProPhoto conversion. Wide-gamut, HDR,
 * unknown-dataspace and default-F16 inputs stay F16 and cannot silently enter it.
 */
fun planAImageDecode(header: AImageHeader, maxEdge: Int): AImageDecodePlan {
    require(maxEdge in 1..16_384) { "AImageDecoder maxEdge must be in [1,16384]" }
    require(header.displayReferredDngFallback ==
        (header.inputKind == AImageInputKind.DNG)
    ) { "AImageDecoder DNG fallback identity is inconsistent" }
    val longest = maxOf(header.width, header.height)
    val targetWidth: Int
    val targetHeight: Int
    if (longest <= maxEdge) {
        targetWidth = header.width
        targetHeight = header.height
    } else if (header.width >= header.height) {
        targetWidth = maxEdge
        targetHeight = ((header.height.toLong() * maxEdge) / header.width)
            .coerceAtLeast(1L).toInt()
    } else {
        targetHeight = maxEdge
        targetWidth = ((header.width.toLong() * maxEdge) / header.height)
            .coerceAtLeast(1L).toInt()
    }

    // A platform DNG result is display-referred fallback evidence, never the
    // sensor sample contract. Even when the platform labels that result sRGB
    // and defaults to 8888, force F16 evidence-only output so RAW fallback
    // cannot silently collapse to the admitted 8-bit working route.
    val isPlainSrgb = header.inputKind != AImageInputKind.DNG &&
        header.dataSpace == AImageDataSpace.SRGB &&
        header.platformDefaultFormat == AImagePixelFormat.RGBA_8888.androidBitmapFormat
    val pixelFormat = if (isPlainSrgb) {
        AImagePixelFormat.RGBA_8888
    } else {
        AImagePixelFormat.RGBA_F16
    }

    // NDK AImageDecoder cannot combine unpremultiplied output with scaling.
    // Reject this candidate rather than losing hidden RGB through premultiplication.
    val hasAlpha = (header.alphaFlags and 0x3) != 1 // ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE
    val scaled = targetWidth != header.width || targetHeight != header.height
    if (hasAlpha && scaled) {
        throw AImageDecoderFallbackException(
            "alpha-bearing input needs scaling; AImageDecoder cannot preserve " +
                "unpremultiplied RGB for this plan",
        )
    }

    val rowStride = checkedAImageProduct(
        targetWidth.toLong(), pixelFormat.bytesPerPixel.toLong(), "row stride",
    )
    val pixelBytes = checkedAImageProduct(
        rowStride.toLong(), targetHeight.toLong(), "pixel buffer",
    )
    val linearBytes = if (pixelFormat == AImagePixelFormat.RGBA_8888) {
        checkedAImageProduct(
            checkedAImageProduct(
                targetWidth.toLong(), targetHeight.toLong(), "pixel count",
            ).toLong(),
            3L * Float.SIZE_BYTES,
            "linear ProPhoto buffer",
        )
    } else {
        0
    }

    return createAImageDecodePlan(
        header = header,
        targetWidth = targetWidth,
        targetHeight = targetHeight,
        pixelFormat = pixelFormat,
        outputDataSpace = header.dataSpace,
        // Pin every representable source dataspace explicitly. UNKNOWN cannot
        // be transformed or proven and therefore stays evidence-only/default.
        setOutputDataSpace = header.dataSpace != AImageDataSpace.UNKNOWN,
        requireUnpremultiplied = hasAlpha,
        rowStrideBytes = rowStride,
        pixelByteCount = pixelBytes,
        linearProPhotoByteCount = linearBytes,
    )
}

private fun checkedAImageProduct(left: Long, right: Long, label: String): Int {
    require(left > 0L && right > 0L) { "$label factors must be positive" }
    val value = try {
        Math.multiplyExact(left, right)
    } catch (failure: ArithmeticException) {
        throw IllegalArgumentException("AImageDecoder $label overflow", failure)
    }
    require(value <= Int.MAX_VALUE.toLong()) {
        "AImageDecoder $label exceeds the direct-buffer limit: $value"
    }
    return value.toInt()
}
