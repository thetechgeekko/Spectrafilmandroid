/*
 * Spektrafilm for Android — on-device engine PARITY test (PR1). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * THE key device test. Runs the full scan_film pipeline on the real arm64 engine .so for the
 * deterministic scan_portra fixture and compares the output to the committed golden vector,
 * to the SAME tolerances the host parity gate uses (max_abs <= 1e-4, rms <= 1e-5).
 *
 * "Bit-exact" per CLAUDE.md is within tolerance of the oracle AND thread-invariant, but NOT
 * necessarily byte-identical across CPU architectures (-ffast-math FMA contraction differs by
 * arch). The host gate runs on x86_64; this proves the arm64 build still lands inside tolerance.
 * The measured max_abs/rms are always reported in the assertion message so an arm64 tolerance
 * graze is a visible finding, never a silently loosened bound.
 *
 * Fixtures bundled in app/src/androidTest/assets:
 *   scan_portra_input_rgb.f64   64x64x3 raw float64 (no header) — the engine input
 *   scan_portra_final_rgb.spkvec 64x64x3 float32 golden (22B header + payload)
 */
package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs
import kotlin.math.sqrt
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class EngineParityOnDeviceTest {

    private val W = 64
    private val H = 64
    private val TOL_MAX_ABS = 1e-4
    private val TOL_RMS = 1e-5

    @Test
    fun scanPortra_matchesGoldenWithinTolerance() {
        val testAssets = DeviceTestSupport.testCtx().assets

        // --- input: 64x64x3 float64 (LE, no header) -> float32 direct buffer (native order) ---
        val f64 = DeviceTestSupport.assetBytes(testAssets, "scan_portra_input_rgb.f64")
        val n = W * H * 3
        assertEquals("input fixture byte length", (n * 8).toLong(), f64.size.toLong())
        val db = ByteBuffer.wrap(f64).order(ByteOrder.LITTLE_ENDIAN).asDoubleBuffer()
        val inBuf = ByteBuffer.allocateDirect(n * 4).order(ByteOrder.nativeOrder())
        val inF = inBuf.asFloatBuffer()
        for (i in 0 until n) inF.put(i, db.get(i).toFloat())

        // --- golden: 64x64x3 float32 (.spkvec) ---
        val gold = DeviceTestSupport.readSpkvec(
            DeviceTestSupport.assetBytes(testAssets, "scan_portra_final_rgb.spkvec"),
        )
        assertEquals("golden element count", n, gold.count)

        // --- simulate (FULL, not preview) with the exact scan_portra parity params ---
        val engine = DeviceTestSupport.newEngine()
        var maxAbs = 0.0
        var sse = 0.0
        var argmax = 0
        try {
            LinearImage(inBuf, W, H, "ProPhoto RGB").use { img ->
                engine.simulate(img, DeviceTestSupport.scanParams()).use { res ->
                    assertEquals("output width", W, res.width)
                    assertEquals("output height", H, res.height)
                    val out = res.data.duplicate().order(ByteOrder.nativeOrder())
                    out.rewind()
                    val outF = out.asFloatBuffer()
                    for (i in 0 until n) {
                        val d = abs(outF.get(i).toDouble() - gold.data[i].toDouble())
                        if (d > maxAbs) { maxAbs = d; argmax = i }
                        sse += d * d
                    }
                }
            }
        } finally {
            engine.close()
        }
        val rms = sqrt(sse / n)
        val msg = "arm64 parity vs scan_portra golden: max_abs=%.3e (tol %.0e) rms=%.3e (tol %.0e) worst idx=%d"
            .format(maxAbs, TOL_MAX_ABS, rms, TOL_RMS, argmax)
        assertTrue(msg, maxAbs <= TOL_MAX_ABS && rms <= TOL_RMS)
    }
}
