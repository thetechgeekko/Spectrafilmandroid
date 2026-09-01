/*
 * Spektrafilm for Android — runtime camera capability probing (#196). GPLv3.
 *
 * The capture route is decided by RUNTIME capability evidence — the advertised
 * REQUEST_AVAILABLE_CAPABILITIES set AND a concrete RAW_SENSOR output size — never by
 * model or OS-version guesses. A probe failure yields NO decision (the camera action is
 * then not offered), and a missing capability list fails closed to the honest JPEG label.
 */
package com.spectrafilm.app.camera

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata

/**
 * One camera's capture-route decision, derived purely from probe evidence.
 * [decide] is the pure core (JVM-tested); [probe] is the thin Android wrapper.
 */
data class CameraRouteDecision(
    val rawSupported: Boolean,
    val maxRawWidth: Int,
    val maxRawHeight: Int,
    val manualSensor: Boolean,
) {
    /** Honest user-facing route label — a fallback is never dressed up as RAW. */
    val routeLabel: String
        get() = if (rawSupported) "RAW (DNG)" else "JPEG (this camera does not provide RAW)"

    companion object {
        // Mirrors CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_* so the decision core
        // stays JVM-testable without android.jar. Values are stable public API constants.
        const val CAPABILITY_BACKWARD_COMPATIBLE = 0
        const val CAPABILITY_MANUAL_SENSOR = 1
        const val CAPABILITY_RAW = 3

        /** Pure decision: RAW needs the advertised capability AND a concrete output size. */
        fun decide(capabilities: IntArray?, rawSizes: List<Pair<Int, Int>>): CameraRouteDecision {
            val caps = capabilities ?: intArrayOf()
            val largest = rawSizes.maxByOrNull { (w, h) -> w.toLong() * h.toLong() }
            val raw = caps.contains(CAPABILITY_RAW) && largest != null
            return CameraRouteDecision(
                rawSupported = raw,
                maxRawWidth = if (raw) largest!!.first else 0,
                maxRawHeight = if (raw) largest!!.second else 0,
                manualSensor = caps.contains(CAPABILITY_MANUAL_SENSOR),
            )
        }

        /** A throwing probe yields null — no decision, never a guess. */
        fun decideOrNull(probe: () -> CameraRouteDecision): CameraRouteDecision? =
            runCatching(probe).getOrNull()

        /**
         * Probe the back camera the shell will open. Returns the camera id with its
         * decision, or null when no back camera is usable (the camera action is then
         * not offered — a dead tile is worse than no tile).
         */
        fun probe(context: Context): Pair<String, CameraRouteDecision>? = decideOrNullWithId {
            val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val backId = manager.cameraIdList.firstOrNull { id ->
                manager.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.LENS_FACING) == CameraMetadata.LENS_FACING_BACK
            } ?: error("no back camera")
            val chars = manager.getCameraCharacteristics(backId)
            val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            val sizes = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?.getOutputSizes(ImageFormat.RAW_SENSOR)
                ?.map { it.width to it.height }
                .orEmpty()
            // The mirror constants equal the stable platform values (BACKWARD_COMPATIBLE=0,
            // MANUAL_SENSOR=1, RAW=3), so the capability array passes through directly.
            backId to decide(caps, sizes)
        }

        private fun decideOrNullWithId(
            probe: () -> Pair<String, CameraRouteDecision>,
        ): Pair<String, CameraRouteDecision>? = runCatching(probe).getOrNull()
    }
}
