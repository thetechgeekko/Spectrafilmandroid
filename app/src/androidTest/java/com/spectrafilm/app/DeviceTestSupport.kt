/*
 * Spektrafilm for Android — on-device instrumented test support (PR1, headless). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Shared helpers for the androidTest suite: the two instrumentation contexts, a fresh
 * engine bound to the APP's bundled spektra/ assets, direct-buffer builders, the
 * scan_film parity param set (the exact scan_portra config from
 * tests/test_simulate_e2e.cpp:142-162), and a .spkvec golden reader.
 *
 * PURE-ADDITIVE test-only code — nothing here touches app/src/main or the engine.
 */
package com.spectrafilm.app

import android.content.Context
import android.content.res.AssetManager
import androidx.test.platform.app.InstrumentationRegistry
import com.spectrafilm.engine.CameraParams
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.DirCouplersParams
import com.spectrafilm.engine.FilmRenderingParams
import com.spectrafilm.engine.GlareParams
import com.spectrafilm.engine.GrainParams
import com.spectrafilm.engine.HalationParams
import com.spectrafilm.engine.IoParams
import com.spectrafilm.engine.PrintRenderingParams
import com.spectrafilm.engine.Rgb2Raw
import com.spectrafilm.engine.ScannerParams
import com.spectrafilm.engine.SettingsParams
import com.spectrafilm.engine.SpektraEngine
import com.spectrafilm.engine.SpektraParams
import java.nio.ByteBuffer
import java.nio.ByteOrder

object DeviceTestSupport {

    /** The app-under-test context. Its AssetManager holds the merged engine `spektra/` assets. */
    fun targetCtx(): Context = InstrumentationRegistry.getInstrumentation().targetContext

    /** The instrumentation (test-APK) context. Its assets hold the bundled test vectors. */
    fun testCtx(): Context = InstrumentationRegistry.getInstrumentation().context

    /**
     * A fresh native engine reading the APP APK's bundled `spektra/` profiles+LUTs via the
     * app AssetManager (no on-device extraction). Forces `System.loadLibrary("spektra")`.
     */
    fun newEngine(): SpektraEngine =
        SpektraEngine.fromAssets(targetCtx().applicationContext.assets)

    fun assetExists(am: AssetManager, name: String): Boolean =
        try { am.open(name).use { true } } catch (_: Exception) { false }

    fun assetBytes(am: AssetManager, name: String): ByteArray =
        am.open(name).use { it.readBytes() }

    /** A direct little-/native-endian float32 RGB buffer of [w]x[h], every channel = [v]. */
    fun uniformImage(w: Int, h: Int, v: Float): ByteBuffer {
        val buf = ByteBuffer.allocateDirect(w * h * 3 * 4).order(ByteOrder.nativeOrder())
        val fb = buf.asFloatBuffer()
        for (i in 0 until w * h * 3) fb.put(i, v)
        return buf
    }

    /**
     * The deterministic scan_film parity params (1:1 with tests/test_simulate_e2e.cpp:142-162):
     * scan the negative directly, all stochastic + spatial effects zeroed, DIR couplers on with
     * zero diffusion, sRGB CCTF-encoded output, Hanatos2025 upsampling, 640px preview cap.
     *
     * C++ anchor -> Kotlin field:
     *   exposure_compensation_ev=0  -> camera.exposureCompensationEv
     *   auto_exposure=0             -> camera.autoExposure=false
     *   density_curve_gamma=1       -> filmRender.densityCurveGamma (default)
     *   grain_active=0              -> filmRender.grain.active=false
     *   halation_active=0           -> filmRender.halation.active=false
     *   dir_diffusion_size_um=0     -> filmRender.dirCouplers.diffusionSizeUm=0
     *   dir_couplers_active=1       -> filmRender.dirCouplers.active=true
     *   scanner_unsharp={0,0}       -> scanner.unsharpMask=0f to 0f
     *   glare_active=0              -> filmRender.glare.active=false (+ printRender.glare off)
     *   scan_film=1                 -> io.scanFilm=true
     *   output_color_space=SRGB     -> io.outputColorSpace=SRGB
     *   output_cctf_encoding=1      -> io.outputCctfEncoding=true
     *   rgb_to_raw_method=HANATOS   -> settings.rgbToRawMethod=HANATOS2025
     *   preview_max_size=640        -> settings.previewMaxSize=640
     */
    fun scanParams(
        film: String = "kodak_portra_400",
        print: String = "kodak_portra_endura",
    ): SpektraParams = SpektraParams(
        filmProfile = film,
        printProfile = print,
        filmRender = FilmRenderingParams(
            densityCurveGamma = 1.0f,
            grain = GrainParams(active = false),
            halation = HalationParams(active = false),
            dirCouplers = DirCouplersParams(active = true, diffusionSizeUm = 0.0f),
            glare = GlareParams(active = false),
        ),
        // scanFilm=true skips the print stage, but mirror the oracle intent (glare off).
        printRender = PrintRenderingParams(glare = GlareParams(active = false)),
        camera = CameraParams(exposureCompensationEv = 0.0f, autoExposure = false),
        scanner = ScannerParams(unsharpMask = 0.0f to 0.0f),
        io = IoParams(
            outputColorSpace = ColorSpace.SRGB,
            outputCctfEncoding = true,
            scanFilm = true,
        ),
        settings = SettingsParams(
            rgbToRawMethod = Rgb2Raw.HANATOS2025,
            previewMaxSize = 640,
        ),
    )

    /** Parsed .spkvec golden: dense little-endian float32 array + its shape. */
    class Spkvec(val shape: IntArray, val data: FloatArray) {
        val count: Int get() = data.size
    }

    /**
     * Read a .spkvec container (see tools/parity/spkvec_io.h): 6B magic "SPKVEC", u16 version=1,
     * u8 dtype=1 (f32), u8 ndim, ndim*u32 shape, then count*float32 — all little-endian.
     */
    fun readSpkvec(bytes: ByteArray): Spkvec {
        val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val magic = ByteArray(6)
        bb.get(magic)
        require(String(magic, Charsets.US_ASCII) == "SPKVEC") { "bad spkvec magic" }
        val version = bb.short.toInt() and 0xFFFF
        require(version == 1) { "unsupported spkvec version $version" }
        val dtype = bb.get().toInt() and 0xFF
        require(dtype == 1) { "unsupported spkvec dtype $dtype (expected f32)" }
        val ndim = bb.get().toInt() and 0xFF
        require(ndim in 1..8) { "bad spkvec ndim $ndim" }
        val shape = IntArray(ndim) { bb.int }
        var count = 1
        for (d in shape) count *= d
        val data = FloatArray(count)
        val fb = bb.asFloatBuffer()
        fb.get(data)
        return Spkvec(shape, data)
    }
}
