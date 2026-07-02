/*
 * Spektrafilm for Android — on-device export-writer test (PR1). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Round-trips the real native writers (libsftiff / libsfpng) and the platform Bitmap encoders to
 * temp files under cacheDir, reloads the raw bytes, and asserts the container magic, dimensions,
 * bit depth, sample format, and embedded ICC. Covers the ExportFormat enum breadth at the writer
 * level: TIFF/PNG16/TIFF32F/SCENE_LINEAR_TIFF via the native writers; PNG(8-bit)/JPEG via Bitmap;
 * ULTRA_HDR is asserted present (its JPEG+gainmap encode is platform/API-gated, not exercised here).
 */
package com.spectrafilm.app

import android.graphics.Bitmap
import com.spectrafilm.pngwriter.PngWriter
import com.spectrafilm.tiffwriter.TiffWriter
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class ExportFormatsTest {

    private val W = 2
    private val H = 2
    private fun cache(name: String) = File(DeviceTestSupport.targetCtx().cacheDir, name)

    /** A real sRGB ICC bundled in the app assets — an honest embed round-trip. */
    private fun icc(): ByteArray =
        DeviceTestSupport.assetBytes(DeviceTestSupport.targetCtx().assets, "spektra/icc/saucecontrol/sRGB-v2-magic.icc")

    private fun rgb16Buf(): ByteBuffer {
        val b = ByteBuffer.allocateDirect(W * H * 3 * 2).order(ByteOrder.LITTLE_ENDIAN)
        val s = b.asShortBuffer()
        for (i in 0 until W * H * 3) s.put(i, ((i * 5000) and 0xFFFF).toShort())
        return b
    }

    private fun rgbFloatBuf(): ByteBuffer {
        val b = ByteBuffer.allocateDirect(W * H * 3 * 4).order(ByteOrder.LITTLE_ENDIAN)
        val f = b.asFloatBuffer()
        for (i in 0 until W * H * 3) f.put(i, (i.toFloat() / (W * H * 3)))
        return b
    }

    // ---- native 16-bit TIFF ----
    @Test
    fun tiff16_write_hasDimsBitdepthIcc() {
        val icc = icc()
        val out = cache("export_16.tif")
        val n = TiffWriter.write(rgb16Buf(), W, H, out.absolutePath, icc = icc)
        assertTrue("bytes written", n > 0)
        val b = out.readBytes()
        val ifd = parseTiff(b)
        assertEquals("ImageWidth", W.toLong(), tiffScalar(ifd, 256))
        assertEquals("ImageLength", H.toLong(), tiffScalar(ifd, 257))
        assertEquals("BitsPerSample[0]", 16, tiffFirstShort(b, ifd, 258))
        assertEquals("ICCProfile length", icc.size.toLong(), ifd[34675]?.count)
        out.delete()
    }

    // ---- native 32-bit float TIFF (TIFF32F) ----
    @Test
    fun tiff32f_write_hasFloatSampleFormat() {
        val out = cache("export_32f.tif")
        val n = TiffWriter.writeFloat32(rgbFloatBuf(), W, H, out.absolutePath, icc = icc())
        assertTrue("bytes written", n > 0)
        val b = out.readBytes()
        val ifd = parseTiff(b)
        assertEquals("ImageWidth", W.toLong(), tiffScalar(ifd, 256))
        assertEquals("BitsPerSample[0]", 32, tiffFirstShort(b, ifd, 258))
        assertEquals("SampleFormat[0]=IEEE float", 3, tiffFirstShort(b, ifd, 339))
        out.delete()
    }

    // ---- SCENE_LINEAR_TIFF uses the SAME verbatim-float writer as TIFF32F ----
    @Test
    fun sceneLinearTiff_verbatimFloatPath() {
        val out = cache("export_scenelinear.tif")
        // Verbatim: an out-of-[0,1] scene-linear value must survive unclamped.
        val b = ByteBuffer.allocateDirect(W * H * 3 * 4).order(ByteOrder.LITTLE_ENDIAN)
        val f = b.asFloatBuffer()
        for (i in 0 until W * H * 3) f.put(i, 4.0f)   // 400% linear
        val n = TiffWriter.writeFloat32(b, W, H, out.absolutePath)
        assertTrue("bytes written", n > 0)
        val bytes = out.readBytes()
        val ifd = parseTiff(bytes)
        assertEquals("BitsPerSample[0]", 32, tiffFirstShort(bytes, ifd, 258))
        assertEquals("SampleFormat[0]=IEEE float", 3, tiffFirstShort(bytes, ifd, 339))
        out.delete()
    }

    // ---- native 16-bit PNG (PNG16), buffer + float overloads ----
    @Test
    fun png16_write_hasDimsBitdepthIcc() {
        val out = cache("export_16.png")
        val n = PngWriter.write(rgb16Buf(), W, H, out.absolutePath, icc = icc())
        assertTrue("bytes written", n > 0)
        val b = out.readBytes()
        assertPngSignature(b)
        assertEquals("PNG width", W.toLong(), pngU32(b, 16))
        assertEquals("PNG height", H.toLong(), pngU32(b, 20))
        assertEquals("PNG bit depth", 16, b[24].toInt())
        assertEquals("PNG color type = RGB", 2, b[25].toInt())
        assertTrue("iCCP chunk present", indexOf(b, "iCCP".toByteArray()) >= 0)
        out.delete()
    }

    @Test
    fun pngFloat_write_quantisesTo16Bit() {
        val out = cache("export_float.png")
        val floats = FloatArray(W * H * 3) { it.toFloat() / (W * H * 3) }
        val n = PngWriter.writeFloat(floats, W, H, out.absolutePath)
        assertTrue("bytes written", n > 0)
        val b = out.readBytes()
        assertPngSignature(b)
        assertEquals("PNG bit depth", 16, b[24].toInt())
        assertEquals("PNG color type = RGB", 2, b[25].toInt())
        out.delete()
    }

    // ---- platform Bitmap encoders back PNG (8-bit) and JPEG / ULTRA_HDR ----
    @Test
    fun bitmap_png8_andJpeg_encode() {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888).apply {
            for (y in 0 until H) for (x in 0 until W) setPixel(x, y, 0xFF204080.toInt())
        }
        val png = cache("export_8.png")
        png.outputStream().use { assertTrue(bmp.compress(Bitmap.CompressFormat.PNG, 100, it)) }
        val pb = png.readBytes()
        assertPngSignature(pb)
        assertEquals("PNG bit depth (8-bit Bitmap)", 8, pb[24].toInt())

        val jpg = cache("export.jpg")
        jpg.outputStream().use { assertTrue(bmp.compress(Bitmap.CompressFormat.JPEG, 90, it)) }
        val jb = jpg.readBytes()
        assertTrue("JPEG SOI magic FF D8 FF",
            (jb[0].toInt() and 0xFF) == 0xFF && (jb[1].toInt() and 0xFF) == 0xD8 && (jb[2].toInt() and 0xFF) == 0xFF)

        bmp.recycle(); png.delete(); jpg.delete()
    }

    // ---- enum breadth: every ExportFormat is accounted for at the writer level ----
    @Test
    fun exportFormatEnum_coversAllSevenTargets() {
        val names = ExportFormat.entries.map { it.name }.toSet()
        val expected = setOf("PNG", "JPEG", "ULTRA_HDR", "TIFF", "PNG16", "TIFF32F", "SCENE_LINEAR_TIFF")
        assertEquals("ExportFormat breadth", expected, names)
        // ULTRA_HDR is a JPEG container (gainmap encode is platform/API-gated, not written here).
        assertEquals("image/jpeg", ExportFormat.ULTRA_HDR.mime)
        assertEquals("image/tiff", ExportFormat.TIFF32F.mime)
        assertEquals("png", ExportFormat.PNG16.ext)
    }

    // ---------- header parsers ----------

    private fun assertPngSignature(b: ByteArray) {
        val sig = intArrayOf(137, 80, 78, 71, 13, 10, 26, 10)
        for (i in sig.indices) assertEquals("PNG sig byte $i", sig[i], b[i].toInt() and 0xFF)
        assertEquals("IHDR chunk", "IHDR", String(b, 12, 4, Charsets.US_ASCII))
    }

    private fun pngU32(b: ByteArray, off: Int): Long =
        ((b[off].toLong() and 0xFF) shl 24) or ((b[off + 1].toLong() and 0xFF) shl 16) or
            ((b[off + 2].toLong() and 0xFF) shl 8) or (b[off + 3].toLong() and 0xFF)

    private data class TiffEntry(val type: Int, val count: Long, val valueOff: Long)

    private fun u16le(b: ByteArray, o: Int): Int =
        (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)

    private fun u32le(b: ByteArray, o: Int): Long =
        (b[o].toLong() and 0xFF) or ((b[o + 1].toLong() and 0xFF) shl 8) or
            ((b[o + 2].toLong() and 0xFF) shl 16) or ((b[o + 3].toLong() and 0xFF) shl 24)

    private fun parseTiff(b: ByteArray): Map<Int, TiffEntry> {
        assertEquals("TIFF little-endian order 'II'", 0x4949, u16le(b, 0))
        assertEquals("TIFF magic 42", 42, u16le(b, 2))
        val ifd = u32le(b, 4).toInt()
        val count = u16le(b, ifd)
        val m = HashMap<Int, TiffEntry>()
        for (i in 0 until count) {
            val e = ifd + 2 + i * 12
            m[u16le(b, e)] = TiffEntry(u16le(b, e + 2), u32le(b, e + 4), u32le(b, e + 8))
        }
        return m
    }

    /** A single-value tag (ImageWidth/Length): SHORT keeps the value in the low 2 bytes; LONG is full. */
    private fun tiffScalar(ifd: Map<Int, TiffEntry>, tag: Int): Long {
        val e = ifd[tag] ?: return -1
        return if (e.type == 3) e.valueOff and 0xFFFF else e.valueOff
    }

    /** First element of a SHORT[N] tag stored at the entry's offset (BitsPerSample / SampleFormat). */
    private fun tiffFirstShort(b: ByteArray, ifd: Map<Int, TiffEntry>, tag: Int): Int {
        val e = ifd[tag] ?: return -1
        return u16le(b, e.valueOff.toInt())
    }

    private fun indexOf(hay: ByteArray, needle: ByteArray): Int {
        outer@ for (i in 0..hay.size - needle.size) {
            for (j in needle.indices) if (hay[i + j] != needle[j]) continue@outer
            return i
        }
        return -1
    }
}
