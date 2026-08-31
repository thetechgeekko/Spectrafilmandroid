/*
 * Spektrafilm for Android — immutable magnifier/ROI render state. GPLv3.
 */
package com.spectrafilm.app

import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.engine.SpektraParams

/** One edit generation used unchanged by every worker pass and its final grade. */
internal data class InteractiveRenderSnapshot(
    val renderKey: Int,
    val params: SpektraParams,
    val cctfEncoded: Boolean,
    val saturation: Float,
    val vibrance: Float,
    val gamutCompress: Float,
    val localAdjustments: List<LocalAdjustment>,
) {
    fun paramsForCrop(
        cropFraction: Float,
        previewMaxSize: Int? = null,
    ): SpektraParams {
        require(cropFraction.isFinite() && cropFraction > 0f) {
            "crop fraction must be finite and positive"
        }
        val settings = if (previewMaxSize == null) {
            params.settings
        } else {
            require(previewMaxSize > 0) { "preview size must be positive" }
            params.settings.copy(previewMaxSize = previewMaxSize)
        }
        return params.copy(
            camera = params.camera.copy(
                filmFormatMm = params.camera.filmFormatMm * cropFraction,
            ),
            settings = settings,
        )
    }
}

/** Must be called on the UI thread that owns [ParamsState]. */
internal fun ParamsState.captureInteractiveRenderSnapshot(
    renderKey: Int,
): InteractiveRenderSnapshot = InteractiveRenderSnapshot(
    renderKey = renderKey,
    params = toParams(),
    cctfEncoded = savingCctfEncoding,
    saturation = saturation,
    vibrance = vibrance,
    gamutCompress = gamutCompress,
    localAdjustments = localAdjustments.toList(),
)
