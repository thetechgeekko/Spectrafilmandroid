/*
 * Spektrafilm for Android — connected output-contract probes for ticket #174. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ColorSpace as AndroidColorSpace
import android.os.Build
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.pngwriter.PngWriter
import com.spectrafilm.tiffwriter.ExifColorSpace
import com.spectrafilm.tiffwriter.TiffWriter
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.Arrays
import java.util.zip.InflaterInputStream

/** Small deterministic encodes against the exact release app and its bundled native writers. */
object OutputContractInstrumentationChecks {
    @JvmStatic
    fun run(context: Context) {
        require(Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            "ticket #174 connected probe requires API 26+ color-managed Bitmaps"
        }
        ultraHdrIsRejectedBeforeRender()
        bitmapTagsAndSdrEncodersRoundTrip(context)

        val descriptor = rendered(
            ExportFormat.TIFF,
            ColorSpace.SRGB,
            OutputBitDepth.UINT16,
        )
        val icc = requireNotNull(ColorManagement.requireIccBytes(context, descriptor))
        require(icc.size >= 128 && ascii(icc, 36, 4) == "acsp") {
            "descriptor ICC asset is not a packaged ICC profile"
        }
        png16RoundTrip(context, icc)
        tiff16RoundTrip(context, icc)
        tiff32fRoundTrip(context, icc)
    }

    private fun ultraHdrIsRejectedBeforeRender() {
        val failure = runCatching {
            ExportOptions(
                format = ExportFormat.ULTRA_HDR,
                jpegQuality = 95,
                size = ExportSize.FULL,
                customLongEdge = 0,
                customName = "",
            ).outputDescriptor(ColorSpace.SRGB, outputCctfEncoding = true, Build.VERSION.SDK_INT)
        }.exceptionOrNull()
        require(failure is IllegalArgumentException) {
            "blocked Ultra HDR placeholder passed connected pre-render validation: $failure"
        }
    }

    private fun bitmapTagsAndSdrEncodersRoundTrip(context: Context) {
        val bitmapSpaces = listOf(
            ColorSpace.SRGB,
            ColorSpace.ADOBE_RGB,
            ColorSpace.PROPHOTO,
            ColorSpace.REC2020,
            ColorSpace.LINEAR_SRGB,
        )
        for (space in bitmapSpaces) {
            val descriptor = rendered(
                ExportFormat.JPEG,
                space,
                OutputBitDepth.UINT8,
            )
            val bitmap = bitmapFor(space)
            try {
                val expected = namedColorSpace(requireNotNull(descriptor.metadata.bitmapColorSpaceName))
                require(bitmap.colorSpace?.id == expected.id) {
                    "$space Bitmap tag ${bitmap.colorSpace} != descriptor tag $expected"
                }
            } finally {
                bitmap.recycle()
            }
        }

        // Exercise both platform encoders in sRGB and a wide-gamut profile. Reopening the bytes
        // proves Bitmap.compress preserved the descriptor-selected tag, not merely the source Bitmap.
        for (format in listOf(ExportFormat.JPEG, ExportFormat.PNG)) {
            for (space in listOf(ColorSpace.SRGB, ColorSpace.ADOBE_RGB)) {
                val descriptor = rendered(format, space, OutputBitDepth.UINT8)
                    .requireExportable(Build.VERSION.SDK_INT)
                val bitmap = bitmapFor(space)
                val file = File.createTempFile("ticket174-sdr-", ".${format.ext}", context.cacheDir)
                try {
                    val compress = when (descriptor.encoder) {
                        OutputEncoder.ANDROID_BITMAP_JPEG -> Bitmap.CompressFormat.JPEG
                        OutputEncoder.ANDROID_BITMAP_PNG -> Bitmap.CompressFormat.PNG
                        else -> error("unexpected SDR encoder ${descriptor.encoder}")
                    }
                    val sink = FileOutputStream(file)
                    try {
                        require(bitmap.compress(compress, 100, sink)) {
                            "$format Bitmap encoder returned false"
                        }
                        sink.fd.sync()
                    } finally {
                        sink.close()
                    }
                    val bytes = readFile(file)
                    if (format == ExportFormat.JPEG) {
                        require(jpegSamplePrecision(bytes) == 8) { "SDR JPEG is not 8-bit" }
                    } else {
                        val ihdr = requirePngChunk(bytes, "IHDR")
                        require(ihdr.size == 13 && u8(ihdr[8]) == 8) { "SDR PNG is not 8-bit" }
                    }
                    val reopened = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                        ?: error("$format could not be reopened")
                    try {
                        val expected = namedColorSpace(
                            requireNotNull(descriptor.metadata.bitmapColorSpaceName),
                        )
                        require(reopened.colorSpace?.id == expected.id) {
                            "$format reopened as ${reopened.colorSpace}, expected $expected"
                        }
                        require(!reopened.hasAlpha() || android.graphics.Color.alpha(reopened.getPixel(0, 0)) == 255) {
                            "$format violated the opaque-alpha contract"
                        }
                    } finally {
                        reopened.recycle()
                    }
                    if (space == ColorSpace.ADOBE_RGB) {
                        val carriesProfile = if (format == ExportFormat.JPEG) {
                            containsAscii(bytes, "ICC_PROFILE")
                        } else {
                            findPngChunk(bytes, "iCCP") != null
                        }
                        require(carriesProfile) { "$format wide-gamut encode has no ICC profile" }
                    }
                } finally {
                    bitmap.recycle()
                    file.delete()
                }
            }
        }
    }

    private fun png16RoundTrip(context: Context, icc: ByteArray) {
        val file = File.createTempFile("ticket174-png16-", ".png", context.cacheDir)
        try {
            val samples = uint16Samples()
            val written = PngWriter.write(
                samples.duplicate().order(ByteOrder.LITTLE_ENDIAN),
                2,
                1,
                file.absolutePath,
                icc = icc,
                software = "Spektrafilm ticket174",
            )
            require(written == file.length() && written > 0L) { "PNG16 writer length mismatch" }
            val bytes = readFile(file)
            val ihdr = requirePngChunk(bytes, "IHDR")
            require(ihdr.size == 13 && u8(ihdr[8]) == 16 && u8(ihdr[9]) == 2) {
                "PNG16 IHDR is not 16-bit RGB"
            }
            val embedded = inflatePngIcc(requirePngChunk(bytes, "iCCP"))
            require(Arrays.equals(embedded, icc)) { "PNG16 iCCP differs from descriptor ICC" }
        } finally {
            file.delete()
        }
    }

    private fun tiff16RoundTrip(context: Context, icc: ByteArray) {
        val file = File.createTempFile("ticket174-tiff16-", ".tif", context.cacheDir)
        try {
            val written = TiffWriter.write(
                uint16Samples().duplicate().order(ByteOrder.LITTLE_ENDIAN),
                2,
                1,
                file.absolutePath,
                icc = icc,
                exifColorSpace = ExifColorSpace.SRGB,
                software = "Spektrafilm ticket174",
                packBits = false,
            )
            require(written == file.length() && written > 0L) { "TIFF16 writer length mismatch" }
            val tiff = ParsedTiff(readFile(file))
            require(Arrays.equals(tiff.shortValues(258), intArrayOf(16, 16, 16))) {
                "TIFF16 BitsPerSample mismatch"
            }
            require(Arrays.equals(tiff.shortValues(339), intArrayOf(1, 1, 1))) {
                "TIFF16 SampleFormat is not unsigned integer"
            }
            require(Arrays.equals(tiff.bytes(34675), icc)) { "TIFF16 ICC differs from descriptor" }
            val strip = tiff.stripBytes()
            require(u16le(strip, 0) == 0 && u16le(strip, 2) == 32768 && u16le(strip, 4) == 65535) {
                "TIFF16 endpoint/midpoint samples did not round-trip"
            }
        } finally {
            file.delete()
        }
    }

    private fun tiff32fRoundTrip(context: Context, icc: ByteArray) {
        val file = File.createTempFile("ticket174-tiff32f-", ".tif", context.cacheDir)
        val expected = floatArrayOf(-0.25f, 0.5f, 1.25f, 0f, 1f, 2f)
        val samples = ByteBuffer.allocateDirect(expected.size * Float.SIZE_BYTES)
            .order(ByteOrder.LITTLE_ENDIAN)
        for (value in expected) samples.putFloat(value)
        samples.flip()
        try {
            val written = TiffWriter.writeFloat32(
                samples,
                2,
                1,
                file.absolutePath,
                icc = icc,
                exifColorSpace = ExifColorSpace.SRGB,
                software = "Spektrafilm ticket174",
                packBits = false,
            )
            require(written == file.length() && written > 0L) { "TIFF32F writer length mismatch" }
            val tiff = ParsedTiff(readFile(file))
            require(Arrays.equals(tiff.shortValues(258), intArrayOf(32, 32, 32))) {
                "TIFF32F BitsPerSample mismatch"
            }
            require(Arrays.equals(tiff.shortValues(339), intArrayOf(3, 3, 3))) {
                "TIFF32F SampleFormat is not IEEE float"
            }
            require(Arrays.equals(tiff.bytes(34675), icc)) { "TIFF32F ICC differs from descriptor" }
            val strip = ByteBuffer.wrap(tiff.stripBytes()).order(ByteOrder.LITTLE_ENDIAN)
            for (index in expected.indices) {
                require(strip.float.toRawBits() == expected[index].toRawBits()) {
                    "TIFF32F sample $index was clamped or quantized"
                }
            }
        } finally {
            file.delete()
        }
    }

    private fun rendered(
        format: ExportFormat,
        space: ColorSpace,
        depth: OutputBitDepth,
    ): OutputDescriptor = OutputDescriptor.rendered(
        format,
        space,
        outputCctfEncoding = space != ColorSpace.LINEAR_SRGB,
        bitDepth = depth,
    )

    private fun bitmapFor(space: ColorSpace): Bitmap {
        val samples = floatArrayOf(0.1f, 0.5f, 0.9f, 1f, 0.25f, 0f)
        val buffer = ByteBuffer.allocateDirect(samples.size * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        for (sample in samples) buffer.putFloat(sample)
        buffer.flip()
        return simResultToBitmap(buffer, 2, 1, space)
    }

    private fun namedColorSpace(name: String): AndroidColorSpace =
        AndroidColorSpace.get(AndroidColorSpace.Named.valueOf(name))

    private fun uint16Samples(): ByteBuffer = ByteBuffer.allocateDirect(12)
        .order(ByteOrder.LITTLE_ENDIAN)
        .apply {
            putShort(0)
            putShort(0x8000.toShort())
            putShort(0xFFFF.toShort())
            putShort(0x1234.toShort())
            putShort(0x5678.toShort())
            putShort(0x7ABC.toShort())
            flip()
        }

    private fun jpegSamplePrecision(bytes: ByteArray): Int {
        require(bytes.size >= 4 && u8(bytes[0]) == 0xFF && u8(bytes[1]) == 0xD8) {
            "invalid JPEG signature"
        }
        var offset = 2
        while (offset + 4 <= bytes.size) {
            while (offset < bytes.size && u8(bytes[offset]) != 0xFF) offset++
            while (offset < bytes.size && u8(bytes[offset]) == 0xFF) offset++
            if (offset >= bytes.size) break
            val marker = u8(bytes[offset++])
            if (marker == 0xD9 || marker == 0xDA) break
            if (marker == 0x01 || marker in 0xD0..0xD7) continue
            require(offset + 2 <= bytes.size) { "truncated JPEG segment" }
            val length = u16be(bytes, offset)
            require(length >= 2 && offset + length <= bytes.size) { "invalid JPEG segment" }
            if (isJpegSofMarker(marker)) {
                return u8(bytes[offset + 2])
            }
            offset += length
        }
        error("JPEG has no SOF sample-precision marker")
    }

    private fun isJpegSofMarker(marker: Int): Boolean = when (marker) {
        0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
        0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF -> true
        else -> false
    }

    private fun findPngChunk(bytes: ByteArray, wanted: String): ByteArray? {
        require(bytes.size >= 8 &&
            u8(bytes[0]) == 0x89 && u8(bytes[1]) == 0x50 &&
            u8(bytes[2]) == 0x4E && u8(bytes[3]) == 0x47 &&
            u8(bytes[4]) == 0x0D && u8(bytes[5]) == 0x0A &&
            u8(bytes[6]) == 0x1A && u8(bytes[7]) == 0x0A
        ) { "invalid PNG signature" }
        var offset = 8
        while (offset + 12 <= bytes.size) {
            val length = u32be(bytes, offset)
            require(length >= 0 && offset + 12L + length <= bytes.size.toLong()) {
                "invalid PNG chunk length"
            }
            val type = ascii(bytes, offset + 4, 4)
            if (type == wanted) return Arrays.copyOfRange(bytes, offset + 8, offset + 8 + length)
            offset += 12 + length
            if (type == "IEND") break
        }
        return null
    }

    private fun requirePngChunk(bytes: ByteArray, wanted: String): ByteArray =
        requireNotNull(findPngChunk(bytes, wanted)) { "PNG has no $wanted chunk" }

    private fun inflatePngIcc(data: ByteArray): ByteArray {
        var separator = -1
        var separatorSearch = 0
        while (separatorSearch < data.size) {
            if (data[separatorSearch].toInt() == 0) {
                separator = separatorSearch
                break
            }
            separatorSearch++
        }
        require(separator > 0 && separator + 2 <= data.size && u8(data[separator + 1]) == 0) {
            "invalid PNG iCCP header"
        }
        val output = ByteArrayOutputStream()
        val input = InflaterInputStream(
            ByteArrayInputStream(data, separator + 2, data.size - separator - 2),
        )
        try {
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                output.write(buffer, 0, count)
            }
        } finally {
            input.close()
        }
        return output.toByteArray()
    }

    private fun readFile(file: File): ByteArray {
        val input = FileInputStream(file)
        val output = ByteArrayOutputStream()
        try {
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                output.write(buffer, 0, count)
            }
        } finally {
            input.close()
        }
        return output.toByteArray()
    }

    private class ParsedTiff(private val data: ByteArray) {
        private data class Entry(
            val type: Int,
            val count: Int,
            val valueOffset: Int,
            val inlineOffset: Int,
        )

        private val entries: Map<Int, Entry>

        init {
            require(data.size >= 8 && data[0] == 'I'.code.toByte() && data[1] == 'I'.code.toByte()) {
                "TIFF is not little-endian"
            }
            require(u16le(data, 2) == 42) { "invalid TIFF signature" }
            val ifdOffset = u32le(data, 4)
            val count = u16le(data, ifdOffset)
            val parsed = LinkedHashMap<Int, Entry>()
            for (index in 0 until count) {
                val offset = ifdOffset + 2 + index * 12
                require(offset + 12 <= data.size) { "truncated TIFF IFD" }
                val tag = u16le(data, offset)
                parsed[tag] = Entry(
                    type = u16le(data, offset + 2),
                    count = u32le(data, offset + 4),
                    valueOffset = u32le(data, offset + 8),
                    inlineOffset = offset + 8,
                )
            }
            entries = parsed
        }

        fun shortValues(tag: Int): IntArray {
            val entry = requireEntry(tag)
            require(entry.type == 3) { "TIFF tag $tag is not SHORT" }
            val bytes = bytes(tag)
            return IntArray(entry.count) { index -> u16le(bytes, index * 2) }
        }

        fun bytes(tag: Int): ByteArray {
            val entry = requireEntry(tag)
            val typeSize = when (entry.type) {
                1, 2, 7 -> 1
                3 -> 2
                4 -> 4
                else -> error("unsupported TIFF field type ${entry.type}")
            }
            val length = Math.multiplyExact(entry.count, typeSize)
            val offset = if (length <= 4) entry.inlineOffset else entry.valueOffset
            require(offset >= 0 && offset + length <= data.size) { "TIFF tag $tag points outside file" }
            return Arrays.copyOfRange(data, offset, offset + length)
        }

        fun stripBytes(): ByteArray {
            val offset = scalarLong(273)
            val length = scalarLong(279)
            require(offset >= 0 && length >= 0 && offset + length <= data.size) {
                "TIFF strip points outside file"
            }
            return Arrays.copyOfRange(data, offset, offset + length)
        }

        private fun scalarLong(tag: Int): Int {
            val entry = requireEntry(tag)
            require(entry.type == 4 && entry.count == 1) { "TIFF tag $tag is not scalar LONG" }
            return entry.valueOffset
        }

        private fun requireEntry(tag: Int): Entry =
            requireNotNull(entries[tag]) { "TIFF tag $tag is missing" }
    }

    private fun containsAscii(bytes: ByteArray, value: String): Boolean {
        val needle = value.toByteArray(StandardCharsets.US_ASCII)
        var offset = 0
        while (offset <= bytes.size - needle.size) {
            var index = 0
            while (index < needle.size && bytes[offset + index] == needle[index]) {
                index++
            }
            if (index == needle.size) return true
            offset++
        }
        return false
    }

    private fun ascii(bytes: ByteArray, offset: Int, length: Int): String =
        String(bytes, offset, length, StandardCharsets.US_ASCII)

    private fun u8(value: Byte): Int = value.toInt() and 0xFF

    private fun u16be(bytes: ByteArray, offset: Int): Int =
        (u8(bytes[offset]) shl 8) or u8(bytes[offset + 1])

    private fun u32be(bytes: ByteArray, offset: Int): Int =
        (u8(bytes[offset]) shl 24) or (u8(bytes[offset + 1]) shl 16) or
            (u8(bytes[offset + 2]) shl 8) or u8(bytes[offset + 3])

    private fun u16le(bytes: ByteArray, offset: Int): Int =
        u8(bytes[offset]) or (u8(bytes[offset + 1]) shl 8)

    private fun u32le(bytes: ByteArray, offset: Int): Int =
        u8(bytes[offset]) or (u8(bytes[offset + 1]) shl 8) or
            (u8(bytes[offset + 2]) shl 16) or (u8(bytes[offset + 3]) shl 24)
}
