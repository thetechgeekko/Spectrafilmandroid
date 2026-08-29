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

/**
 * A linear, scene-referred image: interleaved RGB float32, row-major.
 *
 * MEMORY OWNERSHIP — full-resolution buffers live OFF the managed heap. A
 * full-res RAW decode is ~140 MB; allocated as a JVM-managed direct ByteBuffer
 * (`ByteBuffer.allocateDirect`, which on Android is a non-movable `byte[]` on the
 * ART heap) two of those at export time blow the ~256 MB heap-growth limit
 * (OutOfMemoryError). Following Adobe Lightroom — whose native engine keeps all
 * full-res pixels in native memory and never crosses them to the Java heap — the
 * large RAW/engine buffers are allocated natively (`malloc` + `NewDirectByteBuffer`)
 * and reclaimed via [onClose]. Small/proxy buffers stay managed ([onClose] null),
 * so the GC handles them and [close] is a no-op.
 *
 * [close] must be called when an off-heap image is no longer needed (native memory
 * is NOT tracked by the GC). It is idempotent and safe on managed images.
 */
class LinearImage(
    val data: ByteBuffer,        // direct buffer, length = width*height*3*4 bytes
    val width: Int,
    val height: Int,
    val colorSpace: String = "ProPhoto RGB",
    private val onClose: ((ByteBuffer) -> Unit)? = null,
) : AutoCloseable {
    init {
        require(data.isDirect) { "LinearImage requires a direct ByteBuffer" }
        require(width > 0 && height > 0) { "invalid dimensions ${width}x$height" }
    }

    private var closed = false

    /** Free the backing native buffer if this image owns one; no-op for managed buffers. */
    override fun close() {
        if (closed) return
        closed = true
        onClose?.invoke(data)
    }
}

/**
 * Result of a simulation: display-referred RGB in [SpektraParams.io].outputColorSpace.
 *
 * The engine output buffer is allocated in NATIVE memory (`malloc` +
 * `NewDirectByteBuffer`), off the ART managed heap (see [LinearImage]). Call [close]
 * once the result has been consumed (turned into a Bitmap, or written to a file) to
 * free it; it is idempotent.
 */
class SimResult(
    val data: ByteBuffer,
    val width: Int,
    val height: Int,
    val colorSpace: ColorSpace,
) : AutoCloseable {

    private var closed = false

    override fun close() {
        if (closed) return
        closed = true
        freeDirectBuffer(data)
    }

    companion object {
        /**
         * Free a native (`NewDirectByteBuffer`-wrapped `malloc`) engine-output buffer.
         * Implemented in spektra_jni.cpp; libspektra is already loaded by [SpektraEngine].
         */
        @JvmStatic external fun freeDirectBuffer(buf: ByteBuffer)

        /**
         * Allocate an OFF-HEAP direct [ByteBuffer] of [size] bytes (native `malloc` +
         * `NewDirectByteBuffer`), or null on failure. Unlike `ByteBuffer.allocateDirect`
         * (a non-movable `byte[]` on the ~256 MB ART heap on Android), this lives in native
         * memory — use it for large export staging buffers so they don't OOM the managed
         * heap. The caller MUST release it with [freeDirectBuffer]; NEVER pass a managed
         * `allocateDirect` buffer to [freeDirectBuffer].
         */
        @JvmStatic external fun allocDirectBuffer(size: Long): ByteBuffer?
    }
}

class SpektraEngine private constructor(
    handle: Long,
    // Held ONLY to keep the AssetManager alive for the engine's lifetime when the
    // engine was created in AAssetManager mode: the native AAssetManager* obtained
    // via AAssetManager_fromJava is valid only while this Java AssetManager is
    // referenced. Null in filesystem (extracted-dir) mode.
    private val assetManager: AssetManager? = null,
) : AutoCloseable {

    private val handle: Long = handle

    @Volatile private var destroyed = false

    /** Filesystem mode: read bundled assets from an extracted [assetDir] on disk. */
    constructor(assetDir: String? = null) : this(nativeCreate(assetDir), null)

    init {
        require(handle != 0L) { "spektra: engine creation returned a null handle" }
    }

    /** Available film/print profile ids bundled in assets (see docs/ASSETS.md). */
    fun listProfiles(): List<String> =
        nativeListProfiles(handle).split('\n').filter { it.isNotBlank() }

    /**
     * Full pipeline: RGB → negative → (print) → scan. Heavy; call off the main
     * thread. On a native failure the underlying [RuntimeException] (with the
     * specific `spk_status` message) propagates; a null return without an
     * exception is reported as an unexpected fault.
     */
    fun simulate(image: LinearImage, params: SpektraParams): SimResult {
        check(!destroyed) { "spektra: simulate called on a closed engine" }
        return nativeSimulate(handle, image.data, image.width, image.height,
            image.colorSpace, params, /* preview = */ false)
            ?: error("spektra: simulate returned null (handle=$handle)")
    }

    /** Downscaled fast path to [SettingsParams.previewMaxSize] for interactive tuning. */
    fun simulatePreview(image: LinearImage, params: SpektraParams): SimResult {
        check(!destroyed) { "spektra: simulatePreview called on a closed engine" }
        return nativeSimulate(handle, image.data, image.width, image.height,
            image.colorSpace, params, /* preview = */ true)
            ?: error("spektra: simulatePreview returned null (handle=$handle)")
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
    ): String {
        check(!destroyed) { "spektra: bakeCubeLut called on a closed engine" }
        return nativeBakeCubeLut(handle, params, size, shaper)
            ?: error("spektra: bakeCubeLut returned null (handle=$handle)")
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
    fun meterExposureEv(image: LinearImage, params: SpektraParams): Double {
        check(!destroyed) { "spektra: meterExposureEv called on a closed engine" }
        return nativeMeterExposureEv(handle, image.data, image.width, image.height, params)
    }

    /** Linear exposure gain (`2^ev`) for [image] under [params]. See [meterExposureEv]. */
    fun exposureGain(image: LinearImage, params: SpektraParams): Float =
        Math.pow(2.0, meterExposureEv(image, params)).toFloat()

    /** Destroy the native engine. Idempotent — a second call is a no-op (no double free). */
    @Synchronized
    override fun close() {
        if (destroyed || handle == 0L) return
        destroyed = true
        nativeDestroy(handle)
    }

    // --- native bridge (see spektra_jni.cpp) ---
    private external fun nativeDestroy(handle: Long)
    private external fun nativeListProfiles(handle: Long): String
    private external fun nativeSimulate(
        handle: Long, inBuf: ByteBuffer, w: Int, h: Int, inCs: String,
        params: SpektraParams, preview: Boolean,
    ): SimResult?
    private external fun nativeMeterExposureEv(
        handle: Long, inBuf: ByteBuffer, w: Int, h: Int, params: SpektraParams,
    ): Double
    private external fun nativeBakeCubeLut(
        handle: Long, params: SpektraParams, size: Int, shaper: Int,
    ): String?

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

        @JvmStatic private external fun nativeSetBigCores(mode: Int)
        @JvmStatic private external fun nativeBigCoreCount(): Int

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
        fun fromAssets(assetManager: AssetManager): SpektraEngine =
            SpektraEngine(nativeCreateFromAssets(assetManager), assetManager)
    }
}
