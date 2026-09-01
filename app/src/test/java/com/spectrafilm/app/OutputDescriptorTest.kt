/*
 * Spektrafilm for Android — output contract tests. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SpektraParams
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OutputDescriptorTest {

    @Test
    fun sdrJpeg_isOneImmutableSelfConsistentContract() {
        val descriptor = OutputDescriptor.rendered(
            format = ExportFormat.JPEG,
            colorSpace = ColorSpace.SRGB,
            outputCctfEncoding = true,
            bitDepth = OutputBitDepth.UINT8,
        )

        assertEquals(OutputPrimaries.SRGB_REC709, descriptor.primaries)
        assertEquals(OutputWhitePoint.D65, descriptor.whitePoint)
        assertEquals(OutputTransfer.SRGB, descriptor.transfer)
        assertEquals(OutputReference.DISPLAY_REFERRED, descriptor.reference)
        assertEquals(OutputBitDepth.UINT8, descriptor.bitDepth)
        assertEquals(OutputAlpha.OPAQUE, descriptor.alpha)
        assertEquals(OutputRange.NORMALIZED_0_TO_1, descriptor.range)
        assertEquals(OutputEncoder.ANDROID_BITMAP_JPEG, descriptor.encoder)
        assertEquals(OutputQuantizer.ARGB8888_ROUND_CLAMP, descriptor.quantizer)
        assertEquals("SRGB", descriptor.metadata.bitmapColorSpaceName)
        assertNull(descriptor.metadata.iccAssetPath)
        assertTrue(descriptor.metadata.copySourceExif)
        assertNull(descriptor.metadata.hdrGainMap)
        assertEquals("sdr-jpeg8-v1", descriptor.existingExportClass.id)

        val original = SpektraParams(filmProfile = "film", printProfile = "paper")
        val applied = descriptor.applyTo(original)
        assertEquals(ColorSpace.SRGB, applied.io.outputColorSpace)
        assertTrue(applied.io.outputCctfEncoding)
        assertEquals(original.copy(io = original.io.copy(
            outputColorSpace = ColorSpace.SRGB,
            outputCctfEncoding = true,
        )), applied)

        val sameValue = OutputDescriptor.rendered(
            ExportFormat.JPEG,
            ColorSpace.SRGB,
            outputCctfEncoding = true,
            bitDepth = OutputBitDepth.UINT8,
        )
        assertEquals("descriptor is a value suitable for cache keys", descriptor, sameValue)
        assertEquals(descriptor.hashCode(), sameValue.hashCode())
    }

    @Test
    fun renderedLegality_isExhaustiveAcrossFormatSpaceTransferAndDepth() {
        val canonicalCctf = mapOf(
            ColorSpace.SRGB to true,
            ColorSpace.ADOBE_RGB to true,
            ColorSpace.PROPHOTO to true,
            ColorSpace.REC2020 to true,
            ColorSpace.ACES2065_1 to true,
            ColorSpace.LINEAR_SRGB to false,
        )
        val canonicalTransfer = mapOf(
            ColorSpace.SRGB to OutputTransfer.SRGB,
            ColorSpace.ADOBE_RGB to OutputTransfer.ADOBE_GAMMA_2_19921875,
            ColorSpace.PROPHOTO to OutputTransfer.PROPHOTO_ROMM,
            ColorSpace.REC2020 to OutputTransfer.BT2020,
            ColorSpace.ACES2065_1 to OutputTransfer.LINEAR,
            ColorSpace.LINEAR_SRGB to OutputTransfer.LINEAR,
        )
        val requiredDepth = mapOf(
            ExportFormat.PNG to OutputBitDepth.UINT8,
            ExportFormat.JPEG to OutputBitDepth.UINT8,
            ExportFormat.ULTRA_HDR to OutputBitDepth.UINT8,
            ExportFormat.TIFF to OutputBitDepth.UINT16,
            ExportFormat.PNG16 to OutputBitDepth.UINT16,
            ExportFormat.TIFF32F to OutputBitDepth.FLOAT32,
        )
        val supportedSpaces = mapOf(
            ExportFormat.PNG to setOf(
                ColorSpace.SRGB, ColorSpace.ADOBE_RGB, ColorSpace.PROPHOTO,
                ColorSpace.REC2020, ColorSpace.LINEAR_SRGB,
            ),
            ExportFormat.JPEG to setOf(
                ColorSpace.SRGB, ColorSpace.ADOBE_RGB, ColorSpace.PROPHOTO,
                ColorSpace.REC2020, ColorSpace.LINEAR_SRGB,
            ),
            ExportFormat.ULTRA_HDR to setOf(ColorSpace.SRGB),
            ExportFormat.TIFF to ColorSpace.entries.toSet(),
            ExportFormat.PNG16 to ColorSpace.entries.toSet(),
            ExportFormat.TIFF32F to ColorSpace.entries.toSet(),
        )
        val expectedEncoder = mapOf(
            ExportFormat.PNG to OutputEncoder.ANDROID_BITMAP_PNG,
            ExportFormat.JPEG to OutputEncoder.ANDROID_BITMAP_JPEG,
            ExportFormat.ULTRA_HDR to OutputEncoder.ANDROID_BITMAP_JPEG,
            ExportFormat.TIFF to OutputEncoder.NATIVE_TIFF_UINT16,
            ExportFormat.PNG16 to OutputEncoder.NATIVE_PNG16,
            ExportFormat.TIFF32F to OutputEncoder.NATIVE_TIFF_FLOAT32,
        )
        val expectedQuantizer = mapOf(
            ExportFormat.PNG to OutputQuantizer.ARGB8888_ROUND_CLAMP,
            ExportFormat.JPEG to OutputQuantizer.ARGB8888_ROUND_CLAMP,
            ExportFormat.ULTRA_HDR to OutputQuantizer.ARGB8888_ROUND_CLAMP,
            ExportFormat.TIFF to OutputQuantizer.UINT16_ROUND_CLAMP,
            ExportFormat.PNG16 to OutputQuantizer.UINT16_ROUND_CLAMP,
            ExportFormat.TIFF32F to OutputQuantizer.VERBATIM_FLOAT32,
        )

        var accepted = 0
        for (format in ExportFormat.entries) {
            for (space in ColorSpace.entries) {
                for (cctf in listOf(false, true)) {
                    for (depth in OutputBitDepth.entries) {
                        val expected = format in requiredDepth &&
                            depth == requiredDepth[format] &&
                            cctf == canonicalCctf.getValue(space) &&
                            space in supportedSpaces.getValue(format)
                        val result = runCatching {
                            OutputDescriptor.rendered(format, space, cctf, depth)
                        }
                        assertEquals(
                            "$format / $space / cctf=$cctf / $depth",
                            expected,
                            result.isSuccess,
                        )
                        if (expected) {
                            accepted++
                            val descriptor = result.getOrThrow()
                            assertEquals(format, descriptor.format)
                            assertEquals(space, descriptor.engineColorSpace)
                            assertEquals(cctf, descriptor.engineCctfEncoding)
                            assertEquals(canonicalTransfer.getValue(space), descriptor.transfer)
                            assertEquals(depth, descriptor.bitDepth)
                            assertEquals(expectedEncoder.getValue(format), descriptor.encoder)
                            assertEquals(expectedQuantizer.getValue(format), descriptor.quantizer)
                            assertEquals(
                                if (depth == OutputBitDepth.FLOAT32) {
                                    OutputRange.EXTENDED_FLOAT
                                } else {
                                    OutputRange.NORMALIZED_0_TO_1
                                },
                                descriptor.range,
                            )
                            val bitmapEncoded = descriptor.encoder == OutputEncoder.ANDROID_BITMAP_PNG ||
                                descriptor.encoder == OutputEncoder.ANDROID_BITMAP_JPEG
                            assertEquals(bitmapEncoded, descriptor.metadata.bitmapColorSpaceName != null)
                            assertEquals(!bitmapEncoded, descriptor.metadata.iccAssetPath != null)
                        }
                    }
                }
            }
        }
        assertEquals("all and only the 29 classified rendered contracts", 29, accepted)
    }

    @Test
    fun sceneLinearInput_isOnlyAnUntaggedVerbatimFloatTiff() {
        val descriptor = OutputDescriptor.sceneLinearTiff(OutputBitDepth.FLOAT32)

        assertEquals(ExportFormat.SCENE_LINEAR_TIFF, descriptor.format)
        assertEquals(OutputPrimaries.SOURCE_NATIVE, descriptor.primaries)
        assertEquals(OutputWhitePoint.SOURCE_NATIVE, descriptor.whitePoint)
        assertEquals(OutputTransfer.LINEAR, descriptor.transfer)
        assertEquals(OutputReference.SCENE_REFERRED, descriptor.reference)
        assertEquals(OutputBitDepth.FLOAT32, descriptor.bitDepth)
        assertEquals(OutputRange.SCENE_FLOAT, descriptor.range)
        assertEquals(OutputEncoder.NATIVE_TIFF_FLOAT32, descriptor.encoder)
        assertEquals(OutputQuantizer.VERBATIM_FLOAT32, descriptor.quantizer)
        assertNull(descriptor.engineColorSpace)
        assertNull(descriptor.engineCctfEncoding)
        assertNull(descriptor.metadata.bitmapColorSpaceName)
        assertNull(descriptor.metadata.iccAssetPath)
        assertEquals(false, descriptor.metadata.copySourceExif)
        assertEquals(
            ExistingExportClass.SCENE_LINEAR_INPUT_TIFF32F,
            descriptor.existingExportClass,
        )

        assertTrue(runCatching {
            OutputDescriptor.sceneLinearTiff(OutputBitDepth.UINT16)
        }.isFailure)
        assertTrue(runCatching {
            descriptor.applyTo(SpektraParams("film", "paper"))
        }.isFailure)
    }

    @Test
    fun ultraHdrPlaceholder_hasMeasuredMetadataAndIsReleaseBlocked() {
        val descriptor = OutputDescriptor.rendered(
            ExportFormat.ULTRA_HDR,
            ColorSpace.SRGB,
            outputCctfEncoding = true,
            bitDepth = OutputBitDepth.UINT8,
        )
        val gainMap = checkNotNull(descriptor.metadata.hdrGainMap)

        assertEquals(1, gainMap.width)
        assertEquals(1, gainMap.height)
        assertEquals(false, gainMap.isSpatial)
        assertEquals(1.0f, gainMap.ratioMin)
        assertEquals(1.6f, gainMap.ratioMax)
        assertEquals(1.0f, gainMap.gamma)
        assertEquals(0.015625f, gainMap.epsilonSdr)
        assertEquals(0.015625f, gainMap.epsilonHdr)
        assertEquals(1.6f, gainMap.displayRatioForFullHdr)
        assertEquals(1.0f, gainMap.minDisplayRatioForHdrTransition)
        assertEquals(OutputReleaseStatus.BLOCKED_PENDING_HONEST_HDR, descriptor.releaseStatus)
        assertEquals(34, descriptor.minimumApi)
        assertTrue(runCatching { descriptor.requirePlatformApi(33) }.isFailure)
        assertEquals(descriptor, descriptor.requirePlatformApi(34))
        assertTrue(runCatching { descriptor.requireExportable(34) }.isFailure)
    }

    @Test
    fun exifColorSpaceTag_isDerivedFromOutputSamplesNotCopiedFromSource() {
        val expected = mapOf(
            ColorSpace.SRGB to OutputExifColorSpace.SRGB,
            ColorSpace.ADOBE_RGB to OutputExifColorSpace.UNCALIBRATED,
            ColorSpace.PROPHOTO to OutputExifColorSpace.UNCALIBRATED,
            ColorSpace.REC2020 to OutputExifColorSpace.UNCALIBRATED,
            ColorSpace.ACES2065_1 to OutputExifColorSpace.UNCALIBRATED,
            ColorSpace.LINEAR_SRGB to OutputExifColorSpace.UNCALIBRATED,
        )
        for ((space, exif) in expected) {
            val cctf = space != ColorSpace.LINEAR_SRGB
            val descriptor = OutputDescriptor.rendered(
                ExportFormat.TIFF,
                space,
                outputCctfEncoding = cctf,
                bitDepth = OutputBitDepth.UINT16,
            )
            assertEquals(space.name, exif, descriptor.metadata.exifColorSpace)
        }
        assertEquals(
            OutputExifColorSpace.UNCALIBRATED,
            OutputDescriptor.sceneLinearTiff(OutputBitDepth.FLOAT32).metadata.exifColorSpace,
        )
    }

    @Test
    fun everyExistingExportFormat_hasAStablePreGoldenClassification() {
        val expected = mapOf(
            ExportFormat.PNG to ExistingExportClass.SDR_PNG8,
            ExportFormat.JPEG to ExistingExportClass.SDR_JPEG8,
            ExportFormat.ULTRA_HDR to ExistingExportClass.ULTRA_HDR_UNIFORM_GAIN_MAP_PLACEHOLDER,
            ExportFormat.TIFF to ExistingExportClass.RENDERED_TIFF16,
            ExportFormat.PNG16 to ExistingExportClass.RENDERED_PNG16,
            ExportFormat.TIFF32F to ExistingExportClass.RENDERED_TIFF32F,
            ExportFormat.SCENE_LINEAR_TIFF to ExistingExportClass.SCENE_LINEAR_INPUT_TIFF32F,
        )
        for ((format, classification) in expected) {
            val descriptor = if (format == ExportFormat.SCENE_LINEAR_TIFF) {
                OutputDescriptor.sceneLinearTiff(OutputBitDepth.FLOAT32)
            } else {
                OutputDescriptor.rendered(
                    format,
                    ColorSpace.SRGB,
                    outputCctfEncoding = true,
                    bitDepth = OutputDescriptor.fixedBitDepth(format),
                )
            }
            assertEquals(format.name, classification, descriptor.existingExportClass)
        }
        assertEquals(ExportFormat.entries.toSet(), expected.keys)
    }
}
