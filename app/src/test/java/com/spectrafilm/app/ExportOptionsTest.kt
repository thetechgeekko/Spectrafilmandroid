/*
 * Spektrafilm for Android — unit tests for the export-options model (export sheet §6a/§6b). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Pins the resize maths (preserve aspect, never enlarge), the format-aware target long-edge (16-bit
 * always full-res, custom clamped) and the filename sanitiser.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ExportOptionsTest {

    private fun opts(
        format: ExportFormat = ExportFormat.JPEG,
        size: ExportSize = ExportSize.FULL,
        custom: Int = 2048,
    ) = ExportOptions(format, jpegQuality = 90, size = size, customLongEdge = custom, customName = "")

    @Test
    fun scaledDimensions_doesNotEnlarge() {
        assertEquals(1000 to 800, scaledDimensions(1000, 800, 2000))
        assertEquals(1000 to 800, scaledDimensions(1000, 800, 1000)) // exactly fits
    }

    @Test
    fun scaledDimensions_downscalesPreservingAspect() {
        assertEquals(2000 to 1500, scaledDimensions(4000, 3000, 2000))
        assertEquals(1500 to 2000, scaledDimensions(3000, 4000, 2000)) // portrait: long edge is height
    }

    @Test
    fun scaledDimensions_neverGoesBelowOne() {
        assertEquals(2 to 1, scaledDimensions(4000, 2, 2))
    }

    @Test
    fun targetLongEdge_bitmapFormats() {
        assertNull(opts(size = ExportSize.FULL).targetLongEdge())
        assertEquals(2048, opts(size = ExportSize.MEDIUM).targetLongEdge())
        assertEquals(3000, opts(size = ExportSize.CUSTOM, custom = 3000).targetLongEdge())
    }

    @Test
    fun targetLongEdge_customIsClamped() {
        assertEquals(ExportOptions.MIN_CUSTOM_EDGE, opts(size = ExportSize.CUSTOM, custom = 10).targetLongEdge())
        assertEquals(ExportOptions.MAX_CUSTOM_EDGE, opts(size = ExportSize.CUSTOM, custom = 999_999).targetLongEdge())
    }

    @Test
    fun targetLongEdge_emptyCustomFieldMeansFullRes() {
        // 0 = the sheet's custom-size field is empty/unset: full resolution, not a 256 px export.
        assertNull(opts(size = ExportSize.CUSTOM, custom = 0).targetLongEdge())
    }

    @Test
    fun targetLongEdge_16BitAlwaysFullRes() {
        assertNull(opts(format = ExportFormat.TIFF, size = ExportSize.MEDIUM).targetLongEdge())
        assertNull(opts(format = ExportFormat.PNG16, size = ExportSize.CUSTOM, custom = 1000).targetLongEdge())
    }

    @Test
    fun exportBaseName_defaultsWhenBlankOrFullyStripped() {
        assertEquals("Spektrafilm_123", exportBaseName("", 123))
        assertEquals("Spektrafilm_123", exportBaseName("   ", 123))
        assertEquals("Spektrafilm_123", exportBaseName("***", 123))
    }

    @Test
    fun exportBaseName_sanitisesToAPortableName() {
        assertEquals("My_Photo", exportBaseName("My Photo!", 1))
        assertEquals("Roll_12_frame", exportBaseName("Roll 12 — frame", 1))
        assertEquals("abc", exportBaseName("a/b\\c", 1))
        assertEquals("keep-this_1", exportBaseName("keep-this_1", 1))
    }

    @Test
    fun outputDescriptor_rejectsIllegalChoicesBeforeRender() {
        val jpeg = opts(format = ExportFormat.JPEG)
        assertEquals(
            ExistingExportClass.SDR_JPEG8,
            jpeg.outputDescriptor(ColorSpace.SRGB, outputCctfEncoding = true, apiLevel = 24)
                .existingExportClass,
        )
        assertTrue(runCatching {
            jpeg.outputDescriptor(ColorSpace.SRGB, outputCctfEncoding = false, apiLevel = 34)
        }.isFailure)
        assertTrue(runCatching {
            jpeg.outputDescriptor(ColorSpace.ACES2065_1, outputCctfEncoding = true, apiLevel = 34)
        }.isFailure)

        val adobeJpeg = jpeg.outputDescriptor(
            ColorSpace.ADOBE_RGB,
            outputCctfEncoding = true,
            apiLevel = 26,
        )
        assertEquals(26, adobeJpeg.minimumApi)
        assertTrue(runCatching { adobeJpeg.requirePlatformApi(25) }.isFailure)

        val scene = opts(format = ExportFormat.SCENE_LINEAR_TIFF).outputDescriptor(
            ColorSpace.ADOBE_RGB,
            outputCctfEncoding = true,
            apiLevel = 24,
        )
        assertEquals(OutputReference.SCENE_REFERRED, scene.reference)

        // Ultra HDR is exportable on API 34+ since #140 gave it a real, render-derived gain map.
        val ultraHdr = opts(format = ExportFormat.ULTRA_HDR).outputDescriptor(
            ColorSpace.SRGB,
            outputCctfEncoding = true,
            apiLevel = 34,
        )
        assertEquals(ExistingExportClass.ULTRA_HDR_SPATIAL_GAIN_MAP, ultraHdr.existingExportClass)
        assertTrue(runCatching {
            opts(format = ExportFormat.ULTRA_HDR).outputDescriptor(
                ColorSpace.SRGB,
                outputCctfEncoding = true,
                apiLevel = 33,
            )
        }.isFailure)
    }
}
