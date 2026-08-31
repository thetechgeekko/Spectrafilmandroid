/*
 * Spektrafilm for Android — engine facade.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Film modeling powered by spektrafilm (GPLv3).
 *
 * Kotlin entry point to the native engine (libspektra.so). Mirrors spektrafilm's
 * simulate(image, params) / simulate_preview(image, params). Image buffers are passed
 * as linear, scene-referred float RGB and returned as a display-referred result.
 *
 * The native methods throw a [RuntimeException] carrying the specific
 * `spk_status` message (e.g. "spektra: profile not found") on failure, so a real,
 * actionable error reaches the caller. A null return without an exception is
 * treated as an unexpected engine fault.
 */
package com.spectrafilm.engine

import android.content.res.AssetManager
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.locks.ReentrantReadWriteLock

/** Logical app purpose carried into native timing/Perfetto records. */
enum class RenderKind(internal val nativeCode: Int) {
    EXPORT(1),
    PREVIEW(2),
    MAGNIFIER(3),
    ROI(4),
    EXACT_RENDER(5),
}

/** App-level disposition of native work; distinct from native success/error. */
enum class AppRenderOutcome(internal val nativeCode: Int) {
    CONSUMED(1),
    CANCELLED(2),
    SUPERSEDED(3),
    FAILED(4),
}

/** Thread-safe cooperative cancellation signal for a native engine call. */
class RenderCancellation {
    @Volatile private var requested = false
    @Volatile internal var testPollObserver: (() -> Unit)? = null

    val isCancellationRequested: Boolean
        get() {
            // Instrumentation-only synchronization point. Production tokens keep
            // this null, paying only one predictable nullable read per bounded
            // native poll. JNI invokes the getter only on the render owner thread.
            testPollObserver?.invoke()
            return requested
        }

    fun cancel() {
        requested = true
    }
}

/** Explicit lifetime authority for a direct-buffer view. */
class DataLease internal constructor(
    val data: ByteBuffer,
    private val releaseLease: () -> Unit,
) : AutoCloseable {
    private val closed = AtomicBoolean(false)

    override fun close() {
        if (closed.compareAndSet(false, true)) releaseLease()
    }
}

/** JNI-only result of a manual allocation; its token is required to release it. */
internal class NativeBufferAllocation(
    val data: ByteBuffer,
    val token: Long,
) {
    init {
        require(token > 0L) { "native allocation token must be positive" }
    }
}

/**
 * A linear, scene-referred image: packed interleaved RGB float32, row-major.
 * Rows are contiguous (`rowStrideBytes == width * 3 * 4`); padded/strided input
 * is not part of this API. The constructor buffer's [ByteBuffer.position] and
 * limit are captured as the immutable logical sample window; each lease gets an
 * independent view and JNI validates its remaining range before every native read.
 *
 * MEMORY OWNERSHIP — full-resolution buffers live OFF the managed heap. A
 * full-res RAW decode is ~140 MB; allocated as a JVM-managed direct ByteBuffer
 * (`ByteBuffer.allocateDirect`, which on Android is a non-movable `byte[]` on the
 * ART heap) two of those at export time blow the ~256 MB heap-growth limit
 * (OutOfMemoryError). Following Adobe Lightroom — whose native engine keeps all
 * full-res pixels in native memory and never crosses them to the Java heap — the
 * large RAW/engine buffers are allocated natively (`malloc` + `NewDirectByteBuffer`)
 * and reclaimed through their owning [DataLease]. Small/proxy buffers stay managed,
 * so the GC handles them and [close] is a no-op.
 *
 * [close] must be called when an off-heap image is no longer needed (native memory
 * is NOT tracked by the GC). It is idempotent and safe on managed images.
 */
class LinearImage private constructor(
    data: ByteBuffer,            // direct buffer, length = width*height*3*4 bytes
    val width: Int,
    val height: Int,
    val colorSpace: String = "ProPhoto RGB",
    private val release: () -> Unit = {},
) : AutoCloseable {
    // Capture the caller's logical sample window once. A duplicate keeps later
    // position/limit mutations on the constructor argument from changing what
    // JNI reads, while retaining the same direct native allocation.
    private val backingData = data.duplicate().order(data.order())

    init {
        require(backingData.isDirect) { "LinearImage requires a direct ByteBuffer" }
        require(backingData.order() == ByteOrder.nativeOrder()) {
            "LinearImage ByteBuffer must use native byte order"
        }
        require(width > 0 && height > 0) { "invalid dimensions ${width}x$height" }
    }

    private val lifecycle = AtomicInteger(0)

    /** Acquire a lease. Close it when the direct-buffer view is no longer in use. */
    fun acquireDataLease(): DataLease {
        while (true) {
            val state = lifecycle.get()
            check(state >= 0) { "LinearImage is closed" }
            check(state != Int.MAX_VALUE) { "LinearImage has too many active leases" }
            if (lifecycle.compareAndSet(state, state + 1)) break
        }
        return DataLease(
            backingData.duplicate().order(ByteOrder.nativeOrder()),
            ::releaseDataLease,
        )
    }

    private fun releaseDataLease() {
        if (lifecycle.decrementAndGet() == closedMarker) release()
    }

    /** Free the backing native buffer if this image owns one; no-op for managed buffers. */
    override fun close() {
        while (true) {
            val state = lifecycle.get()
            if (state < 0) return
            if (lifecycle.compareAndSet(state, state or closedMarker)) {
                if (state == 0) release()
                return
            }
        }
    }

    private val closedMarker = Int.MIN_VALUE

    /** Construct a managed direct-buffer image. */
    constructor(
        data: ByteBuffer,
        width: Int,
        height: Int,
        colorSpace: String = "ProPhoto RGB",
    ) : this(data, width, height, colorSpace, {})

    companion object {

        /**
         * Transfer an already-acquired external lease to the returned image.
         * If validation fails, the transfer is rolled back by closing [lease].
         */
        fun fromDataLease(
            data: ByteBuffer,
            width: Int,
            height: Int,
            colorSpace: String = "ProPhoto RGB",
            lease: AutoCloseable,
        ): LinearImage = try {
            LinearImage(data, width, height, colorSpace) { lease.close() }
        } catch (failure: Throwable) {
            try {
                lease.close()
            } catch (releaseFailure: Throwable) {
                if (releaseFailure !== failure) failure.addSuppressed(releaseFailure)
            }
            throw failure
        }

        internal fun forTest(
            data: ByteBuffer,
            width: Int,
            height: Int,
            colorSpace: String = "ProPhoto RGB",
            release: () -> Unit,
        ): LinearImage = LinearImage(data, width, height, colorSpace, release)
    }
}

/**
 * Opaque owner for a manual native direct allocation. Callers can acquire a
 * [DataLease] or transfer ownership to [LinearImage], but never receive the
 * allocation's free capability or its backing buffer directly.
 */
class NativeBufferOwner private constructor(
    data: ByteBuffer,
    private val token: Long,
    private val release: (ByteBuffer, Long) -> Unit,
) : AutoCloseable {
    // JNI NewDirectByteBuffer does not tag its Java view with native byte order.
    // This owner is the native allocation boundary, so normalize its private view;
    // caller-owned LinearImage buffers remain strict and are never normalized here.
    private val backingData = data.duplicate().order(ByteOrder.nativeOrder())
    private val lifecycle = AtomicInteger(0)

    init {
        require(token > 0L) { "NativeBufferOwner token must be positive" }
        require(backingData.isDirect) { "NativeBufferOwner requires a direct ByteBuffer" }
        require(backingData.order() == ByteOrder.nativeOrder()) {
            "NativeBufferOwner ByteBuffer must use native byte order"
        }
    }

    fun acquireDataLease(): DataLease {
        while (true) {
            val state = lifecycle.get()
            check(state >= 0) { "NativeBufferOwner is closed" }
            check(state != Int.MAX_VALUE) { "NativeBufferOwner has too many active leases" }
            if (lifecycle.compareAndSet(state, state + 1)) break
        }
        return DataLease(
            backingData.duplicate().order(ByteOrder.nativeOrder()),
            ::releaseDataLease,
        )
    }

    fun transferToLinearImage(
        width: Int,
        height: Int,
        colorSpace: String = "ProPhoto RGB",
    ): LinearImage {
        val lease = acquireDataLease()
        return try {
            LinearImage.fromDataLease(lease.data, width, height, colorSpace, lease)
        } finally {
            // The returned image owns the active lease. Closing this owner prevents
            // a forgotten second close from retaining an extra allocation authority.
            close()
        }
    }

    private fun releaseDataLease() {
        if (lifecycle.decrementAndGet() == CLOSED) release(backingData, token)
    }

    override fun close() {
        while (true) {
            val state = lifecycle.get()
            if (state < 0) return
            if (lifecycle.compareAndSet(state, state or CLOSED)) {
                if (state == 0) release(backingData, token)
                return
            }
        }
    }

    companion object {
        private const val CLOSED = Int.MIN_VALUE

        /** Allocate native memory, returning an opaque owner or null on OOM. */
        fun allocate(size: Long): NativeBufferOwner? =
            SimResult.allocateNativeBuffer(size)?.let(::fromNative)

        private fun fromNative(allocation: NativeBufferAllocation): NativeBufferOwner = try {
            NativeBufferOwner(allocation.data, allocation.token, SimResult::releaseNativeBuffer)
        } catch (failure: Throwable) {
            try {
                SimResult.releaseNativeBuffer(allocation.data, allocation.token)
            } catch (releaseFailure: Throwable) {
                if (releaseFailure !== failure) failure.addSuppressed(releaseFailure)
            }
            throw failure
        }

        internal fun forTest(data: ByteBuffer, release: (ByteBuffer) -> Unit): NativeBufferOwner =
            try {
                NativeBufferOwner(data, 1L) { buffer, _ -> release(buffer) }
            } catch (failure: Throwable) {
                try {
                    release(data)
                } catch (releaseFailure: Throwable) {
                    if (releaseFailure !== failure) failure.addSuppressed(releaseFailure)
                }
                throw failure
            }
    }
}

/**
 * Result of a simulation: display-referred RGB in [SpektraParams.io].outputColorSpace.
 * Its native bytes are released by [close], after every active [DataLease] drains.
 */
class SimResult private constructor(
    data: ByteBuffer,
    val width: Int,
    val height: Int,
    val colorSpace: ColorSpace,
    /** Correlates this result with `spk.stage_timings.v1` and Perfetto. */
    val renderId: Long,
    private val allocationToken: Long,
    private val release: (ByteBuffer, Long) -> Unit,
) : AutoCloseable {
    // NewDirectByteBuffer exposes BIG_ENDIAN metadata by default even when the
    // underlying native floats use the device's native order. Keep an isolated,
    // normalized view while preserving the exact allocation base for release.
    private val backingData = data.duplicate().order(ByteOrder.nativeOrder())

    internal constructor(
        data: ByteBuffer,
        width: Int,
        height: Int,
        colorSpace: ColorSpace,
        renderId: Long,
        allocationToken: Long,
    ) : this(data, width, height, colorSpace, renderId, allocationToken, ::freeDirectBuffer)

    init {
        try {
            require(allocationToken > 0L) { "SimResult allocation token must be positive" }
            require(backingData.isDirect) { "SimResult requires a direct ByteBuffer" }
            require(backingData.order() == ByteOrder.nativeOrder()) {
                "SimResult ByteBuffer must use native byte order"
            }
            require(width > 0 && height > 0) { "invalid dimensions ${width}x$height" }
        } catch (failure: Throwable) {
            try {
                release(backingData, allocationToken)
            } catch (releaseFailure: Throwable) {
                if (releaseFailure !== failure) failure.addSuppressed(releaseFailure)
            }
            throw failure
        }
    }

    private val lifecycle = AtomicInteger(0)

    /**
     * Read the native result while holding a lifetime lease. The buffer handed to
     * [block] is a per-reader duplicate in native byte order, so position/limit
     * mutations cannot race other readers. [close] may run concurrently, but the
     * native allocation is not released until the last active reader returns.
     */
    /** Acquire a lease. Close it when the result bytes are no longer in use. */
    fun acquireDataLease(): DataLease {
        while (true) {
            val state = lifecycle.get()
            check(state >= 0) { "SimResult is closed" }
            check(state != Int.MAX_VALUE) { "SimResult has too many active leases" }
            if (lifecycle.compareAndSet(state, state + 1)) break
        }
        return DataLease(
            backingData.duplicate().order(ByteOrder.nativeOrder()),
            ::releaseDataLease,
        )
    }

    private fun releaseDataLease() {
        if (lifecycle.decrementAndGet() == CLOSED) release(backingData, allocationToken)
    }

    override fun close() {
        while (true) {
            val state = lifecycle.get()
            if (state < 0) return
            if (lifecycle.compareAndSet(state, state or CLOSED)) {
                if (state == 0) release(backingData, allocationToken)
                return
            }
        }
    }

    /** Report the app's final disposition; buffer cleanup alone is not consumption. */
    fun reportOutcome(outcome: AppRenderOutcome) {
        Companion.reportOutcome(renderId, outcome)
    }

    companion object {
        private const val CLOSED = Int.MIN_VALUE

        internal fun forTest(
            data: ByteBuffer,
            width: Int,
            height: Int,
            colorSpace: ColorSpace,
            renderId: Long,
            release: (ByteBuffer) -> Unit,
        ): SimResult = SimResult(
            data, width, height, colorSpace, renderId, 1L,
        ) { buffer, _ -> release(buffer) }

        /**
         * Free a native (`NewDirectByteBuffer`-wrapped `malloc`) engine-output buffer.
         * Implemented in spektra_jni.cpp; libspektra is already loaded by [SpektraEngine].
         */
        @JvmStatic private external fun freeDirectBuffer(buf: ByteBuffer, token: Long)

        /**
         * Allocate an OFF-HEAP direct [ByteBuffer] of [size] bytes (native `malloc` +
         * `NewDirectByteBuffer`), or null on failure. Unlike `ByteBuffer.allocateDirect`
         * (a non-movable `byte[]` on the ~256 MB ART heap on Android), this lives in native
         * memory — use it for large export staging buffers so they don't OOM the managed
         * heap. It is immediately wrapped by [NativeBufferOwner], which holds the only
         * release authority.
         */
        @JvmStatic private external fun allocDirectBuffer(size: Long): NativeBufferAllocation?

        internal fun allocateNativeBuffer(size: Long): NativeBufferAllocation? = allocDirectBuffer(size)
        internal fun releaseNativeBuffer(buffer: ByteBuffer, token: Long) =
            freeDirectBuffer(buffer, token)

        /**
         * Emit a keyed app-disposition update for benchmark consumers. Native
         * success does not imply publication: superseded coroutine work may be
         * fully rendered and then deliberately discarded.
         */
        @JvmStatic fun reportOutcome(renderId: Long, outcome: AppRenderOutcome) {
            if (renderId != 0L) nativeReportRenderOutcome(renderId, outcome.nativeCode)
        }

        @JvmStatic private external fun nativeReportRenderOutcome(
            renderId: Long,
            outcome: Int,
        )
    }
}

/** A baked LUT plus the render id used by timing/Perfetto and app disposition. */
data class LutBakeResult(
    val text: String,
    val renderId: Long,
) {
    fun reportOutcome(outcome: AppRenderOutcome) {
        SimResult.reportOutcome(renderId, outcome)
    }
}

internal class EngineHandleLease(initialHandle: Long) {
    private var handle = initialHandle
    private val lock = ReentrantReadWriteLock()

    /** Publish a newly created handle before the owning engine becomes observable. */
    fun initialize(createdHandle: Long) {
        require(createdHandle != 0L) { "spektra: engine creation returned a null handle" }
        check(handle == 0L) { "spektra: engine handle is already initialized" }
        handle = createdHandle
    }

    fun <T> withLease(operation: String, block: (Long) -> T): T {
        val readLock = lock.readLock()
        readLock.lock()
        try {
            val leasedHandle = handle
            check(leasedHandle != 0L) {
                "spektra: $operation called on a closed engine"
            }
            return block(leasedHandle)
        } finally {
            readLock.unlock()
        }
    }

    fun close(destroy: (Long) -> Unit) {
        val writeLock = lock.writeLock()
        writeLock.lock()
        try {
            val doomed = handle
            if (doomed == 0L) return
            handle = 0L
            destroy(doomed)
        } finally {
            writeLock.unlock()
        }
    }

    /** Deterministic test seam: true only after [thread] is queued for this lease lock. */
    internal fun isQueuedForTest(thread: Thread): Boolean = lock.hasQueuedThread(thread)
}

class SpektraEngine private constructor(
    // Held ONLY to keep the AssetManager alive for the engine's lifetime when the
    // engine was created in AAssetManager mode: the native AAssetManager* obtained
    // via AAssetManager_fromJava is valid only while this Java AssetManager is
    // referenced. Null in filesystem (extracted-dir) mode.
    private val assetManager: AssetManager? = null,
) : AutoCloseable {

    // Construct the lock/owner before nativeCreate*. A successful raw native handle
    // is therefore never waiting on a later Kotlin owner construction that can fail.
    private val lifecycle = EngineHandleLease(0L)

    /** Filesystem mode: read bundled assets from an extracted [assetDir] on disk. */
    constructor(assetDir: String? = null) : this(assetManager = null) {
        lifecycle.initialize(nativeCreate(assetDir))
    }

    /** Available film/print profile ids bundled in assets (see docs/ASSETS.md). */
    fun listProfiles(): List<String> = withEngineLease("listProfiles") { leasedHandle ->
        nativeListProfiles(leasedHandle).split('\n').filter { it.isNotBlank() }
    }

    /**
     * Full pipeline: RGB → negative → (print) → scan. Heavy; call off the main
     * thread. On a native failure the underlying [RuntimeException] (with the
     * specific `spk_status` message) propagates; a null return without an
     * exception is reported as an unexpected fault.
     */
    fun simulate(
        image: LinearImage,
        params: SpektraParams,
        kind: RenderKind = RenderKind.EXACT_RENDER,
        cancellation: RenderCancellation? = null,
    ): SimResult = withEngineLease("simulate") { leasedHandle ->
        image.acquireDataLease().use { lease ->
            val data = lease.data
            nativeSimulate(leasedHandle, data, image.width, image.height,
                image.colorSpace, params, /* preview = */ false, kind.nativeCode,
                cancellation)
                ?: error("spektra: simulate returned null (handle=$leasedHandle)")
        }
    }

    /** Downscaled fast path to [SettingsParams.previewMaxSize] for interactive tuning. */
    fun simulatePreview(
        image: LinearImage,
        params: SpektraParams,
        kind: RenderKind = RenderKind.PREVIEW,
        cancellation: RenderCancellation? = null,
    ): SimResult = withEngineLease("simulatePreview") { leasedHandle ->
        image.acquireDataLease().use { lease ->
            val data = lease.data
            nativeSimulate(leasedHandle, data, image.width, image.height,
                image.colorSpace, params, /* preview = */ true, kind.nativeCode,
                cancellation)
                ?: error("spektra: simulatePreview returned null (handle=$leasedHandle)")
        }
    }

    /**
     * Bake the current film look into a 3D `.cube` LUT (Adobe/Resolve format) and
     * return its text. Builds an identity RGB lattice of [size] (default 33) in the
     * engine's linear ProPhoto working space, runs each lattice point through the
     * same pointwise pipeline [simulate] uses, and emits `LUT_3D_SIZE N` + N^3 RGB
     * triples (blue-fastest order).
     *
     * INPUT domain: linear ProPhoto RGB in [0,1]. OUTPUT domain: display RGB in
     * [params].io.outputColorSpace (CCTF per outputCctfEncoding). Spatial/stochastic
     * effects (grain, halation, diffusion glare, DIR-coupler diffusion, scanner
     * unsharp) cannot be captured by a 3D LUT and are forced OFF for the bake;
     * this is documented in the emitted `.cube` header. Heavy; call off the main thread.
     *
     * AUTO-EXPOSURE IS OFF AND THE CALLER OWNS THE GAIN. AE meters a whole image to
     * derive one global gain; the bake's input is a synthetic lattice, not an image,
     * so no meaningful gain exists at bake time and the LUT is emitted at UNITY gain.
     * Anything feeding real pixels through this LUT — the GPU preview, the camera
     * viewfinder — MUST scale them by the exposure gain first, or the result is dark
     * with lifted shadows (the scene sits in the film curve's toe). This differs from
     * [simulate], where the engine's own auto-exposure runs on the real image.
     */
    fun bakeCubeLut(
        params: SpektraParams,
        size: Int = 33,
        shaper: Int = SHAPER_NONE,
        cancellation: RenderCancellation? = null,
    ): LutBakeResult = withEngineLease("bakeCubeLut") { leasedHandle ->
        nativeBakeCubeLut(leasedHandle, params, size, shaper, cancellation)
            ?: error("spektra: bakeCubeLut returned null (handle=$leasedHandle)")
    }

    /**
     * Meter [image] and return the auto-exposure compensation in **EV** that
     * [simulate] would apply to it under [params] — without rendering. The linear
     * gain is `2^ev`; [exposureGain] wraps that. Returns 0.0 (unity gain) when
     * `camera.autoExposure` is off.
     *
     * This is the companion to [bakeCubeLut]. A baked 3D LUT carries no exposure
     * (see that method), so a caller applying one to real pixels must scale them
     * itself — and it must be the SAME gain the engine would use, or the preview
     * misreports brightness. Internally this runs the identical metering function
     * the render calls, so the two cannot drift apart.
     *
     * Metering always runs on a max-256 downscale internally, so a proxy meters
     * near-identically to the full-resolution original of the same scene. Cheap
     * relative to a render, but still off the main thread.
     */
    fun meterExposureEv(
        image: LinearImage,
        params: SpektraParams,
        cancellation: RenderCancellation? = null,
    ): Double = withEngineLease("meterExposureEv") { leasedHandle ->
        image.acquireDataLease().use { lease ->
            val data = lease.data
            nativeMeterExposureEv(
                leasedHandle, data, image.width, image.height, image.colorSpace,
                params, cancellation,
            )
        }
    }

    /** Linear exposure gain (`2^ev`) for [image] under [params]. See [meterExposureEv]. */
    fun exposureGain(
        image: LinearImage,
        params: SpektraParams,
        cancellation: RenderCancellation? = null,
    ): Float = Math.pow(2.0, meterExposureEv(image, params, cancellation)).toFloat()

    /** Destroy the native engine. Idempotent — a second call is a no-op (no double free). */
    override fun close() {
        lifecycle.close(::nativeDestroy)
    }

    private fun <T> withEngineLease(operation: String, block: (Long) -> T): T =
        lifecycle.withLease(operation, block)

    /** Instrumentation seam for proving close reached and blocked on an active render lease. */
    internal fun isLifecycleCloseQueuedForTest(thread: Thread): Boolean =
        lifecycle.isQueuedForTest(thread)

    // --- native bridge (see spektra_jni.cpp) ---
    private external fun nativeDestroy(handle: Long)
    private external fun nativeListProfiles(handle: Long): String
    private external fun nativeSimulate(
        handle: Long, inBuf: ByteBuffer, w: Int, h: Int, inCs: String,
        params: SpektraParams, preview: Boolean, renderKind: Int,
        cancellation: RenderCancellation?,
    ): SimResult?
    private external fun nativeMeterExposureEv(
        handle: Long, inBuf: ByteBuffer, w: Int, h: Int, inCs: String,
        params: SpektraParams, cancellation: RenderCancellation?,
    ): Double
    private external fun nativeBakeCubeLut(
        handle: Long, params: SpektraParams, size: Int, shaper: Int,
        cancellation: RenderCancellation?,
    ): LutBakeResult?

    companion object {
        init { System.loadLibrary("spektra") }

        // Engine constructors. Both are JNI instance-less (the C++ side ignores the
        // receiver); kept @JvmStatic so the secondary constructors can call them
        // before `this` exists.
        /**
         * Lattice spacing for [bakeCubeLut].
         *
         * [SHAPER_NONE] spaces entries evenly in LINEAR light — what a `.cube` file
         * advertises, so the only correct choice for a LUT handed to other software. It is
         * also badly lopsided: at size 17, with the engine's 0.184 midgray, barely three
         * entries fall below mid-grey, leaving the film curve's toe as a few straight lines.
         *
         * [SHAPER_SRGB] spaces entries evenly in sRGB-encoded space instead — roughly eight
         * below mid-grey at the same size. The caller MUST apply the same transfer to its
         * pixels before the lookup, which is why it is only for our own GPU preview.
         */
        const val SHAPER_NONE = 0
        const val SHAPER_SRGB = 1

        /**
         * Pin the render pool to the device's performance cores.
         *
         * A fork-join is only as fast as its slowest chunk, so one worker parked on an
         * efficiency core sets the pace for the whole map however many big cores sit
         * idle. Measured 1.51x on the default-ON spatial path (SM-S948W, two 4.74 GHz
         * prime cores beating all eight) with the output checksum unchanged.
         *
         * Output is unaffected by construction: this changes only WHERE a chunk runs
         * and how many workers split it, and every worker count is byte-identical —
         * the thread-invariance the parity gate already asserts.
         *
         * @param mode 1 = on, 0 = off, -1 = defer to the `SPK_BIG_CORES` env var.
         * Safe to call between renders, including mid-session.
         */
        @JvmStatic fun setBigCores(mode: Int) = nativeSetBigCores(mode)

        /**
         * Cores currently classified as "big", or 0 when pinning is off, detection
         * failed, or the mask would cover every core (pinning to all cores is not
         * pinning). Use it to report whether the setting did anything on this device.
         */
        @JvmStatic fun bigCoreCount(): Int = nativeBigCoreCount()

        /** Instrumentation-only observation of the real Kotlin -> JNI marshaller. */
        @JvmStatic
        internal fun debugMarshalledParams(params: Any?): String =
            nativeDebugMarshalledParams(params)

        @JvmStatic private external fun nativeSetBigCores(mode: Int)
        @JvmStatic private external fun nativeBigCoreCount(): Int
        @JvmStatic private external fun nativeDebugMarshalledParams(params: Any?): String

        @JvmStatic private external fun nativeCreate(assetDir: String?): Long
        @JvmStatic private external fun nativeCreateFromAssets(assetManager: AssetManager): Long

        /**
         * Create an engine that reads bundled assets directly from the APK via the
         * app's [AssetManager] — NO on-device extraction of the ~17 MB spektra/ tree.
         * Pass `context.applicationContext.assets` so the AssetManager (and thus the
         * native `AAssetManager*`) stays alive for the app's lifetime. The returned
         * engine retains [assetManager] to guarantee that.
         *
         * Prefer [createFromAssetsOrExtract] unless you specifically want to fail
         * (rather than fall back) when the AAssetManager path is unavailable.
         */
        @JvmStatic
        fun fromAssets(assetManager: AssetManager): SpektraEngine {
            val engine = SpektraEngine(assetManager)
            engine.lifecycle.initialize(nativeCreateFromAssets(assetManager))
            return engine
        }
    }
}
