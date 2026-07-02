/*
 * Spektrafilm for Android — on-device RAW/DNG decode test (PR1). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Exercises the real libsfraw (LibRaw) decode path end-to-end into the engine. No DNG ships in
 * the repo, so both tests are Assume-guarded: the suite RUNS today (they are skipped, not failed)
 * and the user drops fixtures into app/src/androidTest/assets to turn them on:
 *
 *   sample.dng  — any DECODABLE RAW/DNG (uncompressed, lossless-JPEG/LJ92, or DEFLATE DNG, or a
 *                 mainstream camera RAW). Asserts the decode buffer invariant (w*h*3*4 bytes),
 *                 finite samples, and that the decoded linear image feeds the engine.
 *   expert.dng  — a Samsung Expert RAW (lossy-baseline-JPEG or JPEG-XL DNG) which LibRaw cannot
 *                 unpack. Asserts a RawDecodeException carrying DecodeStatus.LOSSY_JPEG_DNG or
 *                 JPEGXL_DNG (RawDecoder.kt:297/304) — the typed signal the app uses to fall back
 *                 to the platform ImageDecoder.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.LinearImage
import com.spectrafilm.libraw.DecodeStatus
import com.spectrafilm.libraw.RawDecodeException
import com.spectrafilm.libraw.RawDecoder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class RawDecodeTest {

    /** A decodable RAW/DNG: decode → invariant + finiteness → hand to the engine. */
    @Test
    fun sampleDng_decodesFinite_andSimulates() {
        val am = DeviceTestSupport.testCtx().assets
        assumeTrue("drop app/src/androidTest/assets/sample.dng to enable", DeviceTestSupport.assetExists(am, "sample.dng"))

        val bytes = DeviceTestSupport.assetBytes(am, "sample.dng")
        // Bound the decode so a large Expert-RAW sample cannot OOM the device test process.
        val r = RawDecoder.decodeToLinear(bytes, RawDecoder.Settings(maxLongEdge = 2048))

        val expected = r.width.toLong() * r.height.toLong() * 3L * 4L
        assertEquals("decode buffer size == w*h*3*4", expected, r.data.remaining().toLong())
        assertTrue("positive dimensions", r.width > 0 && r.height > 0)

        // Spot-check finiteness on a stride (full scan is unnecessary and slow for big RAWs).
        val fb = r.data.duplicate().also { it.rewind() }.asFloatBuffer()
        val n = r.width * r.height * 3
        var i = 0
        val stride = maxOf(1, n / 4096)
        while (i < n) {
            val v = fb.get(i)
            assertTrue("sample $i is finite ($v)", v.isFinite())
            i += stride
        }

        // The decoded linear ProPhoto image must drive the engine.
        val engine = DeviceTestSupport.newEngine()
        try {
            LinearImage(r.data, r.width, r.height, r.colorSpace).use { img ->
                engine.simulate(img, DeviceTestSupport.scanParams()).use { res ->
                    assertEquals(r.width, res.width)
                    assertEquals(r.height, res.height)
                }
            }
        } finally {
            engine.close()
        }
    }

    /** A compressed Expert-RAW DNG LibRaw can't unpack must surface the typed fallback status. */
    @Test
    fun expertDng_throwsTypedFallbackStatus() {
        val am = DeviceTestSupport.testCtx().assets
        assumeTrue("drop app/src/androidTest/assets/expert.dng to enable", DeviceTestSupport.assetExists(am, "expert.dng"))

        val bytes = DeviceTestSupport.assetBytes(am, "expert.dng")
        try {
            RawDecoder.decodeToLinear(bytes)
            fail("expected RawDecodeException for a lossy-JPEG / JPEG-XL Expert RAW DNG")
        } catch (e: RawDecodeException) {
            assertTrue(
                "status should be the ImageDecoder-fallback signal, was ${e.status}",
                e.status == DecodeStatus.LOSSY_JPEG_DNG || e.status == DecodeStatus.JPEGXL_DNG,
            )
        }
    }
}
