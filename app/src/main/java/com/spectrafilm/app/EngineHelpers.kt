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
import com.spectrafilm.libraw.LinearResult
import com.spectrafilm.libraw.DecodeStatus
import com.spectrafilm.libraw.RawDecodeCancellation
import com.spectrafilm.libraw.RawDecodeException
import com.spectrafilm.libraw.RawDecoder
import com.spectrafilm.libraw.RawInputLimits
import com.spectrafilm.libraw.WhiteBalance
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.withContext
import kotlin.coroutines.CoroutineContext
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
    require(size >= 2) { "synthetic image size must be at least 2" }
    val buf = ByteBuffer.allocateDirect(checkedRgbFloatBytes(size, size))
        .order(ByteOrder.nativeOrder())
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
    cancellation: RawDecodeCancellation? = null,
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
            return decodeRawAtEdge(
                ctx,
                uri,
                wb,
                temperatureK,
                tint,
                attemptEdge,
                halfSize,
                cancellation,
            )
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
 * failed. Returns null if the stream can't be opened; encoded inputs above the canonical
 * 64 MiB LibRaw ceiling fail typed before any provider-sized allocation or read.
 *
 * NOTE: on Android `ByteBuffer.allocateDirect` is still a non-movable byte[] on the ART heap,
 * so this is NOT off-heap — for a pathologically large file the allocateDirect itself can OOM;
 * that case rethrows and loadSource falls back to the sample-size-bounded ImageDecoder.
 */
private fun readUriToDirectBuffer(
    ctx: Context,
    uri: Uri,
    cancellation: RawDecodeCancellation?,
): ByteBuffer? {
    val pfd = ctx.contentResolver.openFileDescriptor(uri, "r") ?: return null
    return pfd.use {
        val size = it.statSize
        val capacity = checkedRawUriInputCapacity(size) ?: return@use null
        val buf = ByteBuffer.allocateDirect(capacity).order(ByteOrder.nativeOrder())
        java.io.FileInputStream(it.fileDescriptor).use { input ->
            val chunk = ByteArray(1 shl 20)
            while (true) {
                if (cancellation?.isCancellationRequested == true) {
                    throw RawDecodeException(
                        "RAW decode cancelled",
                        DecodeStatus.CANCELLED.code,
                        0,
                    )
                }
                val n = input.read(chunk)
                if (n < 0) break
                if (n > buf.remaining()) {
                    throw RawDecodeException(
                        "RAW provider returned more bytes than its declared size",
                        DecodeStatus.INPUT.code,
                        0,
                    )
                }
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
    cancellation: RawDecodeCancellation?,
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
        decodeRawFromFd(ctx, uri, settings, cancellation)
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
        val direct = readUriToDirectBuffer(ctx, uri, cancellation) ?: throw e
        RawDecoder.decodeToLinear(direct, settings, cancellation)
    }
    return rawResultToLinearImage(result, maxEdge)

    // The result is now backed by a NATIVE, off-heap buffer (malloc +
    // NewDirectByteBuffer; see
    // raw_decoder_jni.cpp) rather than a JVM-managed ByteBuffer.allocateDirect (which on
    // Android is a non-movable byte[] on the ~256 MB ART heap). For EXPORT-scale targets
    // we hand that off-heap buffer straight to the engine — keeping the full-res ~140 MB
    // off the managed heap is the actual OOM fix, modelled on Lightroom — and the caller
    // close()s the returned LinearImage to free it. For PREVIEW/magnifier-scale targets we
    // copy into a small managed buffer and free the native original right away, so the
    // proxy decode + preview cache lifecycle (which reuses the buffer across renders and
    // relies on GC) is unchanged.
    // Native ignored the maxLongEdge cap (some DNGs do): box-downsample into a managed
    // proxy, then free the off-heap native original.
}

/** Validate a provider's declared length before any provider-sized allocation or read. */
internal fun checkedRawUriInputCapacity(statSize: Long): Int? =
    if (statSize <= 0L) null else RawInputLimits.checkedCapacity(statSize)

/**
 * Converts a decoded RAW result while making the native allocation ownership explicit.
 * This consumes [result]. Export transfers its active data lease into [LinearImage];
 * proxy paths copy and close immediately. A concurrent `result.close()` cannot free
 * the native allocation while conversion or the transferred image still holds the lease.
 */
internal fun rawResultToLinearImage(result: LinearResult, maxEdge: Int): LinearImage {
    val lease = result.acquireDataLease()
    var leaseTransferred = false
    try {
        require(maxEdge > 0) { "maxEdge must be positive" }
        val width = result.width
        val height = result.height
        val requiredBytes = checkedRgbFloatBytes(width, height)
        val logical = lease.data.duplicate().order(ByteOrder.nativeOrder())
        require(logical.remaining() >= requiredBytes) {
            "RAW result buffer is truncated: ${logical.remaining()} < $requiredBytes"
        }
        logical.limit(logical.position() + requiredBytes)
        val packed = logical.slice().order(ByteOrder.nativeOrder())

        val longest = max(width, height)
        var step = 1
        while (longest / step > maxEdge) step++
        val exportScale = maxEdge > HALF_DECODE_EDGE_THRESHOLD

        return if (step <= 1 && exportScale) {
            val image = LinearImage.fromDataLease(
                packed,
                width,
                height,
                colorSpace = result.colorSpace,
                lease = lease,
            )
            leaseTransferred = true
            image
        } else if (step <= 1) {
            val managed = ByteBuffer.allocateDirect(requiredBytes)
                .order(ByteOrder.nativeOrder())
            managed.put(packed.duplicate())
            managed.flip()
            LinearImage(managed, width, height, colorSpace = result.colorSpace)
        } else {
            val src = packed.asFloatBuffer()
            val outWidth = ((width.toLong() + step - 1L) / step).toInt()
            val outHeight = ((height.toLong() + step - 1L) / step).toInt()
            val out = ByteBuffer.allocateDirect(checkedRgbFloatBytes(outWidth, outHeight))
                .order(ByteOrder.nativeOrder())
            val output = out.asFloatBuffer()
            var outputIndex = 0
            for (outputY in 0 until outHeight) {
                val sourceY = outputY * step
                val sourceRow = sourceY * width
                for (outputX in 0 until outWidth) {
                    val sourceIndex = (sourceRow + outputX * step) * 3
                    output.put(outputIndex, src.get(sourceIndex))
                    output.put(outputIndex + 1, src.get(sourceIndex + 1))
                    output.put(outputIndex + 2, src.get(sourceIndex + 2))
                    outputIndex += 3
                }
            }
            LinearImage(out, outWidth, outHeight, colorSpace = result.colorSpace)
        }
    } finally {
        // Linearize ownership consumption before dropping a local lease. For the
        // export path the transferred lease remains active until LinearImage.close().
        result.close()
        if (!leaseTransferred) lease.close()
    }
}

internal fun checkedRgbFloatBytes(width: Int, height: Int): Int {
    require(width > 0 && height > 0) { "invalid RAW dimensions ${width}x$height" }
    val bytes = width.toLong() * height.toLong() * 3L * Float.SIZE_BYTES
    require(bytes <= Int.MAX_VALUE) { "RAW image is too large: ${width}x$height" }
    return bytes.toInt()
}

/** Close the decoded result if descriptor cleanup itself fails after native success. */
private fun decodeRawFromFd(
    ctx: Context,
    uri: Uri,
    settings: RawDecoder.Settings,
    cancellation: RawDecodeCancellation?,
): LinearResult {
    val descriptor = ctx.contentResolver.openFileDescriptor(uri, "r")
        ?: error("Could not open RAW file")
    var decoded: LinearResult? = null
    try {
        decoded = RawDecoder.decodeToLinear(descriptor.fd, settings, cancellation)
        descriptor.close()
        return decoded
    } catch (failure: Throwable) {
        decoded?.close()
        runCatching { descriptor.close() }
        throw failure
    }
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
): LinearImage = src.acquireDataLease().use { sourceLease ->
    val sourceData = sourceLease.data
    val w = src.width
    val h = src.height
    val cw = cropW.coerceIn(1, w)
    val ch = cropH.coerceIn(1, h)
    val cxPx = (nx.coerceIn(0f, 1f) * w).toInt()
    val cyPx = (ny.coerceIn(0f, 1f) * h).toInt()
    val x0 = (cxPx - cw / 2).coerceIn(0, w - cw)
    val y0 = (cyPx - ch / 2).coerceIn(0, h - ch)

    val requiredSourceBytes = checkedRgbFloatBytes(w, h)
    val logicalSource = sourceData.duplicate().order(ByteOrder.nativeOrder())
    require(logicalSource.remaining() >= requiredSourceBytes) {
        "crop source buffer is truncated: ${logicalSource.remaining()} < $requiredSourceBytes"
    }
    logicalSource.limit(logicalSource.position() + requiredSourceBytes)
    val sf = logicalSource.slice().order(ByteOrder.nativeOrder()).asFloatBuffer()
    val out = ByteBuffer.allocateDirect(checkedRgbFloatBytes(cw, ch))
        .order(ByteOrder.nativeOrder())
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
    LinearImage(out, cw, ch, colorSpace = src.colorSpace)
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

/** Dispose an already-created owned value if any later construction/fill step throws. */
internal inline fun <T, R> disposeOnFailure(
    owned: T,
    dispose: (T) -> Unit,
    block: (T) -> R,
): R = try {
    block(owned)
} catch (failure: Throwable) {
    try {
        dispose(owned)
    } catch (disposeFailure: Throwable) {
        runCatching { failure.addSuppressed(disposeFailure) }
    }
    throw failure
}

/**
 * Run a resource-producing block on [context] without leaking its result in
 * [withContext]'s prompt-cancellation return window.
 *
 * A dispatcher switch can finish [block], then observe caller cancellation before the value is
 * delivered. Ordinary `withContext` discards that value. This helper keeps the produced value in
 * an atomic handoff slot until delivery succeeds; any exceptional/cancelled exit consumes the slot
 * through [dispose]. After a normal return, ownership belongs exclusively to the caller.
 */
internal suspend fun <T : Any> withOwnedContext(
    context: CoroutineContext,
    dispose: (T) -> Unit,
    block: suspend () -> T,
): T {
    val pending = AtomicReference<T?>()
    try {
        val result = withContext(context) {
            block().also { produced ->
                check(pending.compareAndSet(null, produced)) {
                    "owned context produced more than one resource"
                }
            }
        }
        check(pending.compareAndSet(result, null)) {
            "owned context resource handoff was already consumed"
        }
        return result
    } finally {
        pending.getAndSet(null)?.let(dispose)
    }
}

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
internal class SingleFlight<T> {
    private val lock = Any()
    private var key: Any? = null
    private var inflight: Deferred<T>? = null

    suspend fun run(key: Any, scope: CoroutineScope, block: suspend () -> T): T {
        var superseded: Deferred<T>? = null
        var created: Deferred<T>? = null
        val deferred = synchronized(lock) {
            val cur = inflight
            if (this.key == key && cur != null && cur.isActive) {
                cur
            } else {
                if (cur?.isActive == true) superseded = cur
                scope.async { block() }.also {
                    this.key = key
                    inflight = it
                    created = it
                }
            }
        }
        // Cleanup belongs to the stable-scope operation, not to an arbitrary waiter. A caller can
        // be cancelled while the detached decode legitimately continues; its finally block then
        // runs before completion and cannot clear the retained Deferred. invokeOnCompletion also
        // runs immediately when an Unconfined/fast block completed before registration.
        created?.let { owned ->
            owned.invokeOnCompletion {
                synchronized(lock) {
                    if (inflight === owned) {
                        inflight = null
                        this.key = null
                    }
                }
            }
        }
        // A different request cannot publish useful work. Cancellation is cooperative: a native
        // decoder that is already inside a non-cancellable call may finish, but its stale ticket is
        // rejected by DecodedSourceCache.publish and the result is closed there.
        superseded?.cancel(CancellationException("single-flight request superseded"))
        return try {
            deferred.await()
        } finally {
            // A completed Deferred retains its coroutine closure and captured request graph. Drop
            // both it and the key promptly; same-key waiters already hold their own local reference.
            synchronized(lock) {
                if (inflight === deferred && deferred.isCompleted) {
                    inflight = null
                    this.key = null
                }
            }
        }
    }

    /** Cancel and forget the current generation (source replacement / editor teardown). */
    fun invalidate() {
        val previous = synchronized(lock) {
            key = null
            inflight.also { inflight = null }
        }
        previous?.cancel(CancellationException("single-flight invalidated"))
    }

    internal fun isIdle(): Boolean = synchronized(lock) {
        key == null && inflight == null
    }
}

/**
 * In-memory cache of the decoded *proxy-resolution source* [LinearImage], so that interactive
 * slider/param edits don't re-decode the RAW/photo (LibRaw decode or bitmap decode + sRGB→
 * ProPhoto linearization + EXIF/rotation) on every render. Only the look/film params change
 * between most renders, and the decoded source does NOT depend on any of them — it depends
 * only on the DECODE-affecting inputs captured in [Request].
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
 * into the input, this class would have to hand out a copy instead — see [acquire].)
 *
 * Scope: exactly ONE cached entry (the current source at the current proxy resolution). Publishing
 * a new entry retires the previous [LinearImage], closing it now or after its last active lease.
 *
 * Thread-safety: all access happens from the preview render coroutine (Dispatchers.Default /
 * .IO sequentially per render); methods are `@Synchronized` as cheap insurance since
 * [invalidate] may be called from a different scope.
 */
internal interface DecodedSourceCacheConstructionHooks {
    fun beforeTicketConstruction() = Unit
    fun beforeEntryConstruction() = Unit
    fun beforeLeaseConstruction() = Unit

    companion object {
        val NONE: DecodedSourceCacheConstructionHooks = object : DecodedSourceCacheConstructionHooks {}
    }
}

internal class DecodedSourceCache(
    private val constructionHooks: DecodedSourceCacheConstructionHooks =
        DecodedSourceCacheConstructionHooks.NONE,
) {
    /** Everything that affects the DECODE of the proxy source — and nothing that doesn't. */
    data class Request(
        val uri: String?,
        val kind: String,
        val authorizationRequired: Boolean,
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

    /** Publication authority for one immutable logical decode request. */
    data class Ticket(
        internal val request: Request,
        internal val generation: Long,
    )

    private class Entry(
        val request: Request,
        val image: LinearImage,
        private val constructionHooks: DecodedSourceCacheConstructionHooks,
    ) {
        private var leases = 0
        private var retired = false

        @Synchronized
        fun acquire(): Lease? {
            if (retired) return null
            leases++
            return try {
                constructionHooks.beforeLeaseConstruction()
                Lease(image, ::release)
            } catch (failure: Throwable) {
                // Lease construction owns the increment. Roll it back before the exception can
                // escape, otherwise retirement permanently believes a phantom reader is active.
                release()
                throw failure
            }
        }

        @Synchronized
        fun retire() {
            if (retired) return
            retired = true
            if (leases == 0) image.close()
        }

        @Synchronized
        private fun release() {
            check(leases > 0) { "decoded-source lease underflow" }
            leases--
            if (retired && leases == 0) image.close()
        }
    }

    /** Pins one cached frame until [close], deferring eviction/invalidation cleanup. */
    class Lease internal constructor(
        val image: LinearImage,
        private val release: () -> Unit,
    ) : AutoCloseable {
        private val closed = AtomicBoolean(false)

        override fun close() {
            if (closed.compareAndSet(false, true)) release()
        }
    }

    private var entry: Entry? = null
    private var requested: Request? = null
    private var generation = 0L

    /**
     * Begin or join the newest decode generation. A different immutable request invalidates old
     * publication authority before its detached decode can complete.
     */
    fun beginRequest(request: Request): Ticket {
        var stale: Entry? = null
        val ticket = synchronized(this) {
            val changed = requested != request
            val ticketGeneration = if (changed) generation + 1L else generation
            // Construct authority before mutating/detaching current state. OOME here leaves the
            // previous request and entry fully intact instead of creating a half-switched cache.
            constructionHooks.beforeTicketConstruction()
            val created = Ticket(request, ticketGeneration)
            if (changed) {
                requested = request
                generation = ticketGeneration
                stale = entry
                entry = null
            }
            created
        }
        // Never strand the previous source while a replacement decode is pending or fails. Active
        // leases still pin it; Entry.retire closes only after the final reader exits.
        stale?.retire()
        return ticket
    }

    /**
     * Return the cached decoded source if its key matches the supplied decode inputs, else
     * null. A null result means the caller must decode the immutable request and then [publish] it.
     */
    fun acquire(ticket: Ticket): Lease? {
        val candidate = synchronized(this) {
            entry?.takeIf {
                ticket.generation == generation &&
                    ticket.request == requested &&
                    it.request == ticket.request
            }
        }
        return candidate?.acquire()
    }

    fun isCurrent(ticket: Ticket): Boolean = synchronized(this) {
        ticket.generation == generation && ticket.request == requested
    }

    /**
     * Consume [img], publishing it only while [ticket] remains current. Stale detached results are
     * closed here and can never evict a newer cache entry.
     */
    fun publish(ticket: Ticket, img: LinearImage): Boolean {
        var accepted = false
        val previous = try {
            synchronized(this) {
                if (ticket.generation != generation || ticket.request != requested) {
                    null
                } else {
                    val current = entry
                    if (current?.image !== img) {
                        // Entry construction is the ownership-transfer point for img. If it fails,
                        // the catch below closes the still-unowned incoming image exactly once.
                        constructionHooks.beforeEntryConstruction()
                        entry = Entry(ticket.request, img, constructionHooks)
                    }
                    accepted = true
                    if (current?.image !== img) current else null
                }
            }
        } catch (failure: Throwable) {
            img.close()
            throw failure
        }
        if (accepted) {
            previous?.retire()
        } else {
            img.close()
        }
        return accepted
    }

    /** Drop the cached entry (e.g. on teardown), releasing its buffer. */
    fun invalidate() {
        val previous = synchronized(this) {
            generation++
            requested = null
            entry.also { entry = null }
        }
        previous?.retire()
    }
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
    return disposeOnFailure(
        owned = bmp,
        dispose = { bitmap -> if (!bitmap.isRecycled) bitmap.recycle() },
    ) { ownedBitmap ->
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
            ownedBitmap.setPixels(strip, 0, w, 0, y, w, rows)
            y += rows
        }
        ownedBitmap
    }
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
 * masks in place to [res]'s leased output buffer, then convert to a bitmap. Mutating that buffer means
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
): Bitmap = res.acquireDataLease().use { lease ->
    val data = lease.data
    gradeBufferToBitmap(
        data,
        res.width,
        res.height,
        res.colorSpace,
        cctfEncoded,
        saturation,
        vibrance,
        gamutCompress,
        localAdjustments,
    )
}

/**
 * The buffer-level core of [simResultToBitmapGraded]: grade [data] IN PLACE and
 * convert to a bitmap. Also the re-grade entry point for [GradeCache] hits —
 * use [GradeCache.Pristine.withScratch], never the retained master.
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
