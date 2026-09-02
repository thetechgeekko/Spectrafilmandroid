/*
 * Spektrafilm for Android — image import/export helpers. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Converts a display-referred (sRGB) Bitmap into the scene-linear ProPhoto-RGB
 * float buffer the engine expects, and writes a rendered Bitmap to the gallery.
 * For 16-bit TIFF export the engine's float SimResult buffer is quantised directly
 * to uint16 and written via TiffWriter (lib:tiffwriter) — no 8-bit Bitmap round-trip.
 */
package com.spectrafilm.app

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Gainmap
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.system.Os
import androidx.exifinterface.media.ExifInterface
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.LinearImage
import com.spectrafilm.engine.NativeBufferOwner
import com.spectrafilm.engine.SimResult
import com.spectrafilm.pngwriter.PngWriter
import com.spectrafilm.tiffwriter.ExifColorSpace
import com.spectrafilm.tiffwriter.TiffWriter
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.Locale
import java.util.concurrent.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlin.math.max

/**
 * sRGB (D65) -> ProPhoto RGB (D50) linear 3x3 matrix, row-major.
 *
 * Baked constant. Derivation:
 *   M = [ProPhoto XYZ->RGB (D50)] · [Bradford CAT D65->D50] · [sRGB RGB->XYZ (D65)]
 * i.e. take linear sRGB primaries to CIE XYZ under D65, chromatically adapt the
 * white point from D65 to D50 (Bradford), then convert XYZ to linear ProPhoto RGB.
 * Values match the colour-science reference matrix for sRGB->ProPhoto (linear).
 */
private val SRGB_TO_PROPHOTO = floatArrayOf(
    0.5290825f, 0.3303437f, 0.1405738f,
    0.0982640f, 0.8734031f, 0.0283329f,
    0.0167029f, 0.1176946f, 0.8656026f,
)

/** Cap the longest edge for interactive (preview/magnifier) renders so on-device memory stays bounded. */
const val MAX_EDGE_PX = 2048

/**
 * Longest-edge cap for the FINAL export render. Export must be full-resolution (the
 * "proxy preview vs full-res export" model), so this is set high enough to be effectively
 * native for any phone camera (16384 ≈ 200 MP at 4:3) while still bounding a pathological
 * input. The engine's per-stage buffers are native (off-Java-heap) allocations, so a
 * full-res render of a typical 12–50 MP frame fits in device RAM; the RAW decode path also
 * has an OOM-retry ladder, and the export is wrapped in runCatching, so an over-large source
 * degrades/fails gracefully rather than crashing. (Was previously capped at MAX_EDGE_PX,
 * which silently downscaled e.g. a 12 MP export to ~3 MP.)
 */
const val EXPORT_MAX_EDGE_PX = 16384

/** Inverse sRGB CCTF: display sRGB (0..1) -> scene-linear (0..1). */
private fun srgbToLinear(c: Float): Float =
    if (c <= 0.04045f) c / 12.92f else Math.pow(((c + 0.055) / 1.055), 2.4).toFloat()

/** sRGB OETF: scene-linear (0..1) -> display sRGB (0..1). */
private fun linearToSrgb(c: Float): Float {
    val v = c.coerceIn(0f, 1f)
    return if (v <= 0.0031308f) v * 12.92f else (1.055f * Math.pow(v.toDouble(), 1.0 / 2.4).toFloat() - 0.055f)
}

/** ProPhoto RGB (D50) linear -> sRGB (D65) linear 3x3, row-major (inverse of [SRGB_TO_PROPHOTO]). */
private val PROPHOTO_TO_SRGB = floatArrayOf(
    2.0340429f, -0.7275221f, -0.3065211f,
    -0.2289921f, 1.2317125f, -0.0027205f,
    -0.0085287f, -0.1532223f, 1.1617509f,
)

/**
 * Convert a scene-linear wide-gamut [LinearImage] to a display-referred sRGB Bitmap for the
 * "before" reference in the compare viewer. ProPhoto-RGB sources are matrixed to sRGB; other
 * spaces (e.g. ACES2065-1 RAW) are tone-mapped through the same matrix as a reasonable
 * approximation, then sRGB-encoded. This is only a viewing reference, not a render path.
 */
fun linearToDisplayBitmap(img: LinearImage): Bitmap {
    val w = img.width
    val h = img.height
    return img.acquireDataLease().use { lease ->
        val data = lease.data
        val f = data.asFloatBuffer()
        val m = PROPHOTO_TO_SRGB
        val px = IntArray(w * h)
        for (p in 0 until w * h) {
            val i = p * 3
            val pr = f.get(i); val pg = f.get(i + 1); val pb = f.get(i + 2)
            val rl = m[0] * pr + m[1] * pg + m[2] * pb
            val gl = m[3] * pr + m[4] * pg + m[5] * pb
            val bl = m[6] * pr + m[7] * pg + m[8] * pb
            val r = (linearToSrgb(rl) * 255f + 0.5f).toInt().coerceIn(0, 255)
            val g = (linearToSrgb(gl) * 255f + 0.5f).toInt().coerceIn(0, 255)
            val b = (linearToSrgb(bl) * 255f + 0.5f).toInt().coerceIn(0, 255)
            px[p] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        }
        Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
    }
}

/**
 * Decode [uri] to a display-sRGB Bitmap, downscale so the longest edge is
 * <= [MAX_EDGE_PX], then convert to a scene-linear ProPhoto-RGB float [LinearImage].
 *
 * Steps per pixel:
 *   1. read 8-bit sRGB-encoded R,G,B (0..255 -> 0..1)
 *   2. inverse sRGB CCTF -> scene-linear sRGB
 *   3. apply the sRGB->ProPhoto linear 3x3 matrix -> linear ProPhoto RGB float
 */
fun decodeToLinearProPhoto(
    ctx: Context,
    uri: Uri,
    maxEdge: Int = MAX_EDGE_PX,
    isCancelled: () -> Boolean = { false },
): LinearImage {
    val src = decodeDownscaled(ctx, uri, maxEdge)
    try {
        // Export-scale (anything above the preview cap) decodes a full-res photo whose linear
        // float buffer (w*h*3*4) is large — 144 MB at 12 MP — so keep it OFF the managed heap.
        return bitmapToLinearProPhoto(
            src,
            offHeap = maxEdge > MAX_EDGE_PX,
            isCancelled = isCancelled,
        )
    } finally {
        src.recycle()
    }
}

/**
 * Convert an already-decoded display-sRGB [src] Bitmap to a scene-linear ProPhoto-RGB
 * float [LinearImage] (same inverse-CCTF + sRGB->ProPhoto matrix as
 * [decodeToLinearProPhoto]). Used by both the photo-picker path and the lossy/JPEG-XL
 * DNG ImageDecoder fallback (display-referred). Does not recycle [src].
 *
 * When [offHeap] is true the linear float buffer is allocated NATIVELY (malloc +
 * NewDirectByteBuffer) instead of `ByteBuffer.allocateDirect` (which on Android is a
 * non-movable byte[] on the ~256 MB ART heap) — for a full-res photo (e.g. 12 MP -> 144 MB)
 * the managed allocation OOMs the export. The returned [LinearImage] frees it via onClose.
 * Pixels are read band-by-band so the transient int scratch is a few MB, not IntArray(w*h).
 */
internal fun checkedRgbFloatByteCount(width: Int, height: Int, label: String): Int {
    require(width > 0 && height > 0) { "$label has invalid dimensions ${width}x$height" }
    val bytes = try {
        Math.multiplyExact(
            Math.multiplyExact(width.toLong(), height.toLong()),
            3L * Float.SIZE_BYTES,
        )
    } catch (failure: ArithmeticException) {
        throw IllegalArgumentException("$label dimensions overflow: ${width}x$height", failure)
    }
    require(bytes <= Int.MAX_VALUE) { "$label buffer overflow: $bytes bytes" }
    return bytes.toInt()
}

internal fun checkedRgbFloatWindow(
    data: ByteBuffer,
    width: Int,
    height: Int,
    label: String,
): FloatBuffer = checkedRgbFloatByteWindow(data, width, height, label).asFloatBuffer()

internal fun checkedRgbFloatByteWindow(
    data: ByteBuffer,
    width: Int,
    height: Int,
    label: String,
): ByteBuffer {
    require(data.isDirect) { "$label requires a direct buffer" }
    val requiredBytes = checkedRgbFloatByteCount(width, height, label)
    val logical = data.duplicate().order(ByteOrder.nativeOrder())
    require(logical.remaining() >= requiredBytes) {
        "$label buffer is truncated: ${logical.remaining()} < $requiredBytes bytes"
    }
    logical.limit(logical.position() + requiredBytes)
    return logical.slice().order(ByteOrder.nativeOrder())
}

fun bitmapToLinearProPhoto(
    src: Bitmap,
    offHeap: Boolean = false,
    isCancelled: () -> Boolean = { false },
): LinearImage {
    val w = src.width
    val h = src.height
    val byteCount = checkedRgbFloatByteCount(w, h, "bitmap conversion")
    if (isCancelled()) throw CancellationException("bitmap conversion cancelled")
    val nativeOwner = if (offHeap) NativeBufferOwner.allocate(byteCount.toLong()) else null
    val nativeLease = nativeOwner?.acquireDataLease()
    val buf = if (offHeap) {
        checkNotNull(nativeLease).data
    } else {
        ByteBuffer.allocateDirect(byteCount)
    }.order(ByteOrder.nativeOrder())
    val image = try {
        if (nativeLease != null) {
            LinearImage.fromDataLease(buf, w, h, "ProPhoto RGB", nativeLease)
        } else {
            LinearImage(buf, w, h, colorSpace = "ProPhoto RGB")
        }
    } catch (failure: Throwable) {
        nativeOwner?.close()
        throw failure
    }
    nativeOwner?.close()
    val f = buf.asFloatBuffer()
    val m = SRGB_TO_PROPHOTO
    // Read pixels in horizontal strips: avoids a full IntArray(w*h) (4 B/px) managed spike.
    val bandRows = (1024 * 1024 / w).coerceIn(1, h)
    try {
        val rowPix = IntArray(w * bandRows)
        var y = 0
        while (y < h) {
            if (isCancelled()) throw CancellationException("bitmap conversion cancelled")
            val rows = minOf(bandRows, h - y)
            src.getPixels(rowPix, 0, w, 0, y, w, rows)
            var k = 0
            var i = y * w * 3
            repeat(w * rows) {
                val argb = rowPix[k++]
                val rl = srgbToLinear(((argb shr 16) and 0xFF) / 255f)
                val gl = srgbToLinear(((argb shr 8) and 0xFF) / 255f)
                val bl = srgbToLinear((argb and 0xFF) / 255f)
                f.put(i, m[0] * rl + m[1] * gl + m[2] * bl)
                f.put(i + 1, m[3] * rl + m[4] * gl + m[5] * bl)
                f.put(i + 2, m[6] * rl + m[7] * gl + m[8] * bl)
                i += 3
            }
            y += rows
        }
        if (isCancelled()) throw CancellationException("bitmap conversion cancelled")
        return image
    } catch (failure: Throwable) {
        image.close()
        throw failure
    }
}

/**
 * Lossy / JPEG-XL DNG fallback: decode [uri] via the platform image decoder
 * ([android.graphics.ImageDecoder] on API 28+, [BitmapFactory] on 24..27) and convert
 * to a display-referred ProPhoto-RGB [LinearImage]. Used when LibRaw throws on a
 * compressed Samsung/Pixel Expert-RAW DNG. The result is display-referred (NOT linear
 * ACES scene data), so it bypasses the spectral scene-linear assumptions — preview/
 * import quality only. Downscaled to [maxEdge]; EXIF orientation is applied by the
 * caller (loadSource) just like any other source.
 */
fun decodeViaPlatform(
    ctx: Context,
    uri: Uri,
    maxEdge: Int = MAX_EDGE_PX,
    isCancelled: () -> Boolean = { false },
): LinearImage {
    val bmp: Bitmap = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        val source = android.graphics.ImageDecoder.createSource(ctx.contentResolver, uri)
        android.graphics.ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
            // Force a software ARGB_8888 bitmap (getPixels needs CPU-readable pixels) and
            // honour maxEdge with an integer sample size.
            decoder.allocator = android.graphics.ImageDecoder.ALLOCATOR_SOFTWARE
            val longest = max(info.size.width, info.size.height).coerceAtLeast(1)
            var sample = 1
            while (longest / sample > maxEdge) sample *= 2
            if (sample > 1) decoder.setTargetSampleSize(sample)
        }
    } else {
        decodeDownscaled(ctx, uri, maxEdge)
    }
    // Hard cap to maxEdge BEFORE the linear conversion. ImageDecoder.setTargetSampleSize
    // (and BitmapFactory inSampleSize) are only hints and are IGNORED for some DNG/RAW
    // decoders, which then hand back a full-resolution bitmap. bitmapToLinearProPhoto would
    // then allocateDirect(w*h*3*4) — on Android a managed (non-movable) byte[] — and a
    // 4080x3060 DNG is a ~150 MB allocation that OOMs the ART heap ("Failed to allocate a
    // 149817619 byte allocation"). Downscale the bitmap (cheap native op) so the float
    // buffer is bounded by maxEdge regardless of whether the decoder honoured the hint.
    val argb = if (bmp.config == Bitmap.Config.ARGB_8888) bmp
    else bmp.copy(Bitmap.Config.ARGB_8888, false).also { bmp.recycle() }
    val longest = max(argb.width, argb.height)
    val safe = if (longest <= maxEdge) {
        argb
    } else {
        val scale = maxEdge.toFloat() / longest
        Bitmap.createScaledBitmap(
            argb,
            (argb.width * scale).toInt().coerceAtLeast(1),
            (argb.height * scale).toInt().coerceAtLeast(1),
            true,
        ).also { argb.recycle() }
    }
    try {
        return bitmapToLinearProPhoto(
            safe,
            offHeap = maxEdge > MAX_EDGE_PX,
            isCancelled = isCancelled,
        )
    } finally {
        safe.recycle()
    }
}

/**
 * Read the SOURCE image's EXIF orientation tag from [uri] and map it to an
 * [ExifOrientation] baseline (clockwise rotation + optional horizontal mirror).
 * Returns [ExifOrientation.NONE] if the stream has no EXIF, can't be read, or is
 * already upright. This is applied to the decoded [LinearImage] BEFORE the user's
 * manual rotate steps so JPEG/HEIC imports (and lossy-DNG ImageDecoder fallbacks)
 * appear upright in preview AND export.
 */
fun readExifOrientation(ctx: Context, uri: Uri): ExifOrientation {
    return runCatching {
        ctx.contentResolver.openInputStream(uri)?.use { stream ->
            val exif = ExifInterface(stream)
            val o = exif.getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL,
            )
            ExifOrientation.fromExif(o)
        } ?: ExifOrientation.NONE
    }.getOrDefault(ExifOrientation.NONE)
}

/** Decode [uri] with inSampleSize so the longest edge is at most [maxEdge]. */
private fun decodeDownscaled(ctx: Context, uri: Uri, maxEdge: Int = MAX_EDGE_PX): Bitmap {
    val resolver = ctx.contentResolver
    // First pass: read bounds only.
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
    val longest = max(bounds.outWidth, bounds.outHeight).coerceAtLeast(1)

    var sample = 1
    while (longest / sample > maxEdge) sample *= 2

    val opts = BitmapFactory.Options().apply {
        inSampleSize = sample
        inPreferredConfig = Bitmap.Config.ARGB_8888
    }
    val decoded = resolver.openInputStream(uri)?.use {
        BitmapFactory.decodeStream(it, null, opts)
    } ?: error("Could not decode image")

    // inSampleSize is power-of-two; do a final exact downscale if still over the cap.
    val long2 = max(decoded.width, decoded.height)
    if (long2 <= maxEdge) return decoded
    val scale = maxEdge.toFloat() / long2
    val scaled = Bitmap.createScaledBitmap(
        decoded, (decoded.width * scale).toInt().coerceAtLeast(1),
        (decoded.height * scale).toInt().coerceAtLeast(1), true
    )
    if (scaled !== decoded) decoded.recycle()
    return scaled
}

/** Write baked `.cube` LUT [text] to a SAF [uri] (from CreateDocument). */
fun saveTextToUri(ctx: Context, uri: Uri, text: String) {
    writeVerifiedNewDocument(
        ctx,
        uri,
        text.toByteArray(Charsets.UTF_8),
        maxBytes = 64 * 1024 * 1024,
    )
}

/** Filesystem-safe `<film>_<print>_<size>.<cube|clf>` LUT filename from friendly stock names. */
fun lutFileName(film: String, print: String, size: Int, clf: Boolean): String {
    fun clean(s: String) = s.trim().ifEmpty { "lut" }.replace(Regex("[^A-Za-z0-9_\\-]"), "_")
    return "${clean(film)}_${clean(print)}_$size.${if (clf) "clf" else "cube"}"
}

enum class ExportFormat(val display: String, val mime: String, val ext: String) {
    PNG("PNG (8-bit)", "image/png", "png"),
    JPEG("JPEG", "image/jpeg", "jpg"),
    // Ultra HDR is a JPEG container with an embedded ISO 21496-1 / Google gain map +
    // MPF index — its MIME type stays image/jpeg and the extension stays jpg.
    ULTRA_HDR("Ultra HDR (JPEG)", "image/jpeg", "jpg"),
    TIFF("16-bit TIFF", "image/tiff", "tif"),
    // True 16-bit-per-channel PNG via lib:pngwriter (libsfpng.so) — the engine's float
    // SimResult is quantised directly to uint16, no 8-bit Bitmap round-trip (unlike PNG).
    PNG16("16-bit PNG", "image/png", "png"),
    // True 32-bit IEEE-float TIFF (lib:tiffwriter) — the engine's float SimResult written
    // VERBATIM (no quantise/clamp). Archival / scene-linear-grade-elsewhere export.
    TIFF32F("32-bit float TIFF", "image/tiff", "tif"),
    // The decoded scene-linear INPUT (before the film engine) as a 32-bit float TIFF — verbatim,
    // untagged scene-referred linear, for grading in another app (the honest "linear DNG" hand-off).
    SCENE_LINEAR_TIFF("Scene-linear input (32-bit TIFF)", "image/tiff", "tif"),
}

/**
 * Whether [format] writes a JPEG byte-stream (plain JPEG or Ultra HDR). EXIF copy via
 * androidx ExifInterface is only attempted for JPEG targets — PNG has no standard EXIF
 * segment that ExifInterface writes reliably across the 1.3.7 version, and TIFF EXIF is
 * handled by the native TiffWriter (a limited subset).
 */
private fun ExportFormat.isJpeg(): Boolean =
    this == ExportFormat.JPEG || this == ExportFormat.ULTRA_HDR

/**
 * True for the high-bit-depth formats (16-bit TIFF/PNG, 32-bit float TIFF), written straight from
 * the float SimResult rather than via an 8-bit Bitmap — so they always export at full resolution
 * (the Bitmap-based resize does not apply).
 */
fun ExportFormat.isHighBitDepth(): Boolean =
    OutputDescriptor.fixedBitDepth(this).bitsPerSample > 8

/**
 * Downscale [bmp] so its longer edge is [longEdge] px, preserving aspect and never enlarging
 * (Lightroom-style export "Dimensions"). Returns [bmp] unchanged when it already fits. A
 * post-render resample, so grain/halation are rendered at full quality first, then downsampled.
 */
fun scaleBitmapToLongEdge(bmp: Bitmap, longEdge: Int): Bitmap {
    val (w, h) = scaledDimensions(bmp.width, bmp.height, longEdge)
    return if (w == bmp.width && h == bmp.height) bmp else Bitmap.createScaledBitmap(bmp, w, h, true)
}

/**
 * Comprehensive list of standard ExifInterface TAG_* attributes copied verbatim from the
 * source image into the export. Deliberately includes GPS/location (the user wants a FULL
 * copy). Tags that MUST reflect the exported (rendered) image rather than the source —
 * orientation, software, and the width/height/pixel-dimension family — are NOT in this list;
 * they are written explicitly as overrides in [applySourceExif] AFTER this bulk copy.
 */
private val EXIF_COPY_TAGS: List<String> = listOf(
    // --- Camera / image description ---
    ExifInterface.TAG_MAKE,
    ExifInterface.TAG_MODEL,
    ExifInterface.TAG_IMAGE_DESCRIPTION,
    ExifInterface.TAG_ARTIST,
    ExifInterface.TAG_COPYRIGHT,
    ExifInterface.TAG_USER_COMMENT,
    ExifInterface.TAG_X_RESOLUTION,
    ExifInterface.TAG_Y_RESOLUTION,
    ExifInterface.TAG_RESOLUTION_UNIT,
    ExifInterface.TAG_BODY_SERIAL_NUMBER,
    ExifInterface.TAG_CAMERA_OWNER_NAME,
    // --- Lens ---
    ExifInterface.TAG_LENS_MAKE,
    ExifInterface.TAG_LENS_MODEL,
    ExifInterface.TAG_LENS_SERIAL_NUMBER,
    ExifInterface.TAG_LENS_SPECIFICATION,
    // --- Exposure ---
    ExifInterface.TAG_EXPOSURE_TIME,
    ExifInterface.TAG_F_NUMBER,
    ExifInterface.TAG_EXPOSURE_PROGRAM,
    ExifInterface.TAG_SPECTRAL_SENSITIVITY,
    ExifInterface.TAG_PHOTOGRAPHIC_SENSITIVITY,
    ExifInterface.TAG_ISO_SPEED_RATINGS,
    ExifInterface.TAG_ISO_SPEED,
    ExifInterface.TAG_SENSITIVITY_TYPE,
    ExifInterface.TAG_OECF,
    ExifInterface.TAG_SHUTTER_SPEED_VALUE,
    ExifInterface.TAG_APERTURE_VALUE,
    ExifInterface.TAG_BRIGHTNESS_VALUE,
    ExifInterface.TAG_EXPOSURE_BIAS_VALUE,
    ExifInterface.TAG_MAX_APERTURE_VALUE,
    ExifInterface.TAG_SUBJECT_DISTANCE,
    ExifInterface.TAG_METERING_MODE,
    ExifInterface.TAG_LIGHT_SOURCE,
    ExifInterface.TAG_FLASH,
    ExifInterface.TAG_FOCAL_LENGTH,
    ExifInterface.TAG_FLASH_ENERGY,
    ExifInterface.TAG_FOCAL_LENGTH_IN_35MM_FILM,
    ExifInterface.TAG_EXPOSURE_MODE,
    ExifInterface.TAG_EXPOSURE_INDEX,
    ExifInterface.TAG_DIGITAL_ZOOM_RATIO,
    ExifInterface.TAG_SCENE_CAPTURE_TYPE,
    ExifInterface.TAG_GAIN_CONTROL,
    ExifInterface.TAG_CONTRAST,
    ExifInterface.TAG_SATURATION,
    ExifInterface.TAG_SHARPNESS,
    ExifInterface.TAG_SUBJECT_DISTANCE_RANGE,
    ExifInterface.TAG_SENSING_METHOD,
    ExifInterface.TAG_FILE_SOURCE,
    ExifInterface.TAG_SCENE_TYPE,
    ExifInterface.TAG_CUSTOM_RENDERED,
    ExifInterface.TAG_SUBJECT_AREA,
    ExifInterface.TAG_SUBJECT_LOCATION,
    // --- White balance / colour ---
    ExifInterface.TAG_WHITE_BALANCE,
    ExifInterface.TAG_COLOR_SPACE,
    ExifInterface.TAG_WHITE_POINT,
    ExifInterface.TAG_PRIMARY_CHROMATICITIES,
    ExifInterface.TAG_COMPONENTS_CONFIGURATION,
    // --- Date / time ---
    ExifInterface.TAG_DATETIME,
    ExifInterface.TAG_DATETIME_ORIGINAL,
    ExifInterface.TAG_DATETIME_DIGITIZED,
    ExifInterface.TAG_SUBSEC_TIME,
    ExifInterface.TAG_SUBSEC_TIME_ORIGINAL,
    ExifInterface.TAG_SUBSEC_TIME_DIGITIZED,
    ExifInterface.TAG_OFFSET_TIME,
    ExifInterface.TAG_OFFSET_TIME_ORIGINAL,
    ExifInterface.TAG_OFFSET_TIME_DIGITIZED,
    // --- EXIF / Interop versions ---
    ExifInterface.TAG_EXIF_VERSION,
    ExifInterface.TAG_FLASHPIX_VERSION,
    ExifInterface.TAG_MAKER_NOTE,
    ExifInterface.TAG_IMAGE_UNIQUE_ID,
)

/**
 * GPS / location EXIF tags, kept SEPARATE from [EXIF_COPY_TAGS] so they can be
 * gated behind the "preserve location on export" setting (default OFF — strip).
 * Security review F3: a photo app must not silently bake location into shared files.
 */
private val EXIF_GPS_TAGS: List<String> = listOf(
    ExifInterface.TAG_GPS_VERSION_ID,
    ExifInterface.TAG_GPS_LATITUDE,
    ExifInterface.TAG_GPS_LATITUDE_REF,
    ExifInterface.TAG_GPS_LONGITUDE,
    ExifInterface.TAG_GPS_LONGITUDE_REF,
    ExifInterface.TAG_GPS_ALTITUDE,
    ExifInterface.TAG_GPS_ALTITUDE_REF,
    ExifInterface.TAG_GPS_TIMESTAMP,
    ExifInterface.TAG_GPS_DATESTAMP,
    ExifInterface.TAG_GPS_SATELLITES,
    ExifInterface.TAG_GPS_STATUS,
    ExifInterface.TAG_GPS_MEASURE_MODE,
    ExifInterface.TAG_GPS_DOP,
    ExifInterface.TAG_GPS_SPEED_REF,
    ExifInterface.TAG_GPS_SPEED,
    ExifInterface.TAG_GPS_TRACK_REF,
    ExifInterface.TAG_GPS_TRACK,
    ExifInterface.TAG_GPS_IMG_DIRECTION_REF,
    ExifInterface.TAG_GPS_IMG_DIRECTION,
    ExifInterface.TAG_GPS_MAP_DATUM,
    ExifInterface.TAG_GPS_DEST_LATITUDE_REF,
    ExifInterface.TAG_GPS_DEST_LATITUDE,
    ExifInterface.TAG_GPS_DEST_LONGITUDE_REF,
    ExifInterface.TAG_GPS_DEST_LONGITUDE,
    ExifInterface.TAG_GPS_DEST_BEARING_REF,
    ExifInterface.TAG_GPS_DEST_BEARING,
    ExifInterface.TAG_GPS_DEST_DISTANCE_REF,
    ExifInterface.TAG_GPS_DEST_DISTANCE,
    ExifInterface.TAG_GPS_PROCESSING_METHOD,
    ExifInterface.TAG_GPS_AREA_INFORMATION,
    ExifInterface.TAG_GPS_DIFFERENTIAL,
    ExifInterface.TAG_GPS_H_POSITIONING_ERROR,
)

/**
 * A snapshot of the source image's standard EXIF tags, captured from the source URI before
 * export so it can be re-applied to the exported JPEG. [tags] maps tag name -> attribute
 * string for every non-null tag in [EXIF_COPY_TAGS]. Empty when the source has no readable
 * EXIF (e.g. the synthetic demo image, or a source whose EXIF cannot be parsed).
 */
class SourceExif(val tags: Map<String, String>) {
    val isEmpty: Boolean get() = tags.isEmpty()
}

/**
 * Read all standard [EXIF_COPY_TAGS] from [sourceUri] (via the content resolver). Returns an
 * empty [SourceExif] if the URI is null, has no EXIF, or cannot be parsed — never throws.
 */
fun readSourceExif(ctx: Context, sourceUri: Uri?, keepGps: Boolean = false): SourceExif {
    if (sourceUri == null) return SourceExif(emptyMap())
    return runCatching {
        ctx.contentResolver.openInputStream(sourceUri)?.use { input ->
            val exif = ExifInterface(input)
            val map = HashMap<String, String>()
            // GPS/location is only captured when the user has opted in (default OFF);
            // never reading it keeps location out of the export entirely. (F3.)
            val tags = if (keepGps) EXIF_COPY_TAGS + EXIF_GPS_TAGS else EXIF_COPY_TAGS
            for (tag in tags) {
                exif.getAttribute(tag)?.let { map[tag] = it }
            }
            SourceExif(map)
        } ?: SourceExif(emptyMap())
    }.getOrDefault(SourceExif(emptyMap()))
}

/**
 * Apply the captured [source] EXIF to the [dest] ExifInterface (opened on the exported JPEG),
 * then write the Spektrafilm overrides and call saveAttributes(). Overrides written AFTER the
 * bulk copy so they win:
 *   - TAG_ORIENTATION = ORIENTATION_NORMAL (1): Spektrafilm bakes rotation/orientation into the
 *     exported pixels (loadSource applies manual rotation), so viewers must not re-rotate.
 *   - TAG_SOFTWARE = "Spektrafilm".
 *   - width / height / pixel-x/y dimensions = the EXPORTED dimensions (the render may be
 *     cropped/resized/rotated vs the source).
 * Works for an empty [source] too: only the overrides are written, which is the desired
 * behaviour for the demo image / EXIF-less sources.
 */
private fun applySourceExif(
    dest: ExifInterface,
    source: SourceExif,
    outW: Int,
    outH: Int,
    outputColorSpace: OutputExifColorSpace,
) {
    for ((tag, value) in source.tags) {
        runCatching { dest.setAttribute(tag, value) }
    }
    dest.setAttribute(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL.toString())
    dest.setAttribute(ExifInterface.TAG_SOFTWARE, "Spektrafilm")
    dest.setAttribute(ExifInterface.TAG_IMAGE_WIDTH, outW.toString())
    dest.setAttribute(ExifInterface.TAG_IMAGE_LENGTH, outH.toString())
    dest.setAttribute(ExifInterface.TAG_PIXEL_X_DIMENSION, outW.toString())
    dest.setAttribute(ExifInterface.TAG_PIXEL_Y_DIMENSION, outH.toString())
    // The source tag describes the camera file, not these rendered samples. Always override it.
    dest.setAttribute(ExifInterface.TAG_COLOR_SPACE, outputColorSpace.value.toString())
    dest.saveAttributes()
}

/** Apply [source] EXIF + Spektrafilm overrides to an exported JPEG at filesystem [path]. */
private fun writeExifToPath(
    path: String,
    source: SourceExif,
    outW: Int,
    outH: Int,
    outputColorSpace: OutputExifColorSpace,
) {
    applySourceExif(ExifInterface(path), source, outW, outH, outputColorSpace)
}

/**
 * Build a near-neutral Ultra HDR [Gainmap] for an SDR [base] bitmap and attach it (API 34+).
 *
 * The engine output is a display-referred SDR film look, so there is no real HDR headroom to
 * recover. We therefore attach a *near-neutral* gain map: a tiny single-pixel map whose values
 * encode a very gentle highlight lift. On an SDR display the base renders identically (gain map
 * ignored); on an HDR display the modest ratioMax gives a subtle, safe boost rather than a
 * fabricated tone-mapping. Metadata uses the android.graphics.Gainmap defaults adjusted for a
 * gentle global boost:
 *   - gain-map content: a 1x1 ALPHA_8 bitmap at 255 (full, uniform application of the ratios)
 *   - ratioMin = 1.0 (no shadow boost), ratioMax = ~1.6 (gentle highlight headroom, ~+0.7 stop)
 *   - gamma = 1.0, epsilonSdr/Hdr = small constants, displayRatioForFullHdr = ratioMax,
 *     minDisplayRatioForHdrTransition = 1.0
 *
 * NOTE (honest, not device-verified): on API 34+ the platform JPEG encoder embeds the attached
 * Gainmap into a valid Ultra HDR JPEG (base SDR primary + gain-map secondary + MPF) when
 * Bitmap.compress(JPEG, ...) is called. This is the documented Android 14 behaviour for
 * gainmap-bearing bitmaps. It has NOT been verified on a physical device in this environment.
 */
internal fun attachRenderedGainmap(
    result: SimResult,
    base: Bitmap,
    descriptor: OutputDescriptor,
): HdrGainMap.Result? {
    val contract = descriptor.metadata.hdrGainMap ?: return null
    if (Build.VERSION.SDK_INT < 34) return null
    val colorSpace = descriptor.engineColorSpace ?: return null
    val cctf = descriptor.engineCctfEncoding ?: return null
    val computed = result.acquireDataLease().use { lease ->
        HdrGainMap.compute(
            rgb = lease.data.order(java.nio.ByteOrder.nativeOrder()).asFloatBuffer(),
            width = result.width,
            height = result.height,
            colorSpace = colorSpace,
            cctfEncoded = cctf,
            downsample = contract.downsample,
            ratioMaxCeiling = contract.ratioMaxCeiling,
            epsilonSdr = contract.epsilonSdr,
            epsilonHdr = contract.epsilonHdr,
        )
    }
    val content = Bitmap.createBitmap(computed.width, computed.height, Bitmap.Config.ALPHA_8)
    // ALPHA_8 rows can be padded, so copy row by row against the bitmap's own stride rather
    // than assuming rowBytes == width.
    val rowBytes = content.rowBytes
    val staging = java.nio.ByteBuffer.allocate(rowBytes * computed.height)
    for (y in 0 until computed.height) {
        staging.position(y * rowBytes)
        staging.put(computed.alpha, y * computed.width, computed.width)
    }
    staging.rewind()
    content.copyPixelsFromBuffer(staging)

    val gainmap = Gainmap(content).apply {
        setRatioMin(contract.ratioMin, contract.ratioMin, contract.ratioMin)
        setRatioMax(computed.ratioMax, computed.ratioMax, computed.ratioMax)
        setGamma(contract.gamma, contract.gamma, contract.gamma)
        setEpsilonSdr(contract.epsilonSdr, contract.epsilonSdr, contract.epsilonSdr)
        setEpsilonHdr(contract.epsilonHdr, contract.epsilonHdr, contract.epsilonHdr)
        setDisplayRatioForFullHdr(computed.ratioMax)
        minDisplayRatioForHdrTransition = contract.minDisplayRatioForHdrTransition
    }
    base.setGainmap(gainmap)
    Diag.i(
        "gainmap ${computed.width}x${computed.height} ratioMax=${computed.ratioMax} " +
            "trueMax=${computed.maxRatio} gained=${computed.gainedPixels} " +
            "headroom=${computed.hasHeadroom}",
    )
    return computed
}

/**
 * Save [bmp] to the public gallery under Pictures/Spektrafilm as PNG or JPEG.
 * Uses scoped storage (MediaStore RELATIVE_PATH + IS_PENDING) on API 29+, and the
 * legacy direct-file + MediaStore insert path below that. Returns the content [Uri].
 *
 * For TIFF, use [saveSimResultAsTiff] instead — Bitmap.compress has no TIFF support.
 */
suspend fun saveToGallery(
    ctx: Context,
    bmp: Bitmap,
    descriptor: OutputDescriptor,
    jpegQuality: Int = 95,
    sourceExif: SourceExif = SourceExif(emptyMap()),
    displayName: String? = null,
    onCommitted: () -> Unit = {},
    /**
     * Invoked with the staged, verified artifact AFTER it has published successfully, so the
     * export cache (#179) can adopt the exact bytes that reached the gallery without re-encoding
     * or re-hashing them: the arguments are the staged file, its length and the SHA-256 the
     * publish path already computed. The stage is deleted immediately afterwards, so an
     * implementation that wants to keep it must move it. Never called for a failed or cancelled
     * export. Deliberately not an EncodedArtifact: these encoders are public API and that type
     * is internal.
     */
    onStaged: (File, Long, String) -> Unit = { _, _, _ -> },
): Uri {
    descriptor.requireExportable(Build.VERSION.SDK_INT)
    require(descriptor.encoder == OutputEncoder.ANDROID_BITMAP_PNG ||
        descriptor.encoder == OutputEncoder.ANDROID_BITMAP_JPEG) {
        "${descriptor.format} is not a Bitmap encoder contract"
    }
    val format = descriptor.format
    val expectedBitmapSpace = requireNotNull(descriptor.metadata.bitmapColorSpaceName)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
        val expected = android.graphics.ColorSpace.get(
            android.graphics.ColorSpace.Named.valueOf(expectedBitmapSpace),
        )
        require(bmp.colorSpace?.id == expected.id) {
            "Bitmap color space does not match $expectedBitmapSpace output contract"
        }
    } else {
        require(expectedBitmapSpace == "SRGB") { "Wide-gamut Bitmap export requires API 26+" }
    }
    val name = "${displayName ?: "Spektrafilm_${System.currentTimeMillis()}"}.${format.ext}"
    val compress = if (descriptor.encoder == OutputEncoder.ANDROID_BITMAP_PNG) {
        Bitmap.CompressFormat.PNG
    } else {
        Bitmap.CompressFormat.JPEG
    }
    val quality = if (descriptor.encoder == OutputEncoder.ANDROID_BITMAP_PNG) 100
    else jpegQuality.coerceIn(1, 100)
    val exportJob = currentCoroutineContext()[Job]
    val isCancelled = { exportJob?.isActive == false }
    fun ensureExportActive() {
        if (isCancelled()) throw CancellationException("bitmap export cancelled")
    }
    ensureExportActive()

    // Ultra HDR: attach a near-neutral gain map so the platform JPEG encoder emits a valid
    // Ultra HDR JPEG (base SDR + gain map + MPF). No-op below API 34. setGainmap mutates
    // [bmp] IN PLACE (the input bitmap carries the gain map); its pixel data is untouched.

    // EXIF is only writable (via androidx ExifInterface) for JPEG targets. The exported
    // dimensions are taken from the rendered bitmap (post crop/resize/rotate).
    val writeExif = descriptor.metadata.copySourceExif
    val outW = bmp.width
    val outH = bmp.height

    // Encode completely into a unique app-private stage. This makes Bitmap.compress(false),
    // EXIF failure, ENOSPC and interruption fail before any public gallery item is visible.
    val stage = File.createTempFile("spectrafilm-export-", ".${format.ext}.part", ctx.cacheDir)
    try {
        FileOutputStream(stage).use { fileOutput ->
            // Bitmap.compress is native, but it emits encoded chunks through this stream.
            // Poll at that boundary so cancellation cannot leave a stale public item.
            val output = object : OutputStream() {
                override fun write(value: Int) {
                    ensureExportActive()
                    fileOutput.write(value)
                }

                override fun write(buffer: ByteArray, offset: Int, length: Int) {
                    ensureExportActive()
                    fileOutput.write(buffer, offset, length)
                    ensureExportActive()
                }

                override fun flush() = fileOutput.flush()
            }
            val compressed = bmp.compress(compress, quality, output)
            ensureExportActive()
            if (!compressed) {
                error("Bitmap encoder returned false")
            }
            output.flush()
            fileOutput.fd.sync()
            ensureExportActive()
        }
        ensureExportActive()
        if (writeExif) writeExifToPath(
            stage.absolutePath,
            sourceExif,
            outW,
            outH,
            descriptor.metadata.exifColorSpace,
        )
        ensureExportActive()
        val artifact = EncodedArtifact.fromCompletedFile(stage, isCancelled = isCancelled)
        val published = publishStagedToGallery(
            ctx,
            artifact,
            name,
            format.mime,
            isCancelled,
            onCommitted,
        )
        onStaged(artifact.file, artifact.length, artifact.sha256)
        return published
    } finally {
        if (stage.exists() && !stage.delete()) {
            Diag.w("could not delete export stage ${stage.name}")
        }
    }
}

/**
 * Convert the descriptor's EXIF ColorSpace policy to the native TIFF writer value.
 */
private fun OutputExifColorSpace.toNativeTiffTag(): ExifColorSpace = when (this) {
    OutputExifColorSpace.SRGB -> ExifColorSpace.SRGB
    OutputExifColorSpace.UNCALIBRATED -> ExifColorSpace.UNCALIBRATED
}

/**
 * Quantise the engine's display-referred float RGB [SimResult] to 16-bit per channel
 * and write a baseline TIFF to the gallery via [TiffWriter], using MediaStore for
 * scoped-storage / legacy compatibility.
 *
 * Bit depth: the engine's leased [SimResult] buffer holds float32 RGB values already
 * CCTF-encoded in the chosen [SimResult.colorSpace]. We round-to-nearest quantise
 * each [0,1]-clamped float to [0, 65535] uint16 — a true 16-bit encode with no
 * intermediate 8-bit Bitmap step.
 *
 * ICC: the bundled profile matching the output space is embedded (see [ColorManagement]),
 * so wide-gamut exports open correctly in color-managed apps. The EXIF ColorSpace advisory
 * tag is also set — SRGB only for encoded sRGB, UNCALIBRATED otherwise.
 *
 * @param ctx     Android context (for MediaStore / cacheDir)
 * @param result  The engine SimResult whose float data is quantised to 16-bit
 * @return        The MediaStore [Uri] of the written file
 */
suspend fun saveSimResultAsTiff(
    ctx: Context,
    result: SimResult,
    descriptor: OutputDescriptor,
    displayName: String? = null,
    onCommitted: () -> Unit = {},
    /**
     * Invoked with the staged, verified artifact AFTER it has published successfully, so the
     * export cache (#179) can adopt the exact bytes that reached the gallery without re-encoding
     * or re-hashing them: the arguments are the staged file, its length and the SHA-256 the
     * publish path already computed. The stage is deleted immediately afterwards, so an
     * implementation that wants to keep it must move it. Never called for a failed or cancelled
     * export. Deliberately not an EncodedArtifact: these encoders are public API and that type
     * is internal.
     */
    onStaged: (File, Long, String) -> Unit = { _, _, _ -> },
): Uri {
    require(descriptor.encoder == OutputEncoder.NATIVE_TIFF_UINT16 ||
        descriptor.encoder == OutputEncoder.NATIVE_TIFF_FLOAT32) {
        "${descriptor.format} is not a rendered TIFF contract"
    }
    require(descriptor.engineColorSpace == result.colorSpace) {
        "Engine result ${result.colorSpace} does not match ${descriptor.engineColorSpace} output contract"
    }
    val float32 = descriptor.encoder == OutputEncoder.NATIVE_TIFF_FLOAT32
    val w = result.width
    val h = result.height
    val nSamples = checkedRgbFloatByteCount(w, h, "TIFF export") / Float.SIZE_BYTES
    val rgb16Bytes = try {
        Math.multiplyExact(nSamples, Short.SIZE_BYTES)
    } catch (failure: ArithmeticException) {
        throw IllegalArgumentException("TIFF uint16 buffer size overflow", failure)
    }
    val exportJob = currentCoroutineContext()[Job]
    fun ensureExportActive() {
        if (exportJob?.isActive == false) throw CancellationException("TIFF export cancelled")
    }
    ensureExportActive()

    val dateTime = ExportClock.exifDateTime()
    val exifCs = descriptor.metadata.exifColorSpace.toNativeTiffTag()
    val icc = ColorManagement.requireIccBytes(ctx, descriptor)

    // Write to a temp file in cacheDir first; this avoids holding a MediaStore
    // output stream open for the entire (potentially large) TiffWriter write.
    val tmpFile = File.createTempFile("spectrafilm-export-", ".tif.part", ctx.cacheDir)

    try {
        val encodedBytes = if (float32) {
            // 32-bit float TIFF: write the engine's float samples VERBATIM (no quantise/clamp).
            // The lease keeps the native float32 buffer alive for the whole native writer call.
            runCancellableTiffWrite { cancellation ->
                result.acquireDataLease().use { lease ->
                    val data = lease.data
                    TiffWriter.writeFloat32(
                        rgbFloat = checkedRgbFloatByteWindow(data, w, h, "TIFF32F export"),
                        width = w, height = h, outPath = tmpFile.absolutePath,
                        icc = icc, exifColorSpace = exifCs, software = "Spektrafilm",
                        dateTime = dateTime, packBits = false,
                        cancellation = cancellation,
                    )
                }
            }
        } else {
            // Quantise float [0,1] -> uint16 [0,65535] into an OFF-HEAP direct buffer (LE uint16).
            // ByteBuffer.allocateDirect is a managed byte[] on Android — at 100 MP that's ~600 MB on
            // the ~256 MB ART heap and OOMs. Allocate through the global native
            // coordinator and fail closed on denial; bypassing into ART would
            // violate the same hard ceiling. Freed after the writer consumes it.
            val nativeOwner = NativeBufferOwner.allocate(rgb16Bytes.toLong())
            val nativeLease = nativeOwner.acquireDataLease()
            val rgb16Buf = nativeLease.data
                .order(ByteOrder.LITTLE_ENDIAN)
            try {
                result.acquireDataLease().use { lease ->
                    val data = lease.data
                    val floatBuf = checkedRgbFloatWindow(data, w, h, "TIFF export")
                    for (i in 0 until nSamples) {
                        if ((i and 4095) == 0) ensureExportActive()
                        val v = floatBuf.get(i).coerceIn(0f, 1f)
                        val u16 = (v * 65535f + 0.5f).toInt().coerceIn(0, 65535)
                        // Write as little-endian uint16 (low byte first).
                        rgb16Buf.put((u16 and 0xFF).toByte())
                        rgb16Buf.put(((u16 shr 8) and 0xFF).toByte())
                    }
                }
                rgb16Buf.flip()
                runCancellableTiffWrite { cancellation ->
                    TiffWriter.write(
                        rgb16 = rgb16Buf,
                        width = w,
                        height = h,
                        outPath = tmpFile.absolutePath,
                        icc = icc,
                        exifColorSpace = exifCs,
                        software = "Spektrafilm",
                        dateTime = dateTime,
                        packBits = false,
                        cancellation = cancellation,
                    )
                }
            } finally {
                nativeLease.close()
                nativeOwner.close()
            }
        }

        ensureExportActive()
        val isCancelled = { exportJob?.isActive == false }
        val artifact = EncodedArtifact.fromCompletedFile(tmpFile, encodedBytes, isCancelled)
        val published = publishStagedToGallery(
            ctx,
            artifact,
            "${displayName ?: "Spektrafilm_${System.currentTimeMillis()}"}.tif",
            descriptor.format.mime,
            isCancelled,
            onCommitted,
        )
        onStaged(artifact.file, artifact.length, artifact.sha256)
        return published
    } finally {
        // Delete on ALL paths: a writer/publish throw must not leave the (potentially
        // huge) temp file behind in cacheDir. Publish has consumed the bytes by now.
        tmpFile.delete()
    }
}

/**
 * Publish an export straight from the content-addressed cache (#179), skipping decode, engine,
 * grade and encode entirely.
 *
 * The entry was already validated against its recorded key, length and SHA-256 when it was read,
 * so its digest is reused here rather than re-hashing 70 MB: publishStagedToGallery still verifies
 * what it writes and reads back, so the published bytes are checked end to end regardless.
 */
internal fun publishCachedExport(
    ctx: Context,
    entry: ExportCacheEntry,
    displayName: String,
    mime: String,
    isCancelled: () -> Boolean = { false },
    onCommitted: () -> Unit = {},
): Uri = publishStagedToGallery(
    ctx,
    EncodedArtifact(entry.payload, entry.bytes, entry.sha256),
    displayName,
    mime,
    isCancelled,
    onCommitted,
)

/** Publish one already-encoded, verified stage through the shared transaction path. */
private fun publishStagedToGallery(
    ctx: Context,
    artifact: EncodedArtifact,
    name: String,
    mime: String,
    isCancelled: () -> Boolean = { false },
    onCommitted: () -> Unit = {},
): Uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
    publishStagedImage(
        ctx,
        artifact,
        name,
        mime,
        isCancelled,
        onPublished = { onCommitted() },
    )
} else {
    publishLegacyArtifact(ctx, artifact, name, mime, isCancelled, onCommitted)
}

/** API 24–28: sibling `.part` + fsync + digest + non-overwriting rename. */
@Suppress("DEPRECATION")
private fun publishLegacyArtifact(
    ctx: Context,
    artifact: EncodedArtifact,
    requestedName: String,
    mime: String,
    isCancelled: () -> Boolean = { false },
    onCommitted: () -> Unit = {},
): Uri {
    fun ensureExportActive() {
        if (isCancelled()) throw CancellationException("legacy export publication cancelled")
    }
    ensureExportActive()
    val dir = File(
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
        "Spektrafilm",
    )
    if (!dir.isDirectory && !dir.mkdirs()) error("Could not create legacy export directory")
    val part = File.createTempFile(".spectrafilm-", ".part", dir)
    var destination: File? = null
    var destinationCommitted = false
    try {
        val copied = FileOutputStream(part).use { output ->
            val count = artifact.file.inputStream().use { input ->
                val buffer = ByteArray(256 * 1024)
                var total = 0L
                while (true) {
                    ensureExportActive()
                    val read = input.read(buffer)
                    if (read < 0) break
                    output.write(buffer, 0, read)
                    total += read
                    ensureExportActive()
                }
                total
            }
            output.flush()
            output.fd.sync()
            ensureExportActive()
            count
        }
        val copiedArtifact = EncodedArtifact.fromCompletedFile(part, copied, isCancelled)
        check(copiedArtifact.length == artifact.length && copiedArtifact.sha256 == artifact.sha256) {
            "legacy export digest mismatch"
        }
        ensureExportActive()
        destination = renameWithoutOverwrite(part, dir, requestedName, isCancelled)
        // Rename is the legacy publication linearization point. A late cancellation must
        // not turn the already-visible file into a retryable duplicate.
        destinationCommitted = true
        onCommitted()
        val finalArtifact = EncodedArtifact.fromCompletedFile(destination)
        check(finalArtifact.length == artifact.length && finalArtifact.sha256 == artifact.sha256) {
            "legacy export changed during commit"
        }
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, destination.name)
            put(MediaStore.Images.Media.MIME_TYPE, mime)
            put(MediaStore.Images.Media.DATA, destination.absolutePath)
        }
        return ctx.contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            ?: throw IOException("legacy gallery index insert failed")
    } catch (failure: Throwable) {
        if (destinationCommitted) {
            // The final file is already visible. Gallery indexing or callback/unwind failure must
            // not delete it and invite a duplicate retry; file:// is the stable committed handle.
            Diag.w("legacy export committed but gallery indexing failed: ${failure.message}")
            return Uri.fromFile(checkNotNull(destination))
        }
        val cleanup = destination ?: part
        if (cleanup.exists() && !cleanup.delete()) {
            failure.addSuppressed(IOException("could not delete failed legacy export: $cleanup"))
        }
        throw failure
    }
}

private fun renameWithoutOverwrite(
    part: File,
    dir: File,
    requestedName: String,
    isCancelled: () -> Boolean = { false },
): File {
    require('/' !in requestedName && '\\' !in requestedName && '\u0000' !in requestedName)
    return synchronized(legacyExportNameLock) {
        val dot = requestedName.lastIndexOf('.')
        val base = if (dot > 0) requestedName.substring(0, dot) else requestedName
        val extension = if (dot > 0) requestedName.substring(dot) else ""
        for (suffix in 0..9_999) {
            if (isCancelled()) throw CancellationException("legacy export publication cancelled")
            val name = if (suffix == 0) requestedName else "$base ($suffix)$extension"
            val candidate = File(dir, name)
            // createNewFile is O_EXCL: it atomically reserves this exact final name even
            // against another process. Rename then atomically replaces our own zero-byte
            // reservation with the already-fsynced sibling stage.
            if (!candidate.createNewFile()) continue
            try {
                Os.rename(part.absolutePath, candidate.absolutePath)
                return@synchronized candidate
            } catch (failure: Throwable) {
                val cleanup = runCatching {
                    if (candidate.exists() && !candidate.delete()) {
                        throw IOException("could not remove failed legacy name reservation: $candidate")
                    }
                }
                cleanup.exceptionOrNull()?.let(failure::addSuppressed)
                throw failure
            }
        }
        error("Could not allocate a unique legacy export name")
    }
}

private val legacyExportNameLock = Any()

/** API 24-28 startup cleanup for hidden, incomplete sibling stages left by process death. */
internal fun recoverAbandonedLegacyExportStages(
    context: Context,
    priorProcessCutoffMillis: Long,
): AbandonedExportStageRecoveryReport {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        return AbandonedExportStageRecoveryReport(0, 0, 0)
    }
    val dir = File(
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
        "Spektrafilm",
    )
    if (!dir.isDirectory) return AbandonedExportStageRecoveryReport(0, 0, 0)
    var examined = 0
    var removed = 0
    var retained = 0
    dir.listFiles()?.forEach { candidate ->
        if (
            candidate.isFile &&
            candidate.name.startsWith(".spectrafilm-") &&
            candidate.name.endsWith(".part") &&
            candidate.lastModified() < priorProcessCutoffMillis
        ) {
            examined++
            if (candidate.delete()) removed++ else retained++
        }
    }
    return AbandonedExportStageRecoveryReport(examined, removed, retained)
}

/**
 * Export the decoded *scene-linear input* (the linear RGB fed to the engine, before the film
 * simulation) as a 32-bit IEEE-float TIFF — the honest answer to "give me a linear file to finish
 * elsewhere". Written VERBATIM and UNTAGGED (no ICC; EXIF Uncalibrated): the data is scene-referred
 * linear in [image]'s own primaries ([LinearImage.colorSpace], e.g. ACES2065-1 for RAW), which a
 * grading app reads as linear. A display-gamma ICC would mis-describe linear data, so none is
 * embedded; the producer string records the primaries.
 */
suspend fun saveLinearInputAsTiff32f(
    ctx: Context,
    image: LinearImage,
    descriptor: OutputDescriptor,
    displayName: String? = null,
    onCommitted: () -> Unit = {},
): Uri {
    require(descriptor.format == ExportFormat.SCENE_LINEAR_TIFF &&
        descriptor.reference == OutputReference.SCENE_REFERRED &&
        descriptor.encoder == OutputEncoder.NATIVE_TIFF_FLOAT32 &&
        descriptor.quantizer == OutputQuantizer.VERBATIM_FLOAT32 &&
        descriptor.metadata.iccAssetPath == null) {
        "Scene-linear input requires the untagged verbatim TIFF32F contract"
    }
    val exportJob = currentCoroutineContext()[Job]
    val isCancelled = { exportJob?.isActive == false }
    val dateTime = ExportClock.exifDateTime()
    val tmpFile = File.createTempFile("spectrafilm-export-", ".tif.part", ctx.cacheDir)
    try {
        val encodedBytes = runCancellableTiffWrite { cancellation ->
            image.acquireDataLease().use { lease ->
                val data = lease.data
                TiffWriter.writeFloat32(
                    rgbFloat = checkedRgbFloatByteWindow(
                        data, image.width, image.height, "scene-linear TIFF export",
                    ),
                    width = image.width,
                    height = image.height,
                    outPath = tmpFile.absolutePath,
                    icc = null,
                    exifColorSpace = descriptor.metadata.exifColorSpace.toNativeTiffTag(),
                    software = "Spektrafilm (scene-linear ${image.colorSpace})",
                    dateTime = dateTime,
                    packBits = false,
                    cancellation = cancellation,
                )
            }
        }
        if (isCancelled()) {
            throw CancellationException("scene-linear TIFF export cancelled")
        }
        val artifact = EncodedArtifact.fromCompletedFile(tmpFile, encodedBytes, isCancelled)
        return publishStagedToGallery(
            ctx,
            artifact,
            "${displayName ?: "Spektrafilm_${System.currentTimeMillis()}"}_scene-linear.tif",
            descriptor.format.mime,
            isCancelled,
            onCommitted,
        )
    } finally {
        // Delete on ALL paths: a writer/publish throw must not leave the temp behind.
        tmpFile.delete()
    }
}

/**
 * Quantise the engine's display-referred float RGB [SimResult] to 16-bit per channel
 * and write a true 16-bit-per-channel RGB PNG to the gallery via [PngWriter]
 * (lib:pngwriter / libsfpng.so), using MediaStore for scoped-storage compatibility.
 *
 * Mirrors [saveSimResultAsTiff]: same round-to-nearest float[0,1] -> uint16[0,65535]
 * quantisation with no intermediate 8-bit Bitmap step. The PNG writer byte-swaps the
 * little-endian uint16 samples to big-endian internally (PNG spec), so the caller only
 * supplies native/little-endian uint16. The PNG carries an iCCP chunk with the bundled
 * profile matching the output space (see [ColorManagement]) so wide-gamut exports are tagged.
 *
 * Unlike the 8-bit [saveToGallery] PNG path (Bitmap.compress, 8 bpc), this preserves
 * the engine's full tonal precision.
 */
suspend fun saveSimResultAsPng16(
    ctx: Context,
    result: SimResult,
    descriptor: OutputDescriptor,
    displayName: String? = null,
    onCommitted: () -> Unit = {},
    /**
     * Invoked with the staged, verified artifact AFTER it has published successfully, so the
     * export cache (#179) can adopt the exact bytes that reached the gallery without re-encoding
     * or re-hashing them: the arguments are the staged file, its length and the SHA-256 the
     * publish path already computed. The stage is deleted immediately afterwards, so an
     * implementation that wants to keep it must move it. Never called for a failed or cancelled
     * export. Deliberately not an EncodedArtifact: these encoders are public API and that type
     * is internal.
     */
    onStaged: (File, Long, String) -> Unit = { _, _, _ -> },
): Uri {
    require(descriptor.encoder == OutputEncoder.NATIVE_PNG16 &&
        descriptor.quantizer == OutputQuantizer.UINT16_ROUND_CLAMP) {
        "${descriptor.format} is not a PNG16 contract"
    }
    require(descriptor.engineColorSpace == result.colorSpace) {
        "Engine result ${result.colorSpace} does not match ${descriptor.engineColorSpace} output contract"
    }
    val w = result.width
    val h = result.height
    val nSamples = checkedRgbFloatByteCount(w, h, "PNG16 export") / Float.SIZE_BYTES
    val rgb16Bytes = try {
        Math.multiplyExact(nSamples, Short.SIZE_BYTES)
    } catch (failure: ArithmeticException) {
        throw IllegalArgumentException("PNG16 uint16 buffer size overflow", failure)
    }
    val exportJob = currentCoroutineContext()[Job]
    fun ensureExportActive() {
        if (exportJob?.isActive == false) throw CancellationException("PNG16 export cancelled")
    }
    ensureExportActive()

    // Write to a temp file in cacheDir first; avoids holding a MediaStore output stream
    // open for the whole (potentially large) PNG deflate. Deleted on ALL paths by the
    // try/finally below — a writer/publish throw must not leave it behind.
    val tmpFile = File.createTempFile("spectrafilm-export-", ".png.part", ctx.cacheDir)

    try {
        // Quantise float [0,1] -> uint16 [0,65535] into an OFF-HEAP direct buffer (LE uint16).
        // ByteBuffer.allocateDirect is a managed byte[] on Android — ~600 MB at 100 MP → ART OOM.
        // Allocate through the global native coordinator and fail closed on
        // denial; falling back to ART would escape the hard ceiling.
        val nativeOwner = NativeBufferOwner.allocate(rgb16Bytes.toLong())
        val nativeLease = nativeOwner.acquireDataLease()
        val rgb16Buf = nativeLease.data
            .order(ByteOrder.LITTLE_ENDIAN)
        val encodedBytes = try {
            result.acquireDataLease().use { lease ->
                val data = lease.data
                val floatBuf = checkedRgbFloatWindow(data, w, h, "PNG16 export")
                for (i in 0 until nSamples) {
                    if ((i and 4095) == 0) ensureExportActive()
                    val v = floatBuf.get(i).coerceIn(0f, 1f)
                    val u16 = (v * 65535f + 0.5f).toInt().coerceIn(0, 65535)
                    rgb16Buf.put((u16 and 0xFF).toByte())
                    rgb16Buf.put(((u16 shr 8) and 0xFF).toByte())
                }
            }
            rgb16Buf.flip()
            runCancellablePngWrite { cancellation ->
                PngWriter.write(
                    rgb16 = rgb16Buf,
                    width = w,
                    height = h,
                    outPath = tmpFile.absolutePath,
                    icc = ColorManagement.requireIccBytes(ctx, descriptor),
                    software = "Spektrafilm",
                    cancellation = cancellation,
                )
            }
        } finally {
            nativeLease.close()
            nativeOwner.close()
        }

        ensureExportActive()
        val name = "${displayName ?: "Spektrafilm_${System.currentTimeMillis()}"}.png"
        val isCancelled = { exportJob?.isActive == false }
        val artifact = EncodedArtifact.fromCompletedFile(tmpFile, encodedBytes, isCancelled)
        val published = publishStagedToGallery(
            ctx,
            artifact,
            name,
            descriptor.format.mime,
            isCancelled,
            onCommitted,
        )
        onStaged(artifact.file, artifact.length, artifact.sha256)
        return published
    } finally {
        tmpFile.delete()
    }
}
