/*
 * Spektrafilm for Android — lib:tiffwriter Kotlin facade.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Writes a baseline 16-bit-per-channel RGB TIFF (TIFF 6.0, little-endian) with an
 * optional embedded ICC profile and basic EXIF/TIFF metadata, for the M2 export
 * path. The native writer (libsftiff.so) is dependency-free; see tiff_writer.cpp.
 *
 * Pixel input is 16-bit RGB, interleaved R,G,B, row-major, width*height*3 samples.
 * This is the natural quantisation of the engine's display-referred float output
 * (SimResult / spk_image in the caller's chosen output color space). The caller
 * picks the matching ICC profile (sRGB / Display-P3 / ProPhoto / …) and passes its
 * bytes; this module does not bundle profiles.
 *
 * NOTE: app/UI wiring (choosing the profile asset, output Uri, threading) is a
 * later wave; this facade is the stable callable surface for it.
 */
package com.spectrafilm.tiffwriter

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean

private const val PACK_COPY_CHUNK_BYTES = 64 * 1024

class TiffCancellationToken {
    private val state = AtomicBoolean(false)

    val isCancelled: Boolean
        get() = state.get()

    fun cancel() {
        state.set(true)
    }

    internal val nativeSignal: AtomicBoolean
        get() = state

    internal fun throwIfCancelled() {
        if (isCancelled) throw CancellationException("TIFF write cancelled")
    }
}

internal fun checkedTiffByteCount(
    width: Int,
    height: Int,
    bytesPerSample: Int,
): Int {
    require(width > 0 && height > 0) { "TIFF dimensions must be positive" }
    require(bytesPerSample > 0) { "TIFF bytes per sample must be positive" }
    val total = try {
        val rowSamples = Math.multiplyExact(width.toLong(), 3L)
        val rowBytes = Math.multiplyExact(rowSamples, bytesPerSample.toLong())
        Math.multiplyExact(rowBytes, height.toLong())
    } catch (_: ArithmeticException) {
        throw IllegalArgumentException("TIFF pixel byte count overflow")
    }
    require(total <= Int.MAX_VALUE.toLong()) {
        "TIFF pixel byte count exceeds ByteBuffer limits"
    }
    return total.toInt()
}

internal fun checkedTiffOutputPath(outPath: String) {
    require(outPath.isNotEmpty()) { "TIFF output path must not be empty" }
    require('\u0000' !in outPath) { "TIFF output path must not contain NUL" }
}

internal fun packedTiffBuffer(
    source: ByteBuffer,
    width: Int,
    height: Int,
    bytesPerSample: Int,
    cancellation: TiffCancellationToken? = null,
): ByteBuffer {
    val requiredBytes = checkedTiffByteCount(width, height, bytesPerSample)
    require(source.remaining() >= requiredBytes) {
        "pixel buffer too small: need $requiredBytes bytes, have ${source.remaining()}"
    }
    val selected = source.duplicate().apply {
        limit(position() + requiredBytes)
    }
    cancellation?.throwIfCancelled()
    if (selected.isDirect) {
        require(selected.position() % bytesPerSample == 0) {
            "direct pixel buffer position must be $bytesPerSample-byte aligned"
        }
        return selected.slice().order(ByteOrder.LITTLE_ENDIAN)
    }
    val packed = ByteBuffer.allocateDirect(requiredBytes).order(ByteOrder.LITTLE_ENDIAN)
    while (selected.hasRemaining()) {
        cancellation?.throwIfCancelled()
        val previousLimit = selected.limit()
        selected.limit(selected.position() + minOf(PACK_COPY_CHUNK_BYTES, selected.remaining()))
        packed.put(selected)
        selected.limit(previousLimit)
    }
    cancellation?.throwIfCancelled()
    return packed.apply { flip() }
}

/**
 * EXIF ColorSpace tag values written into the EXIF sub-IFD. The authoritative
 * color definition is the embedded ICC profile; this is an advisory hint.
 */
enum class ExifColorSpace(val tagValue: Int) {
    /** EXIF ColorSpace = 1 (sRGB). Use when output space is sRGB. */
    SRGB(1),

    /** EXIF ColorSpace = 0xFFFF (Uncalibrated): any wide-gamut / non-sRGB space. */
    UNCALIBRATED(0xFFFF),
}

object TiffWriter {

    private object NativeLibrary {
        init {
            System.loadLibrary("sftiff")
        }

        fun ensureLoaded() = Unit
    }

    /**
     * Write a 16-bit RGB TIFF from a direct [ByteBuffer] of little-endian uint16
     * samples (length = width*height*3*2 bytes). Fastest path: no per-pixel copy.
     *
     * @param rgb16    direct ByteBuffer, width*height*3 little-endian uint16 samples
     * @param width    image width in pixels
     * @param height   image height in pixels
     * @param outPath  absolute filesystem path to write
     * @param icc      optional ICC profile bytes (null/empty => no ICCProfile tag)
     * @param exifColorSpace advisory EXIF ColorSpace hint
     * @param software Software/producer string (TIFF tag 305)
     * @param dateTime optional "YYYY:MM:DD HH:MM:SS" DateTime (tag 306); null => omit
     * @param packBits true => PackBits (lossless RLE); false => uncompressed
     * @return number of bytes written
     * @throws IllegalStateException on write failure
     */
    fun write(
        rgb16: ByteBuffer,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        exifColorSpace: ExifColorSpace = ExifColorSpace.UNCALIBRATED,
        software: String = "Spektrafilm",
        dateTime: String? = null,
        packBits: Boolean = false,
        cancellation: TiffCancellationToken? = null,
    ): Long {
        checkedTiffOutputPath(outPath)
        val direct = packedTiffBuffer(
            rgb16, width, height, bytesPerSample = 2, cancellation = cancellation,
        )
        NativeLibrary.ensureLoaded()
        return nativeWriteBuffer(
            direct, width, height, exifColorSpace.tagValue,
            software, dateTime, icc, packBits, outPath, cancellation?.nativeSignal,
        )
    }

    /**
     * Write a 16-bit RGB TIFF from a [ShortArray] of width*height*3 samples
     * (interpreted as unsigned 16-bit). Convenience overload.
     */
    fun write(
        rgb16: ShortArray,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        exifColorSpace: ExifColorSpace = ExifColorSpace.UNCALIBRATED,
        software: String = "Spektrafilm",
        dateTime: String? = null,
        packBits: Boolean = false,
        cancellation: TiffCancellationToken? = null,
    ): Long {
        checkedTiffOutputPath(outPath)
        val requiredSamples = checkedTiffByteCount(width, height, bytesPerSample = 2) / 2
        require(rgb16.size >= requiredSamples) {
            "short buffer too small: need $requiredSamples samples, have ${rgb16.size}"
        }
        cancellation?.throwIfCancelled()
        NativeLibrary.ensureLoaded()
        return nativeWriteShorts(
            rgb16, width, height, exifColorSpace.tagValue,
            software, dateTime, icc, packBits, outPath, cancellation?.nativeSignal,
        )
    }

    /**
     * Write a true 32-bit IEEE-float RGB TIFF (SampleFormat=3, BitsPerSample=32) from a direct
     * [ByteBuffer] of little-endian float32 samples (length = width*height*3*4 bytes). The samples
     * are written VERBATIM — no quantisation, no clamp — so this preserves the engine's full float
     * precision and any scene-linear / out-of-[0,1] values. Reads in Photoshop / darktable / Resolve.
     *
     * @param rgbFloat direct ByteBuffer, width*height*3 little-endian float32 samples
     * @return number of bytes written
     * @throws IllegalStateException on write failure
     */
    /**
     * 16-bit TIFF written straight from the engine's float samples: the native writer
     * quantises row by row (same clamp and rounding as [write], so the file is
     * byte-identical), which removes the caller's full uint16 image -- 75 MB at
     * 12.5 MP, and it had to be off-heap to exist at all (#175).
     *
     * @param rgbFloat direct ByteBuffer, width*height*3 little-endian float32 samples in [0,1]
     * @return number of bytes written
     * @throws IllegalStateException on write failure
     */
    fun writeFloat16(
        rgbFloat: ByteBuffer,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        exifColorSpace: ExifColorSpace = ExifColorSpace.UNCALIBRATED,
        software: String = "Spektrafilm",
        dateTime: String? = null,
        packBits: Boolean = false,
        cancellation: TiffCancellationToken? = null,
    ): Long {
        checkedTiffOutputPath(outPath)
        val direct = packedTiffBuffer(
            rgbFloat, width, height, bytesPerSample = 4, cancellation = cancellation,
        )
        NativeLibrary.ensureLoaded()
        return nativeWriteFloat16Buffer(
            direct, width, height, exifColorSpace.tagValue,
            software, dateTime, icc, packBits, outPath, cancellation?.nativeSignal,
        )
    }

    fun writeFloat32(
        rgbFloat: ByteBuffer,
        width: Int,
        height: Int,
        outPath: String,
        icc: ByteArray? = null,
        exifColorSpace: ExifColorSpace = ExifColorSpace.UNCALIBRATED,
        software: String = "Spektrafilm",
        dateTime: String? = null,
        packBits: Boolean = false,
        cancellation: TiffCancellationToken? = null,
    ): Long {
        checkedTiffOutputPath(outPath)
        val direct = packedTiffBuffer(
            rgbFloat, width, height, bytesPerSample = 4, cancellation = cancellation,
        )
        NativeLibrary.ensureLoaded()
        return nativeWriteFloatBuffer(
            direct, width, height, exifColorSpace.tagValue,
            software, dateTime, icc, packBits, outPath, cancellation?.nativeSignal,
        )
    }

    // --- native bridge (tiff_writer_jni.cpp) ---
    private external fun nativeWriteBuffer(
        rgb16: ByteBuffer, width: Int, height: Int, exifColorSpace: Int,
        software: String, dateTime: String?, icc: ByteArray?,
        packBits: Boolean, outPath: String,
        cancellationSignal: AtomicBoolean?,
    ): Long

    private external fun nativeWriteShorts(
        rgb16: ShortArray, width: Int, height: Int, exifColorSpace: Int,
        software: String, dateTime: String?, icc: ByteArray?,
        packBits: Boolean, outPath: String,
        cancellationSignal: AtomicBoolean?,
    ): Long

    private external fun nativeWriteFloat16Buffer(
        rgbFloat: ByteBuffer, width: Int, height: Int, exifColorSpace: Int,
        software: String, dateTime: String?, icc: ByteArray?,
        packBits: Boolean, outPath: String,
        cancellationSignal: AtomicBoolean?,
    ): Long

    private external fun nativeWriteFloatBuffer(
        rgbFloat: ByteBuffer, width: Int, height: Int, exifColorSpace: Int,
        software: String, dateTime: String?, icc: ByteArray?,
        packBits: Boolean, outPath: String,
        cancellationSignal: AtomicBoolean?,
    ): Long
}
