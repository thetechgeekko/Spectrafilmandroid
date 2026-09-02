/*
 * Spektrafilm for Android — one immutable output contract. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SpektraParams

enum class OutputPrimaries { SRGB_REC709, ADOBE_RGB_1998, PROPHOTO_ROMM, REC2020, ACES_AP0, SOURCE_NATIVE }
enum class OutputWhitePoint { D50, D60, D65, SOURCE_NATIVE }
enum class OutputTransfer { SRGB, ADOBE_GAMMA_2_19921875, PROPHOTO_ROMM, BT2020, LINEAR }
enum class OutputReference { DISPLAY_REFERRED, SCENE_REFERRED }
enum class OutputSampleType { UNSIGNED_INTEGER, IEEE_FLOAT }

enum class OutputBitDepth(val bitsPerSample: Int, val sampleType: OutputSampleType) {
    UINT8(8, OutputSampleType.UNSIGNED_INTEGER),
    UINT16(16, OutputSampleType.UNSIGNED_INTEGER),
    FLOAT32(32, OutputSampleType.IEEE_FLOAT),
}

enum class OutputAlpha { OPAQUE }
enum class OutputRange { NORMALIZED_0_TO_1, EXTENDED_FLOAT, SCENE_FLOAT }
enum class OutputEncoder {
    ANDROID_BITMAP_PNG,
    ANDROID_BITMAP_JPEG,
    NATIVE_PNG16,
    NATIVE_TIFF_UINT16,
    NATIVE_TIFF_FLOAT32,
}

enum class OutputQuantizer { ARGB8888_ROUND_CLAMP, UINT16_ROUND_CLAMP, VERBATIM_FLOAT32 }
enum class OutputReleaseStatus { SHIPPED_CLASSIFIED, BLOCKED_PENDING_HONEST_HDR }
enum class OutputExifColorSpace(val value: Int) { SRGB(1), UNCALIBRATED(0xFFFF) }

enum class ExistingExportClass(val id: String) {
    SDR_PNG8("sdr-png8-v1"),
    SDR_JPEG8("sdr-jpeg8-v1"),
    ULTRA_HDR_SPATIAL_GAIN_MAP("ultrahdr-spatial-gainmap-v1"),
    RENDERED_TIFF16("rendered-tiff16-v1"),
    RENDERED_PNG16("rendered-png16-v1"),
    RENDERED_TIFF32F("rendered-tiff32f-v1"),
    SCENE_LINEAR_INPUT_TIFF32F("scene-linear-input-tiff32f-v1"),
}

data class OutputMetadataPolicy(
    val bitmapColorSpaceName: String?,
    val iccAssetPath: String?,
    val exifColorSpace: OutputExifColorSpace,
    val copySourceExif: Boolean,
    val hdrGainMap: HdrGainMapContract?,
)

/**
 * The policy for building a spatial HDR gain map. It carries no ratioMax: the headroom a file may
 * claim is a property of the RENDER, measured per image by [HdrGainMap], not a constant chosen in
 * advance. Fixing it in advance is precisely what made the old 1x1 placeholder dishonest.
 */
data class HdrGainMapContract(
    /** The map is the image divided by this; gain maps are low-frequency by nature. */
    val downsample: Int,
    val ratioMin: Float,
    /** The most headroom this contract permits a file to claim, whatever the render contains. */
    val ratioMaxCeiling: Float,
    val gamma: Float,
    val epsilonSdr: Float,
    val epsilonHdr: Float,
    val minDisplayRatioForHdrTransition: Float,
) {
    val isSpatial: Boolean get() = downsample >= 1
}

/**
 * The only value allowed to cross from export choices into rendering/encoding. The constructor is
 * private so callers cannot use `copy` or directly assemble a format/transfer/depth contradiction.
 */
class OutputDescriptor private constructor(
    val format: ExportFormat,
    val primaries: OutputPrimaries,
    val whitePoint: OutputWhitePoint,
    val transfer: OutputTransfer,
    val reference: OutputReference,
    val bitDepth: OutputBitDepth,
    val alpha: OutputAlpha,
    val range: OutputRange,
    val encoder: OutputEncoder,
    val quantizer: OutputQuantizer,
    val metadata: OutputMetadataPolicy,
    val existingExportClass: ExistingExportClass,
    val engineColorSpace: ColorSpace?,
    val engineCctfEncoding: Boolean?,
    val minimumApi: Int,
    val releaseStatus: OutputReleaseStatus,
) {
    fun applyTo(params: SpektraParams): SpektraParams {
        val colorSpace = requireNotNull(engineColorSpace) { "Scene-linear input export bypasses the engine" }
        val cctf = requireNotNull(engineCctfEncoding) { "Scene-linear input export bypasses the engine" }
        return params.copy(io = params.io.copy(
            outputColorSpace = colorSpace,
            outputCctfEncoding = cctf,
        ))
    }

    fun requirePlatformApi(apiLevel: Int): OutputDescriptor {
        require(apiLevel >= minimumApi) {
            "${format.display} requires Android API $minimumApi or newer (device API $apiLevel)"
        }
        return this
    }

    /** Release preflight used by every executable export path, before decode/render begins. */
    fun requireExportable(apiLevel: Int): OutputDescriptor {
        requirePlatformApi(apiLevel)
        require(releaseStatus == OutputReleaseStatus.SHIPPED_CLASSIFIED) {
            "${format.display} is blocked until an honest spatial HDR gain-map pipeline ships"
        }
        return this
    }

    override fun equals(other: Any?): Boolean = this === other || (
        other is OutputDescriptor &&
            format == other.format &&
            primaries == other.primaries &&
            whitePoint == other.whitePoint &&
            transfer == other.transfer &&
            reference == other.reference &&
            bitDepth == other.bitDepth &&
            alpha == other.alpha &&
            range == other.range &&
            encoder == other.encoder &&
            quantizer == other.quantizer &&
            metadata == other.metadata &&
            existingExportClass == other.existingExportClass &&
            engineColorSpace == other.engineColorSpace &&
            engineCctfEncoding == other.engineCctfEncoding &&
            minimumApi == other.minimumApi &&
            releaseStatus == other.releaseStatus
        )

    override fun hashCode(): Int = listOf(
        format, primaries, whitePoint, transfer, reference, bitDepth, alpha, range,
        encoder, quantizer, metadata, existingExportClass, engineColorSpace,
        engineCctfEncoding, minimumApi, releaseStatus,
    ).hashCode()

    override fun toString(): String =
        "OutputDescriptor(${existingExportClass.id}, ${engineColorSpace ?: primaries}, " +
            "$transfer, $bitDepth, $encoder)"

    companion object {
        private data class ColorContract(
            val primaries: OutputPrimaries,
            val whitePoint: OutputWhitePoint,
            val transfer: OutputTransfer,
            val cctfEncoding: Boolean,
            val bitmapColorSpaceName: String?,
            val iccAssetPath: String,
        )

        private data class FormatContract(
            val depth: OutputBitDepth,
            val encoder: OutputEncoder,
            val quantizer: OutputQuantizer,
            val existingExportClass: ExistingExportClass,
            val copySourceExif: Boolean,
            val embedsIccDirectly: Boolean,
        )

        fun rendered(
            format: ExportFormat,
            colorSpace: ColorSpace,
            outputCctfEncoding: Boolean,
            bitDepth: OutputBitDepth,
        ): OutputDescriptor {
            require(format != ExportFormat.SCENE_LINEAR_TIFF) {
                "Scene-linear input export must use sceneLinearTiff()"
            }
            val color = colorContract(colorSpace)
            val output = formatContract(format)
            require(outputCctfEncoding == color.cctfEncoding) {
                "$colorSpace requires outputCctfEncoding=${color.cctfEncoding}"
            }
            require(bitDepth == output.depth) {
                "$format requires ${output.depth.bitsPerSample}-bit ${output.depth.sampleType} samples"
            }
            val isBitmapEncoder = output.encoder == OutputEncoder.ANDROID_BITMAP_JPEG ||
                output.encoder == OutputEncoder.ANDROID_BITMAP_PNG
            if (isBitmapEncoder) {
                require(color.bitmapColorSpaceName != null) {
                    "$colorSpace has no faithful ARGB_8888 Bitmap color-space tag"
                }
            }
            if (format == ExportFormat.ULTRA_HDR) {
                require(colorSpace == ColorSpace.SRGB) {
                    "The classified Ultra HDR placeholder contract requires an encoded sRGB base"
                }
            }
            val minimumApi = when {
                format == ExportFormat.ULTRA_HDR -> 34
                isBitmapEncoder && color.bitmapColorSpaceName != "SRGB" -> 26
                else -> 24
            }
            return OutputDescriptor(
                format = format,
                primaries = color.primaries,
                whitePoint = color.whitePoint,
                transfer = color.transfer,
                reference = OutputReference.DISPLAY_REFERRED,
                bitDepth = bitDepth,
                alpha = OutputAlpha.OPAQUE,
                range = if (bitDepth == OutputBitDepth.FLOAT32) {
                    OutputRange.EXTENDED_FLOAT
                } else {
                    OutputRange.NORMALIZED_0_TO_1
                },
                encoder = output.encoder,
                quantizer = output.quantizer,
                metadata = OutputMetadataPolicy(
                    bitmapColorSpaceName = if (isBitmapEncoder) color.bitmapColorSpaceName else null,
                    iccAssetPath = if (output.embedsIccDirectly) color.iccAssetPath else null,
                    exifColorSpace = if (colorSpace == ColorSpace.SRGB) {
                        OutputExifColorSpace.SRGB
                    } else {
                        OutputExifColorSpace.UNCALIBRATED
                    },
                    copySourceExif = output.copySourceExif,
                    hdrGainMap = if (format == ExportFormat.ULTRA_HDR) {
                        HdrGainMapContract(
                            // Quarter resolution: the ISO 21496-1 / Android encoders treat the map
                            // as low-frequency, and a full-resolution one costs bytes for detail
                            // no display reconstructs.
                            downsample = 4,
                            ratioMin = 1.0f,
                            // +3 stops. A handful of overshooting pixels must not be able to
                            // stretch the whole map's scale and flatten the real detail.
                            ratioMaxCeiling = 8.0f,
                            gamma = 1.0f,
                            epsilonSdr = 0.015625f,
                            epsilonHdr = 0.015625f,
                            minDisplayRatioForHdrTransition = 1.0f,
                        )
                    } else {
                        null
                    },
                ),
                existingExportClass = output.existingExportClass,
                engineColorSpace = colorSpace,
                engineCctfEncoding = outputCctfEncoding,
                minimumApi = minimumApi,
                // #140: the gain map is now derived per pixel from the render, so the format
                // no longer over-promises and is releasable.
                releaseStatus = OutputReleaseStatus.SHIPPED_CLASSIFIED,
            )
        }

        fun sceneLinearTiff(bitDepth: OutputBitDepth): OutputDescriptor {
            require(bitDepth == OutputBitDepth.FLOAT32) {
                "Scene-linear input export requires verbatim 32-bit IEEE-float samples"
            }
            return OutputDescriptor(
                format = ExportFormat.SCENE_LINEAR_TIFF,
                primaries = OutputPrimaries.SOURCE_NATIVE,
                whitePoint = OutputWhitePoint.SOURCE_NATIVE,
                transfer = OutputTransfer.LINEAR,
                reference = OutputReference.SCENE_REFERRED,
                bitDepth = bitDepth,
                alpha = OutputAlpha.OPAQUE,
                range = OutputRange.SCENE_FLOAT,
                encoder = OutputEncoder.NATIVE_TIFF_FLOAT32,
                quantizer = OutputQuantizer.VERBATIM_FLOAT32,
                metadata = OutputMetadataPolicy(
                    bitmapColorSpaceName = null,
                    iccAssetPath = null,
                    exifColorSpace = OutputExifColorSpace.UNCALIBRATED,
                    copySourceExif = false,
                    hdrGainMap = null,
                ),
                existingExportClass = ExistingExportClass.SCENE_LINEAR_INPUT_TIFF32F,
                engineColorSpace = null,
                engineCctfEncoding = null,
                minimumApi = 24,
                releaseStatus = OutputReleaseStatus.SHIPPED_CLASSIFIED,
            )
        }

        internal fun bitmapColorSpaceNameFor(colorSpace: ColorSpace): String? =
            colorContract(colorSpace).bitmapColorSpaceName

        internal fun iccAssetPathFor(colorSpace: ColorSpace): String =
            colorContract(colorSpace).iccAssetPath

        internal fun fixedBitDepth(format: ExportFormat): OutputBitDepth = when (format) {
            ExportFormat.SCENE_LINEAR_TIFF -> OutputBitDepth.FLOAT32
            else -> formatContract(format).depth
        }

        private fun colorContract(colorSpace: ColorSpace): ColorContract = when (colorSpace) {
            ColorSpace.SRGB -> ColorContract(
                OutputPrimaries.SRGB_REC709, OutputWhitePoint.D65, OutputTransfer.SRGB, true,
                "SRGB", "spektra/icc/saucecontrol/sRGB-v4.icc",
            )
            ColorSpace.ADOBE_RGB -> ColorContract(
                OutputPrimaries.ADOBE_RGB_1998, OutputWhitePoint.D65,
                OutputTransfer.ADOBE_GAMMA_2_19921875, true,
                "ADOBE_RGB", "spektra/icc/saucecontrol/AdobeCompat-v4.icc",
            )
            ColorSpace.PROPHOTO -> ColorContract(
                OutputPrimaries.PROPHOTO_ROMM, OutputWhitePoint.D50,
                OutputTransfer.PROPHOTO_ROMM, true,
                "PRO_PHOTO_RGB", "spektra/icc/saucecontrol/ProPhoto-v4.icc",
            )
            ColorSpace.REC2020 -> ColorContract(
                OutputPrimaries.REC2020, OutputWhitePoint.D65, OutputTransfer.BT2020, true,
                "BT2020", "spektra/icc/saucecontrol/Rec2020-v4.icc",
            )
            ColorSpace.ACES2065_1 -> ColorContract(
                OutputPrimaries.ACES_AP0, OutputWhitePoint.D60, OutputTransfer.LINEAR, true,
                null, "spektra/icc/ellelstone/ACES-elle-V4-g10.icc",
            )
            ColorSpace.LINEAR_SRGB -> ColorContract(
                OutputPrimaries.SRGB_REC709, OutputWhitePoint.D65, OutputTransfer.LINEAR, false,
                "LINEAR_SRGB", "spektra/icc/ellelstone/sRGB-elle-V4-g10.icc",
            )
        }

        private fun formatContract(format: ExportFormat): FormatContract = when (format) {
            ExportFormat.PNG -> FormatContract(
                OutputBitDepth.UINT8, OutputEncoder.ANDROID_BITMAP_PNG,
                OutputQuantizer.ARGB8888_ROUND_CLAMP, ExistingExportClass.SDR_PNG8,
                copySourceExif = false, embedsIccDirectly = false,
            )
            ExportFormat.JPEG -> FormatContract(
                OutputBitDepth.UINT8, OutputEncoder.ANDROID_BITMAP_JPEG,
                OutputQuantizer.ARGB8888_ROUND_CLAMP, ExistingExportClass.SDR_JPEG8,
                copySourceExif = true, embedsIccDirectly = false,
            )
            ExportFormat.ULTRA_HDR -> FormatContract(
                OutputBitDepth.UINT8, OutputEncoder.ANDROID_BITMAP_JPEG,
                OutputQuantizer.ARGB8888_ROUND_CLAMP,
                ExistingExportClass.ULTRA_HDR_SPATIAL_GAIN_MAP,
                copySourceExif = true, embedsIccDirectly = false,
            )
            ExportFormat.TIFF -> FormatContract(
                OutputBitDepth.UINT16, OutputEncoder.NATIVE_TIFF_UINT16,
                OutputQuantizer.UINT16_ROUND_CLAMP, ExistingExportClass.RENDERED_TIFF16,
                copySourceExif = false, embedsIccDirectly = true,
            )
            ExportFormat.PNG16 -> FormatContract(
                OutputBitDepth.UINT16, OutputEncoder.NATIVE_PNG16,
                OutputQuantizer.UINT16_ROUND_CLAMP, ExistingExportClass.RENDERED_PNG16,
                copySourceExif = false, embedsIccDirectly = true,
            )
            ExportFormat.TIFF32F -> FormatContract(
                OutputBitDepth.FLOAT32, OutputEncoder.NATIVE_TIFF_FLOAT32,
                OutputQuantizer.VERBATIM_FLOAT32, ExistingExportClass.RENDERED_TIFF32F,
                copySourceExif = false, embedsIccDirectly = true,
            )
            ExportFormat.SCENE_LINEAR_TIFF -> error("Scene-linear input export is not rendered")
        }
    }
}
