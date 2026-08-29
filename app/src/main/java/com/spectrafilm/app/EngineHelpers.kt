/*
 * Spektrafilm for Android — engine asset/bitmap helpers. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app

import android.content.Context
import android.net.Uri
import android.graphics.Bitmap
import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.app.masks.MaskCompositor
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.LinearImage
import com.spectrafilm.engine.SimResult
import com.spectrafilm.libraw.RawDecodeException
import com.spectrafilm.libraw.RawDecoder
import com.spectrafilm.libraw.WhiteBalance
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlin.math.max
import kotlin.math.min

/** Recursively copy the bundled assets/spektra tree to filesDir/spektra; returns that dir. */
fun extractAssets(ctx: Context): File {
    val out = File(ctx.filesDir, "spektra")
    val am = ctx.assets
    fun copyDir(rel: String) {
        val entries = am.list(rel) ?: emptyArray()
        if (entries.isEmpty()) { // it's a file
            File(out.parentFile, rel).apply { parentFile?.mkdirs() }
            am.open(rel).use { input -> File(ctx.filesDir, rel).outputStream().use { input.copyTo(it) } }
            return
        }
        File(ctx.filesDir, rel).mkdirs()
        for (e in entries) copyDir("$rel/$e")
    }
    if (!out.exists()) copyDir("spektra")
    return out
}

/** A deterministic scene-linear ProPhoto-ish test image: horizontal exposure ramp + RGB bands. */
fun syntheticLinearImage(size: Int): LinearImage {
    val buf = ByteBuffer.allocateDirect(size * size * 3 * 4).order(ByteOrder.nativeOrder())
    val f = buf.asFloatBuffer()
    for (y in 0 until size) {
        val band = (y * 4 / size) // 0..3
        for (x in 0 until size) {
            val t = x.toFloat() / (size - 1)        // 0..1 exposure ramp
            val v = 0.02f + t * t * 0.9f            // perceptually spread linear values
            val (r, g, b) = when (band) {
                0 -> Triple(v, v, v)                // neutral
                1 -> Triple(v, v * 0.25f, v * 0.25f)// reds
                2 -> Triple(v * 0.25f, v, v * 0.25f)// greens
                else -> Triple(v * 0.25f, v * 0.25f, v) // blues
            }
            val i = (y * size + x) * 3
            f.put(i, r); f.put(i + 1, g); f.put(i + 2, b)
        }
    }
    return LinearImage(buf, size, size, colorSpace = "ProPhoto RGB")
}

/**
 * Decode a camera RAW/DNG [uri] to a scene-linear ProPhoto-RGB [LinearImage] via LibRaw
 * (decoded as ACES2065-1, then converted to the engine's ProPhoto input space).
 *
 * Lightroom-style big-file handling: rather than always decoding at full native
 * resolution (a 50-200MP DNG is a multi-hundred-MB float transient — the thing that
 * OOMs), we decode a reduced *proxy* for interactive/preview-scale targets and reserve
 * the full-resolution decode for export. Lightroom does the same with Smart Previews
 * (it edits a ~2560px proxy and only reaches for the original on export / deep zoom).
 * Concretely, when [maxEdge] is at or below [HALF_DECODE_EDGE_THRESHOLD] we ask LibRaw
 * for a half-size decode (averages each Bayer 2x2 -> 1/4 the pixels and memory), then
 * box-downsample the rest of the way to [maxEdge]. Export-scale targets decode full-res.
 *
 * [wb]/[temperatureK]/[tint] mirror the GUI "Import Raw" white-balance controls.
 */
fun decodeRawToLinear(
    ctx: Context,
    uri: Uri,
    wb: WhiteBalance,
    temperatureK: Double,
    tint: Double,
    maxEdge: Int = MAX_EDGE_PX,
): LinearImage {
    // Issue #7 mitigation: even with the fd decode (no full-file Java byte[]), LibRaw
    // still expands the RAW to a float32 buffer (12 bytes/px) natively. For preview-scale
    // targets we cut that 4x up front with a half-size decode (the proxy); for export we
    // start full-res and only fall back to half on OOM. The retry ladder then (1) flips to
    // a half-size decode if a full-res one OOMs, and (2) shrinks the output cap, so a
    // borderline device still yields a usable image rather than crashing.
    var attemptEdge = maxEdge.coerceAtLeast(MIN_RAW_FALLBACK_EDGE)
    var halfSize = maxEdge <= HALF_DECODE_EDGE_THRESHOLD
    while (true) {
        try {
            return decodeRawAtEdge(ctx, uri, wb, temperatureK, tint, attemptEdge, halfSize)
        } catch (oom: OutOfMemoryError) {
            // Encourage the collector to reclaim the failed transient before retrying.
            System.gc()
            if (!halfSize) {
                // First fallback: halve the native decode (1/4 the memory) before shrinking
                // the output — preserves more output resolution than dropping the cap would.
                halfSize = true
                continue
            }
            if (attemptEdge <= MIN_RAW_FALLBACK_EDGE) {
                // Out of headroom even at the smallest cap: surface a clear, catchable error
                // (the caller's runCatching turns this into a user-visible status, not a crash).
                throw RuntimeException(
                    "Not enough memory to decode this RAW file (too large for this device).", oom,
                )
            }
            attemptEdge = max(MIN_RAW_FALLBACK_EDGE, attemptEdge / 2)
        }
    }
}

/**
 * Read [uri] fully into a DIRECT [ByteBuffer], streaming in 1 MB chunks so the only managed
 * allocation is the small reusable chunk — never `readBytes()`'s file-sized (and transiently
 * up to 2x) Java byte[]. Used as the OOM-safer input for LibRaw's buffer decode path when the
 * fd decode isn't usable; LibRaw's in-memory open also tends to succeed where its fd open
 * failed. Returns null if the stream can't be opened or the file exceeds the 2 GiB direct-
 * buffer limit (the caller then propagates to loadSource's bounded platform decoder).
 *
 * NOTE: on Android `ByteBuffer.allocateDirect` is still a non-movable byte[] on the ART heap,
 * so this is NOT off-heap — for a pathologically large file the allocateDirect itself can OOM;
 * that case rethrows and loadSource falls back to the sample-size-bounded ImageDecoder.
 */
private fun readUriToDirectBuffer(ctx: Context, uri: Uri): ByteBuffer? {
    val pfd = ctx.contentResolver.openFileDescriptor(uri, "r") ?: return null
    return pfd.use {
        val size = it.statSize
        if (size <= 0L || size > Int.MAX_VALUE) return@use null
        val buf = ByteBuffer.allocateDirect(size.toInt()).order(ByteOrder.nativeOrder())
        java.io.FileInputStream(it.fileDescriptor).use { input ->
            val chunk = ByteArray(1 shl 20)
            while (true) {
                val n = input.read(chunk)
                if (n < 0) break
                buf.put(chunk, 0, n)
            }
        }
        buf.flip()
        buf
    }
}

/** Smallest longest-edge cap the RAW OOM-retry ladder will fall back to before giving up. */
private const val MIN_RAW_FALLBACK_EDGE = 512

/**
 * At or below this target longest-edge, decode the RAW at half native resolution (the
 * interactive proxy). Above it (export-scale), decode full-res. The preview/magnifier cap
 * (MAX_EDGE_PX = 2048) sits below this; EXPORT_MAX_EDGE_PX is far above.
 */
private const val HALF_DECODE_EDGE_THRESHOLD = 4096

/** Single RAW decode + box-downsample attempt at a specific [maxEdge] (see [decodeRawToLinear]). */
private fun decodeRawAtEdge(
    ctx: Context,
    uri: Uri,
    wb: WhiteBalance,
    temperatureK: Double,
    tint: Double,
    maxEdge: Int,
    halfSize: Boolean,
): LinearImage {
    val settings = RawDecoder.Settings(
        whiteBalance = wb, temperatureK = temperatureK, tint = tint, halfSize = halfSize,
        // Hard cap the NATIVE decode to the target edge: some DNGs ignore LibRaw half_size
        // and decode full-resolution, and the result's direct ByteBuffer is a managed
        // byte[] on Android — a 4080x3060 buffer is ~150 MB and OOMs the ART heap. With
        // this cap the native decoder downsamples before returning, so the allocation is
        // bounded by maxEdge regardless of half_size.
        maxLongEdge = maxEdge,
    )
    // Decode straight from the file descriptor: LibRaw reads through the fd, so we never
    // copy the whole RAW file into a single contiguous Java byte[] first. That byte[]
    // (100-200 MB for a 50MP Samsung Expert-RAW DNG) was itself throwing OutOfMemoryError
    // on the Java heap (growth limit ~256 MB) before LibRaw ever ran. The fd is duplicated
    // natively (caller retains ownership), so the ParcelFileDescriptor is closed here.
    // Fall back to the byte[] path only if the provider's fd can't be decoded (e.g. a
    // non-seekable/in-memory document provider that LibRaw can't seek).
    val result = try {
        ctx.contentResolver.openFileDescriptor(uri, "r")?.use {
            RawDecoder.decodeToLinear(it.fd, settings)
        } ?: error("Could not open RAW file")
    } catch (e: RawDecodeException) {
        // A real LibRaw decode verdict (lossy-JPEG / JPEG-XL Expert-RAW DNG it can't decode):
        // let it propagate so loadSource routes to the bounded platform decoder, instead of
        // swallowing it here and reading the whole file.
        throw e
    } catch (e: RuntimeException) {
        // fd-direct decode failed for a non-verdict reason (some providers' fds LibRaw can't
        // open). Retry from the file bytes via a direct ByteBuffer (avoids readBytes()'s
        // file-sized + 2x-regrowth managed array; LibRaw's buffer open often succeeds here).
        // If even that allocation fails, propagate -> loadSource's platform decoder.
        val direct = readUriToDirectBuffer(ctx, uri) ?: throw e
        RawDecoder.decodeToLinear(direct, settings)
    }
    val w = result.width
    val h = result.height

    val longest = max(w, h)
    var step = 1
    while (longest / step > maxEdge) step++

    // result.data is now a NATIVE, off-heap buffer (malloc + NewDirectByteBuffer; see
    // raw_decoder_jni.cpp) rather than a JVM-managed ByteBuffer.allocateDirect (which on
    // Android is a non-movable byte[] on the ~256 MB ART heap). For EXPORT-scale targets
    // we hand that off-heap buffer straight to the engine — keeping the full-res ~140 MB
    // off the managed heap is the actual OOM fix, modelled on Lightroom — and the caller
    // close()s the returned LinearImage to free it. For PREVIEW/magnifier-scale targets we
    // copy into a small managed buffer and free the native original right away, so the
    // proxy decode + preview cache lifecycle (which reuses the buffer across renders and
    // relies on GC) is unchanged.
    val exportScale = maxEdge > HALF_DECODE_EDGE_THRESHOLD

    if (step <= 1) {
        if (exportScale) {
            return LinearImage(
                result.data, w, h, colorSpace = result.colorSpace,
                onClose = { RawDecoder.freeOffHeap(it) },
            )
        }
        val managed = ByteBuffer.allocateDirect(w * h * 3 * 4).order(ByteOrder.nativeOrder())
        managed.asFloatBuffer().put(result.data.order(ByteOrder.nativeOrder()).asFloatBuffer())
        RawDecoder.freeOffHeap(result.data)
        return LinearImage(managed, w, h, colorSpace = result.colorSpace)
    }

    // Native ignored the maxLongEdge cap (some DNGs do): box-downsample into a managed
    // proxy, then free the off-heap native original.
    val src = result.data.order(ByteOrder.nativeOrder()).asFloatBuffer()
    val outW = (w + step - 1) / step
    val outH = (h + step - 1) / step
    val out = ByteBuffer.allocateDirect(outW * outH * 3 * 4).order(ByteOrder.nativeOrder())
    val of = out.asFloatBuffer()
    var oi = 0
    for (oy in 0 until outH) {
        val sy = oy * step
        val rowBase = sy * w
        for (ox in 0 until outW) {
            val si = (rowBase + ox * step) * 3
            of.put(oi, src.get(si)); of.put(oi + 1, src.get(si + 1)); of.put(oi + 2, src.get(si + 2))
            oi += 3
        }
    }
    RawDecoder.freeOffHeap(result.data)
    return LinearImage(out, outW, outH, colorSpace = result.colorSpace)
}

/**
 * Crop a square-ish window of [cropEdge] native pixels out of [src], centred on the
 * normalized point ([nx], [ny]) in 0..1. The window is clamped to the image bounds. The
 * returned [LinearImage] shares the same color space and is a real pixel-for-pixel slice
 * of the source float buffer (no resampling), so when it is rendered through the engine at
 * full resolution the dye-cloud grain resolves at 1:1 — this is the backing for the 100%
 * grain magnifier, not an upscale of the downscaled preview.
 */
fun cropLinearImage(src: LinearImage, nx: Float, ny: Float, cropEdge: Int): LinearImage =
    cropLinearImageRect(src, nx, ny, cropEdge, cropEdge)

/**
 * Rectangular generalization of [cropLinearImage]: crop a [cropW]×[cropH] native-pixel window
 * out of [src], centred on the normalized point ([nx], [ny]). Clamped to the image bounds, no
 * resampling. This backs the Lightroom-style zoom loupe (render the visible viewport region at
 * native resolution) as well as the square 100% magnifier.
 */
fun cropLinearImageRect(
    src: LinearImage, nx: Float, ny: Float, cropW: Int, cropH: Int,
): LinearImage {
    val w = src.width
    val h = src.height
    val cw = cropW.coerceIn(1, w)
    val ch = cropH.coerceIn(1, h)
    val cxPx = (nx.coerceIn(0f, 1f) * w).toInt()
    val cyPx = (ny.coerceIn(0f, 1f) * h).toInt()
    val x0 = (cxPx - cw / 2).coerceIn(0, w - cw)
    val y0 = (cyPx - ch / 2).coerceIn(0, h - ch)

    val sf = src.data.order(ByteOrder.nativeOrder()).asFloatBuffer()
    val out = ByteBuffer.allocateDirect(cw * ch * 3 * 4).order(ByteOrder.nativeOrder())
    val of = out.asFloatBuffer()
    var oi = 0
    for (oy in 0 until ch) {
        val rowBase = (y0 + oy) * w
        for (ox in 0 until cw) {
            val si = (rowBase + x0 + ox) * 3
            of.put(oi, sf.get(si)); of.put(oi + 1, sf.get(si + 1)); of.put(oi + 2, sf.get(si + 2))
            oi += 3
        }
    }
    return LinearImage(out, cw, ch, colorSpace = src.colorSpace)
}

/** Native pixel edge of the full-resolution magnifier crop. */
const val MAGNIFIER_CROP_PX = 512

/** Max long-edge (px) of a Lightroom-zoom ROI render — bounds cost/memory to ~screen resolution
 *  while still being far sharper than the upscaled ~640px proxy. */
const val ROI_RENDER_MAX_PX = 1600

/** Long-edge (px) of the fast DRAFT zoom-ROI render: a low-res sharp crop shown almost immediately
 *  on a zoom settle, then refined to the full ROI_RENDER_MAX_PX crop. ~5x faster than the full pass,
 *  so the zoomed region resolves quickly instead of waiting ~1s on the soft scaled proxy. */
const val ROI_DRAFT_MAX_PX = 640

/** Long-edge (px) of the live DRAFT preview rendered continuously while a control is being dragged.
 *  Small enough to render back-to-back at interactive rates from the already-cached full-edge proxy
 *  (just a smaller engine pass, never a re-decode); the crisp full preview still lands on settle.
 *  This is Lightroom's draft/final loupe behaviour, ported to the spectral CPU engine. */
const val DRAFT_RENDER_MAX_PX = 384

/**
 * Coalesces concurrent or rapidly-cancelled decodes of the SAME source key into ONE in-flight
 * decode, run in a caller-supplied STABLE [CoroutineScope]. The zoom ROI render + 100% magnifier
 * re-fire on every gesture settle and are cancelled+restarted, but a native LibRaw decode does NOT
 * stop on coroutine cancellation — so without this, one pinch could spawn several overlapping
 * full-resolution decodes (the measured battery drain). Callers await the shared [Deferred]: a
 * cancelled caller drops only its await; the decode keeps running in the stable scope and the next
 * caller awaits the same one (or, by then, hits the now-warm cache). Not for different keys — those
 * just start their own flight (a source/WB/rotation change, never a per-gesture event).
 */
class SingleFlight<T> {
    private val mutex = Mutex()
    private var key: String? = null
    private var inflight: Deferred<T>? = null

    suspend fun run(key: String, scope: CoroutineScope, block: suspend () -> T): T {
        val deferred = mutex.withLock {
            val cur = inflight
            if (this.key == key && cur != null && cur.isActive) {
                cur
            } else {
                scope.async { block() }.also { this.key = key; inflight = it }
            }
        }
        return deferred.await()
    }
}

/**
 * In-memory cache of the decoded *proxy-resolution source* [LinearImage], so that interactive
 * slider/param edits don't re-decode the RAW/photo (LibRaw decode or bitmap decode + sRGB→
 * ProPhoto linearization + EXIF/rotation) on every render. Only the look/film params change
 * between most renders, and the decoded source does NOT depend on any of them — it depends
 * only on the DECODE-affecting inputs captured in [Key].
 *
 * READ-ONLY / DEFENSIVE-COPY DECISION: verified against the engine + JNI that the input image
 * buffer is treated as strictly const, so the SAME cached [LinearImage] can be re-fed to
 * `simulatePreview` across edits with no defensive copy:
 *   - `spk_simulate` / `spk_simulate_preview` take `const spk_image* in` (spektra.h).
 *   - `preprocess_geometry` (the shared entry for both scan_film and print routes) copies
 *     `in->data` into a fresh `std::vector<double> src` and operates only on that copy
 *     (spektra.cpp ~L265-267); the input is never written.
 *   - `spk_simulate_preview` downscales `in->data` into a fresh `std::vector<float> small`
 *     (read-only read; spektra.cpp ~L901) before simulating.
 *   - The JNI obtains `in_data` via `GetDirectBufferAddress` and only reads it (spektra_jni.cpp).
 * Therefore re-using the cached buffer cannot corrupt it. (If the engine ever started writing
 * into the input, this class would have to hand out a copy instead — see [get].)
 *
 * Scope: exactly ONE cached entry (the current source at the current proxy resolution). Storing
 * a new entry drops the previous [LinearImage] reference, so its direct ByteBuffer becomes
 * eligible for GC — we never hold two large source buffers at once.
 *
 * Thread-safety: all access happens from the preview render coroutine (Dispatchers.Default /
 * .IO sequentially per render); methods are `@Synchronized` as cheap insurance since
 * [invalidate] may be called from a different scope.
 */
class DecodedSourceCache {
    /** Everything that affects the DECODE of the proxy source — and nothing that doesn't. */
    private data class Key(
        val uri: String?,
        val kind: String,
        val whiteBalance: WhiteBalance,
        val temperature: Float,
        val tint: Float,
        // Creative WB is baked into the decoded buffer by loadSource (a pre-engine CAT), so it is a
        // decode-affecting input and belongs in the key — a change re-decodes, like raw temp/tint.
        val creativeTemp: Float,
        val creativeTint: Float,
        // "Balance to film stock" (85-filter) is likewise baked into the decoded buffer and depends on
        // the film profile's reference illuminant — so the key carries the profile id when the toggle is
        // ON and "" when OFF. That way switching film with the toggle off does NOT re-decode (the input
        // is film-independent then), while toggling it or changing film with it on does.
        val filmBalance: String,
        val rotationDegrees: Int,
        val maxEdge: Int,
    )

    private var key: Key? = null
    private var image: LinearImage? = null

    /**
     * Return the cached decoded source if its key matches the supplied decode inputs, else
     * null. A null result means the caller must decode (via loadSource) and then [put] it.
     */
    @Synchronized
    fun get(
        uri: String?, kind: String, whiteBalance: WhiteBalance,
        temperature: Float, tint: Float, creativeTemp: Float, creativeTint: Float,
        filmBalance: String, rotationDegrees: Int, maxEdge: Int,
    ): LinearImage? {
        val k = Key(uri, kind, whiteBalance, temperature, tint, creativeTemp, creativeTint, filmBalance, rotationDegrees, maxEdge)
        return if (k == key) image else null
    }

    /** Store [img] as the single cached entry, releasing any previous one. */
    @Synchronized
    fun put(
        uri: String?, kind: String, whiteBalance: WhiteBalance,
        temperature: Float, tint: Float, creativeTemp: Float, creativeTint: Float,
        filmBalance: String, rotationDegrees: Int, maxEdge: Int,
        img: LinearImage,
    ) {
        // Release the previous entry. This cache only holds proxy-scale (previewMaxSize
        // <= 1024, well below HALF_DECODE_EDGE_THRESHOLD) MANAGED buffers, so close() is
        // a no-op the GC then reclaims; closing also correctly frees the native buffer
        // were an off-heap image ever cached, instead of leaking it (native memory is
        // not GC-tracked). Guarded against re-putting the same instance.
        image?.takeIf { it !== img }?.close()
        key = Key(uri, kind, whiteBalance, temperature, tint, creativeTemp, creativeTint, filmBalance, rotationDegrees, maxEdge)
        image = img
    }

    /** Drop the cached entry (e.g. on teardown), releasing its buffer. */
    @Synchronized
    fun invalidate() { image?.close(); key = null; image = null }
}

/**
 * Display-referred float RGB (0..1, already CCTF-encoded by the engine) → ARGB_8888 bitmap.
 *
 * Filled BAND-BY-BAND: the destination ARGB_8888 Bitmap is native memory (Android 8+), and
 * the only managed-heap cost is the int scratch we copy through `setPixels`. Writing one
 * horizontal strip at a time bounds that scratch to `w * bandRows` ints (~a few MB) instead
 * of a single `IntArray(w*h)` — which for a full-res export (e.g. 36 MP → 144 MB) overflowed
 * the ~256 MB ART heap and OOMed the export (device-reported on a 36 MP source). Peak managed
 * allocation is now independent of image megapixels.
 */
fun simResultToBitmap(
    data: ByteBuffer,
    w: Int,
    h: Int,
    colorSpace: ColorSpace = ColorSpace.SRGB,
): Bitmap {
    val f = data.order(ByteOrder.nativeOrder()).asFloatBuffer()
    // Tag the bitmap with the engine output space so the system color-manages it to the panel
    // (and embeds the right ICC on Bitmap.compress export) instead of assuming sRGB. native; no IntArray.
    val bmp = createTaggedBitmap(w, h, colorSpace)
    // Scratch is now a FLOAT strip plus the int strip, so the band is sized by the float
    // budget (~4 MB = 1M floats) rather than the int one. Total managed scratch stays in the
    // same class as the single 4 MB IntArray this replaced, and stays independent of image
    // megapixels — the OOM note above still holds.
    val bandRows = (1024 * 1024 / (w * 3)).coerceIn(1, h)
    val src = FloatArray(w * bandRows * 3)
    val strip = IntArray(w * bandRows)
    var y = 0
    while (y < h) {
        val rows = minOf(bandRows, h - y)
        val n = w * rows
        // Bulk-read the strip instead of three bounds-checked FloatBuffer.get() per pixel.
        // A device variant ladder put per-element buffer ops at ~51% of an equivalent
        // per-pixel loop's cost, with sequential reads costing nothing once bulk
        // (docs/research/perf-lab.md §16.9). This loop has no scatter, so that is the
        // whole of the win here.
        f.position(y * w * 3)
        f.get(src, 0, n * 3)
        packToArgb(src, strip, n)
        bmp.setPixels(strip, 0, w, 0, y, w, rows)
        y += rows
    }
    return bmp
}

/**
 * Clamp, scale, round and pack [count] interleaved RGB float32 pixels from [src] into
 * opaque ARGB_8888 ints in [dst].
 *
 * Split out as a pure function on purpose: it is the per-pixel hot loop of
 * [simResultToBitmap], and a JVM unit test can gate it while the `Bitmap.setPixels` beside
 * it cannot (`Bitmap` is an android.jar stub that throws off-device). The arithmetic is
 * character-for-character what the inlined loop did, so output is unchanged — NaN included,
 * which clamps to 0 exactly as before.
 */
internal fun packToArgb(src: FloatArray, dst: IntArray, count: Int) {
    var i = 0
    var k = 0
    while (k < count) {
        val r = (min(1f, maxOf(0f, src[i])) * 255f + 0.5f).toInt()
        val g = (min(1f, maxOf(0f, src[i + 1])) * 255f + 0.5f).toInt()
        val b = (min(1f, maxOf(0f, src[i + 2])) * 255f + 0.5f).toInt()
        dst[k++] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        i += 3
    }
}

/**
 * ARGB_8888 bitmap tagged with the android color space matching the engine output [cs] (API 26+), so
 * the system color-manages it to the display and embeds the right ICC on Bitmap.compress export —
 * instead of treating wide-gamut output as sRGB. Falls back to a plain (sRGB) bitmap on API 24–25, for
 * ACES2065_1 (no faithful 8-bit tag → [ColorManagement.displayColorSpaceName] is null), or if a device
 * rejects the space. The createBitmap-with-color-space overload is API 26; setColorSpace is only API 29.
 */
private fun createTaggedBitmap(w: Int, h: Int, cs: ColorSpace): Bitmap {
    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
        val name = ColorManagement.displayColorSpaceName(cs)
        if (name != null) {
            val tagged = runCatching {
                val acs = android.graphics.ColorSpace.get(android.graphics.ColorSpace.Named.valueOf(name))
                Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888, /* hasAlpha = */ true, acs)
            }.getOrNull()
            if (tagged != null) return tagged
        }
    }
    return Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
}

/**
 * Apply the creative output grade (gamut compression + Saturation/Vibrance) AND the local-adjustment
 * masks in place to [res]'s output buffer, then convert to a bitmap. Mutating `res.data` in place means
 * a subsequent 16-bit export ([saveSimResultAsTiff] / [saveSimResultAsPng16]) that reads the SAME [res]
 * inherits both — so preview and every export format stay WYSIWYG. Masks run AFTER the global grade
 * (local adjustments are on the final graded image). All-no-op → zero per-pixel cost.
 */
fun simResultToBitmapGraded(
    res: SimResult,
    cctfEncoded: Boolean,
    saturation: Float,
    vibrance: Float,
    gamutCompress: Float,
    localAdjustments: List<LocalAdjustment> = emptyList(),
): Bitmap = gradeBufferToBitmap(res.data, res.width, res.height, res.colorSpace,
    cctfEncoded, saturation, vibrance, gamutCompress, localAdjustments)

/**
 * The buffer-level core of [simResultToBitmapGraded]: grade [data] IN PLACE and
 * convert to a bitmap. Also the re-grade entry point for [GradeCache] hits —
 * pass a [GradeCache.Pristine.scratchCopy], never the retained master.
 */
fun gradeBufferToBitmap(
    data: java.nio.ByteBuffer,
    width: Int,
    height: Int,
    colorSpace: com.spectrafilm.engine.ColorSpace,
    cctfEncoded: Boolean,
    saturation: Float,
    vibrance: Float,
    gamutCompress: Float,
    localAdjustments: List<LocalAdjustment> = emptyList(),
): Bitmap {
    ColorGrade.applyInPlace(data, width, height, colorSpace, cctfEncoded, saturation, vibrance, gamutCompress)
    MaskCompositor.applyInPlace(data, width, height, colorSpace, cctfEncoded, localAdjustments)
    return simResultToBitmap(data, width, height, colorSpace)
}
