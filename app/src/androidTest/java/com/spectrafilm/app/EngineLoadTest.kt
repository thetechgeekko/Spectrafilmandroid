/*
 * Spektrafilm for Android — on-device native-library load smoke test (PR1). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Proves all four JNI libraries load and their boundary calls resolve on the real arm64
 * device ABI: libspektra (engine), libsfraw (RAW), libsfpng (PNG writer), libsftiff
 * (TIFF writer). A missing/mis-ABI .so surfaces here as an UnsatisfiedLinkError and fails
 * the test loudly — the whole point of a device suite the JVM tests can't cover.
 */
package com.spectrafilm.app

import com.spectrafilm.libraw.RawDecoder
import com.spectrafilm.pngwriter.PngWriter
import com.spectrafilm.tiffwriter.TiffWriter
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class EngineLoadTest {

    /** libspektra: fromAssets() forces System.loadLibrary("spektra") and creates a native handle. */
    @Test
    fun engine_libraryLoadsAndCreates() {
        DeviceTestSupport.newEngine().use { eng ->
            assertNotNull("engine handle should be non-null", eng)
            // listProfiles crosses the JNI boundary → proves the .so's symbols resolved.
            assertTrue("engine should expose bundled profiles", eng.listProfiles().isNotEmpty())
        }
    }

    /** libsfraw: touching RawDecoder runs its init { System.loadLibrary("sfraw") }. */
    @Test
    fun raw_libraryLoads_viaIsRawFile() {
        assertTrue("dng is a RAW type", RawDecoder.isRawFile("dng"))
        assertTrue("leading-dot extension accepted", RawDecoder.isRawFile(".NEF"))
        assertFalse("txt is not a RAW type", RawDecoder.isRawFile("txt"))
    }

    /** libsfpng: a 1x1 16-bit PNG write must produce a non-empty file. */
    @Test
    fun png_libraryLoads_writes1x1() {
        val out = File(DeviceTestSupport.targetCtx().cacheDir, "load_test_1x1.png")
        val rgb16 = ByteBuffer.allocateDirect(1 * 1 * 3 * 2).order(ByteOrder.LITTLE_ENDIAN)
        rgb16.asShortBuffer().put(shortArrayOf(0, 32768.toShort(), 65535.toShort()))
        val n = PngWriter.write(rgb16, 1, 1, out.absolutePath)
        assertTrue("png write returned bytes", n > 0)
        assertTrue("png file exists", out.isFile && out.length() > 0)
        out.delete()
    }

    /** libsftiff: a 1x1 16-bit TIFF write must produce a non-empty file. */
    @Test
    fun tiff_libraryLoads_writes1x1() {
        val out = File(DeviceTestSupport.targetCtx().cacheDir, "load_test_1x1.tif")
        val rgb16 = ByteBuffer.allocateDirect(1 * 1 * 3 * 2).order(ByteOrder.LITTLE_ENDIAN)
        rgb16.asShortBuffer().put(shortArrayOf(100, 200, 300))
        val n = TiffWriter.write(rgb16, 1, 1, out.absolutePath)
        assertTrue("tiff write returned bytes", n > 0)
        assertTrue("tiff file exists", out.isFile && out.length() > 0)
        out.delete()
    }
}
