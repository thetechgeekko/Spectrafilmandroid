/*
 * Spektrafilm for Android — default-off ticket #139 device probe implementation. GPLv3.
 */
package com.spectrafilm.app

import android.content.Context
import android.content.pm.ApplicationInfo
import android.graphics.Bitmap
import android.os.Build
import android.os.Process
import com.spectrafilm.app.masks.BlendMode
import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.app.masks.Mask
import com.spectrafilm.app.masks.MaskComponent
import com.spectrafilm.app.masks.TierADelta
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.InputGamutCompress
import com.spectrafilm.engine.OutputGamutCompress
import com.spectrafilm.engine.Rgb2Raw
import com.spectrafilm.engine.SpektraEngine
import com.spectrafilm.libraw.WhiteBalance
import java.io.File
import java.io.IOException
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import org.json.JSONArray
import org.json.JSONObject

/** Internal, default-off implementation behind the small kept release-test ABI facade. */
internal object Ticket139EditorProbe {
    internal data class NavigationRequest(val sequence: Long, val destination: String)
    internal data class SourceProbeRequest(val sequence: Long, val label: String)
    internal data class ExportProbeRequest(val sequence: Long)
    internal data class PreviewCompletionRequest(val sequence: Long)

    private data class MemoryCurrentSignature(
        val totalBytes: Long,
        val domainBytes: List<Long>,
        val stageBytes: List<Long>,
    )

    private data class PreviewCompletionProbe(
        val sequence: Long,
        val baseline: MemoryCurrentSignature,
        val release: CompletableDeferred<Unit>,
        var entered: MemoryCurrentSignature? = null,
        var sourceBPublished: MemoryCurrentSignature? = null,
        var ownerCancellationObserved: Boolean = false,
        var stalePublicationRejected: Boolean = false,
        var cleanup: MemoryCurrentSignature? = null,
    )

    private data class OverlayDraftEvidence(
        val generation: Long,
        val tool: EditorOverlayTool,
        val bitmapWidth: Int,
        val bitmapHeight: Int,
    )

    private const val PROBE_DIRECTORY = "ticket139-editor-oracle"
    private const val PROCESS_MARKER = "ticket139-process-probe.json"
    private const val PROCESS_ORACLE = "ticket139-process-oracle.json"
    private const val ORACLE_SCHEMA = "org.spektrafilm.ticket139-editor-oracle"
    private const val ORACLE_VERSION = 1
    private const val EXPECTED_ROTATION = 270
    private const val EXPECTED_MASK_INDEX = 2
    private const val EXPECTED_UNDO = 1
    private const val EXPECTED_REDO = 1

    private val EXPECTED_PERSISTED_PARAMS_FIELDS = setOf(
        "filmProfile",
        "printProfile",
        "input.inputColorSpace",
        "input.inputCctfDecoding",
        "input.spectralUpsampling",
        "input.adaptationWindow",
        "input.adaptationSurface",
        "input.spectralGaussianBlur",
        "input.filterUv",
        "input.filterIr",
        "input.upscaleFactor",
        "input.crop",
        "input.cropCenter",
        "input.cropSize",
        "raw.whiteBalance",
        "raw.temperature",
        "raw.tint",
        "creativeWb.temp",
        "creativeWb.tint",
        "creativeWb.balanceToFilmStock",
        "grade.contrast",
        "grade.saturation",
        "grade.vibrance",
        "grade.gamutCompress",
        "masks",
        "camera.exposureCompensationEv",
        "camera.autoExposure",
        "camera.autoExposureMethod",
        "camera.filmFormatMm",
        "camera.lensBlurUm",
        "camera.diffusion.active",
        "camera.diffusion.family",
        "camera.diffusion.strength",
        "camera.diffusion.spatialScale",
        "camera.diffusion.haloWarmth",
        "camera.diffusion.coreIntensity",
        "camera.diffusion.coreSize",
        "camera.diffusion.haloIntensity",
        "camera.diffusion.haloSize",
        "camera.diffusion.bloomIntensity",
        "camera.diffusion.bloomSize",
        "enlarger.illuminant",
        "enlarger.printExposure",
        "enlarger.printExposureCompensation",
        "enlarger.yFilterShift",
        "enlarger.mFilterShift",
        "enlarger.lensBlur",
        "enlarger.diffusion.active",
        "enlarger.diffusion.family",
        "enlarger.diffusion.strength",
        "enlarger.diffusion.spatialScale",
        "enlarger.diffusion.haloWarmth",
        "enlarger.diffusion.coreIntensity",
        "enlarger.diffusion.coreSize",
        "enlarger.diffusion.haloIntensity",
        "enlarger.diffusion.haloSize",
        "enlarger.diffusion.bloomIntensity",
        "enlarger.diffusion.bloomSize",
        "enlarger.preflashExposure",
        "enlarger.preflashYFilterShift",
        "enlarger.preflashMFilterShift",
        "scanner.lensBlur",
        "scanner.whiteCorrection",
        "scanner.whiteLevel",
        "scanner.blackCorrection",
        "scanner.blackLevel",
        "scanner.unsharpMask",
        "output.outputColorSpace",
        "output.savingCctfEncoding",
        "output.outputGamutCompress",
        "output.inputGamutCompress",
        "output.scanFilm",
        "grain.active",
        "grain.sublayersActive",
        "grain.particleAreaUm2",
        "grain.particleScale",
        "grain.particleScaleLayers",
        "grain.densityMin",
        "grain.uniformity",
        "grain.blur",
        "grain.blurDyeCloudsUm",
        "grain.microStructure",
        "grain.nSubLayers",
        "halation.active",
        "halation.scatterAmount",
        "halation.scatterSpatialScale",
        "halation.halationAmount",
        "halation.halationSpatialScale",
        "halation.boostEv",
        "halation.protectEv",
        "halation.boostRange",
        "halation.scatterCoreUm",
        "halation.scatterTailUm",
        "halation.scatterTailWeightPct",
        "halation.halationStrengthPct",
        "halation.firstSigmaUm",
        "halation.nBounces",
        "halation.bounceDecay",
        "halation.renormalize",
        "couplers.active",
        "couplers.amount",
        "couplers.inhibitionSamelayer",
        "couplers.inhibitionInterlayer",
        "couplers.gammaSamelayer",
        "couplers.gammaRtoGb",
        "couplers.gammaGtoRb",
        "couplers.gammaBtoRg",
        "couplers.diffusionSizeUm",
        "couplers.diffusionTailUm",
        "couplers.diffusionTailWeight",
        "glare.active",
        "glare.percent",
        "glare.roughness",
        "glare.blur",
        "experimental.filmGammaFactor",
        "experimental.printGammaFactor",
        "experimental.morphActive",
        "experimental.morphGammaFactor",
        "experimental.morphGammaFactorFast",
        "experimental.morphGammaFactorSlow",
        "experimental.morphGammaFactorRed",
        "experimental.morphGammaFactorGreen",
        "experimental.morphGammaFactorBlue",
        "experimental.morphDeveloperExhaustion",
        "toneCurve.active",
        "toneCurve.master",
        "toneCurve.red",
        "toneCurve.green",
        "toneCurve.blue",
    )

    private sealed interface PersistedSentinel {
        data class Text(val value: String) : PersistedSentinel
        data class Flag(val value: Boolean) : PersistedSentinel
        data class Number(val canonical: String) : PersistedSentinel
        data class Json(val canonical: String) : PersistedSentinel
    }

    private fun text(value: String) = PersistedSentinel.Text(value)
    private fun flag(value: Boolean) = PersistedSentinel.Flag(value)
    private fun number(value: String) = PersistedSentinel.Number(value)
    private fun json(value: String) = PersistedSentinel.Json(
        canonicalJson(
            value.trimStart().let { encoded ->
                if (encoded.startsWith("{")) JSONObject(encoded) else JSONArray(encoded)
            },
        ),
    )

    /**
     * Independent typed oracle: no value is read from [richParams] or from the preset encoder.
     * Each persisted property has one exact sentinel, including the complete mask document.
     */
    private val EXPECTED_PERSISTED_PARAMS_SENTINELS: Map<String, PersistedSentinel> = linkedMapOf(
        "filmProfile" to text("kodak_ektar_100"),
        "printProfile" to text("kodak_supra_endura"),
        "input.inputColorSpace" to text("ProPhoto RGB"),
        "input.inputCctfDecoding" to flag(true),
        "input.spectralUpsampling" to text("MALLETT2019"),
        "input.adaptationWindow" to flag(false),
        "input.adaptationSurface" to flag(true),
        "input.spectralGaussianBlur" to number("0.35"),
        "input.filterUv" to json("[0.1,405,7]"),
        "input.filterIr" to json("[0.2,680,14]"),
        "input.upscaleFactor" to number("1.25"),
        "input.crop" to flag(true),
        "input.cropCenter" to json("[0.43,0.57]"),
        "input.cropSize" to json("[0.62,0.38]"),
        "raw.whiteBalance" to text("CUSTOM"),
        "raw.temperature" to number("4875"),
        "raw.tint" to number("0.92"),
        "creativeWb.temp" to number("7"),
        "creativeWb.tint" to number("-4"),
        "creativeWb.balanceToFilmStock" to flag(true),
        "grade.contrast" to number("8"),
        "grade.saturation" to number("6"),
        "grade.vibrance" to number("4"),
        "grade.gamutCompress" to number("3"),
        "masks" to json(
            """
            {
              "schema":"org.spektrafilm.mask-set","version":1,"adjustments":[
                {"delta":{"temp":0,"texture":0,"tint":0,"blacks":0,"exposureEv":0,"saturation":0,"whites":0,"highlights":0,"clarity":0,"contrast":0,"hue":0,"shadows":0,"sharpness":0},"mask":{"components":[{"mode":"ADD","invert":false,"shape":{"angleDeg":0,"feather":0.5,"cx":0.25,"cy":0.5,"rx":0.18,"ry":0.18,"type":"radial"},"value":1}],"invert":false,"opacity":1}},
                {"delta":{"temp":0,"texture":0,"tint":0,"blacks":0,"exposureEv":0.25,"saturation":0,"whites":0,"highlights":0,"clarity":0,"contrast":0,"hue":0,"shadows":0,"sharpness":0},"mask":{"components":[{"mode":"ADD","invert":false,"shape":{"angleDeg":0,"feather":0.5,"cx":0.45,"cy":0.5,"rx":0.18,"ry":0.18,"type":"radial"},"value":1}],"invert":false,"opacity":1}},
                {"delta":{"temp":0,"texture":0,"tint":0,"blacks":0,"exposureEv":0.5,"saturation":0,"whites":0,"highlights":0,"clarity":0,"contrast":0,"hue":0,"shadows":0,"sharpness":0},"mask":{"components":[{"mode":"ADD","invert":false,"shape":{"angleDeg":0,"feather":0.5,"cx":0.65,"cy":0.5,"rx":0.18,"ry":0.18,"type":"radial"},"value":1}],"invert":false,"opacity":1}}
              ]
            }
            """.trimIndent(),
        ),
        "camera.exposureCompensationEv" to number("1.25"),
        "camera.autoExposure" to flag(false),
        "camera.autoExposureMethod" to text("matrix"),
        "camera.filmFormatMm" to number("33"),
        "camera.lensBlurUm" to number("0.15"),
        "camera.diffusion.active" to flag(false),
        "camera.diffusion.family" to text("glimmerglass"),
        "camera.diffusion.strength" to number("0.41"),
        "camera.diffusion.spatialScale" to number("1.1"),
        "camera.diffusion.haloWarmth" to number("0.05"),
        "camera.diffusion.coreIntensity" to number("0.91"),
        "camera.diffusion.coreSize" to number("0.85"),
        "camera.diffusion.haloIntensity" to number("0.82"),
        "camera.diffusion.haloSize" to number("0.75"),
        "camera.diffusion.bloomIntensity" to number("0.73"),
        "camera.diffusion.bloomSize" to number("1.2"),
        "enlarger.illuminant" to text("TH-KG3"),
        "enlarger.printExposure" to number("1.05"),
        "enlarger.printExposureCompensation" to flag(false),
        "enlarger.yFilterShift" to number("0.03"),
        "enlarger.mFilterShift" to number("-0.02"),
        "enlarger.lensBlur" to number("0.12"),
        "enlarger.diffusion.active" to flag(false),
        "enlarger.diffusion.family" to text("cinebloom"),
        "enlarger.diffusion.strength" to number("0.37"),
        "enlarger.diffusion.spatialScale" to number("0.9"),
        "enlarger.diffusion.haloWarmth" to number("-0.04"),
        "enlarger.diffusion.coreIntensity" to number("0.88"),
        "enlarger.diffusion.coreSize" to number("0.84"),
        "enlarger.diffusion.haloIntensity" to number("0.79"),
        "enlarger.diffusion.haloSize" to number("0.76"),
        "enlarger.diffusion.bloomIntensity" to number("0.71"),
        "enlarger.diffusion.bloomSize" to number("1.2"),
        "enlarger.preflashExposure" to number("0.04"),
        "enlarger.preflashYFilterShift" to number("-0.01"),
        "enlarger.preflashMFilterShift" to number("0.02"),
        "scanner.lensBlur" to number("0.08"),
        "scanner.whiteCorrection" to flag(true),
        "scanner.whiteLevel" to number("0.97"),
        "scanner.blackCorrection" to flag(true),
        "scanner.blackLevel" to number("0.015"),
        "scanner.unsharpMask" to json("[0.55,0.82]"),
        "output.outputColorSpace" to text("ADOBE_RGB"),
        "output.savingCctfEncoding" to flag(false),
        "output.outputGamutCompress" to text("ACES_RGC"),
        "output.inputGamutCompress" to text("XY"),
        "output.scanFilm" to flag(true),
        "grain.active" to flag(false),
        "grain.sublayersActive" to flag(false),
        "grain.particleAreaUm2" to number("0.3"),
        "grain.particleScale" to json("[0.9,1.1,1.9]"),
        "grain.particleScaleLayers" to json("[2.4,1.1,0.6]"),
        "grain.densityMin" to json("[0.08,0.09,0.13]"),
        "grain.uniformity" to json("[0.96,0.95,0.98]"),
        "grain.blur" to number("0.61"),
        "grain.blurDyeCloudsUm" to number("0.9"),
        "grain.microStructure" to json("[0.24,28]"),
        "grain.nSubLayers" to number("2"),
        "halation.active" to flag(false),
        "halation.scatterAmount" to number("0.83"),
        "halation.scatterSpatialScale" to number("0.91"),
        "halation.halationAmount" to number("0.77"),
        "halation.halationSpatialScale" to number("0.87"),
        "halation.boostEv" to number("0.12"),
        "halation.protectEv" to number("3.8"),
        "halation.boostRange" to number("0.27"),
        "halation.scatterCoreUm" to json("[2.1,1.9,1.5]"),
        "halation.scatterTailUm" to json("[9.2,9.6,9]"),
        "halation.scatterTailWeightPct" to json("[77,64,66]"),
        "halation.halationStrengthPct" to json("[4.9,1.4,0.1]"),
        "halation.firstSigmaUm" to json("[64,63,62]"),
        "halation.nBounces" to number("2"),
        "halation.bounceDecay" to number("0.47"),
        "halation.renormalize" to flag(false),
        "couplers.active" to flag(false),
        "couplers.amount" to number("0.81"),
        "couplers.inhibitionSamelayer" to number("0.91"),
        "couplers.inhibitionInterlayer" to number("0.89"),
        "couplers.gammaSamelayer" to json("[0.33,0.31,0.26]"),
        "couplers.gammaRtoGb" to json("[0.34,0.29]"),
        "couplers.gammaGtoRb" to json("[0.14,0.35]"),
        "couplers.gammaBtoRg" to json("[0.16,0.21]"),
        "couplers.diffusionSizeUm" to number("21"),
        "couplers.diffusionTailUm" to number("195"),
        "couplers.diffusionTailWeight" to number("0.055"),
        "glare.active" to flag(false),
        "glare.percent" to number("0.04"),
        "glare.roughness" to number("0.62"),
        "glare.blur" to number("0.45"),
        "experimental.filmGammaFactor" to number("1.03"),
        "experimental.printGammaFactor" to number("0.97"),
        "experimental.morphActive" to flag(false),
        "experimental.morphGammaFactor" to number("1.02"),
        "experimental.morphGammaFactorFast" to number("1.01"),
        "experimental.morphGammaFactorSlow" to number("0.99"),
        "experimental.morphGammaFactorRed" to number("1.01"),
        "experimental.morphGammaFactorGreen" to number("0.98"),
        "experimental.morphGammaFactorBlue" to number("1.02"),
        "experimental.morphDeveloperExhaustion" to number("0.03"),
        "toneCurve.active" to flag(true),
        "toneCurve.master" to json("[[0,0],[0.4,0.43],[1,1]]"),
        "toneCurve.red" to json("[[0,0],[1,0.98]]"),
        "toneCurve.green" to json("[[0,0.01],[1,1]]"),
        "toneCurve.blue" to json("[[0,0],[0.7,0.72],[1,1]]"),
    )

    private val armed = AtomicBoolean(false)
    private val commandSequence = AtomicLong(0L)
    private val commands = MutableStateFlow(NavigationRequest(0L, ""))
    private val hostGeneration = AtomicLong(0L)
    private val checkpointGeneration = AtomicLong(0L)
    private val currentDestination = AtomicReference("")
    private val sourceProbeSequence = AtomicLong(0L)
    private val sourceProbeCommands = Channel<SourceProbeRequest>(capacity = 4)
    private val currentProbeSource = AtomicReference("")
    private val sourceRetirementGeneration = AtomicLong(0L)
    private val exportProbeSequence = AtomicLong(0L)
    private val exportProbeCommands = Channel<ExportProbeRequest>(capacity = 2)
    private val exportProbeHandledGeneration = AtomicLong(0L)
    private val exportProbeSheetOpen = AtomicReference(false)
    private val editorInstanceSequence = AtomicLong(0L)
    private val activeEditorInstance = AtomicLong(0L)
    private val latestReadyEditorInstance = AtomicLong(0L)
    private val liveEditorReadyGeneration = AtomicLong(0L)
    private val liveEditorSnapshot = AtomicReference<EditorSessionDocument?>(null)
    private val expectedEditorOracle = AtomicReference<EditorSessionDocument?>(null)
    private val expectedOracleSeed = AtomicReference("")
    private val expectedExportRunId = AtomicLong(0L)
    private val livePreviewSource = AtomicReference("")
    private val livePreviewGeneration = AtomicLong(0L)
    private val sourceABaseline = AtomicReference<MemoryCurrentSignature?>(null)
    private val previewCompletionSequence = AtomicLong(0L)
    private val previewCompletionCommands = Channel<PreviewCompletionRequest>(capacity = 2)
    private val previewCompletionEnteredGeneration = AtomicLong(0L)
    private val previewCompletionCleanupGeneration = AtomicLong(0L)
    private val overlayDraftSequence = AtomicLong(0L)
    private val overlayDraftEvidence = AtomicReference<OverlayDraftEvidence?>(null)
    private val previewCompletionLock = Any()
    private var previewCompletionProbe: PreviewCompletionProbe? = null

    internal val navigationRequests: StateFlow<NavigationRequest> = commands.asStateFlow()
    internal val sourceProbeRequests: Flow<SourceProbeRequest> = sourceProbeCommands.receiveAsFlow()
    internal val exportProbeRequests: Flow<ExportProbeRequest> = exportProbeCommands.receiveAsFlow()
    internal val previewCompletionRequests: Flow<PreviewCompletionRequest> =
        previewCompletionCommands.receiveAsFlow()

    fun arm() {
        clearState()
        armed.set(true)
    }

    fun reset() {
        armed.set(false)
        clearState()
    }

    internal fun isArmed(): Boolean = armed.get()

    internal fun requireArmed() {
        check(armed.get()) { "ticket #139 probe is not armed" }
    }

    private fun clearState() {
        synchronized(previewCompletionLock) {
            previewCompletionProbe?.release?.complete(Unit)
            previewCompletionProbe = null
        }
        currentDestination.set("")
        currentProbeSource.set("")
        sourceRetirementGeneration.set(0L)
        exportProbeHandledGeneration.set(0L)
        exportProbeSheetOpen.set(false)
        checkpointGeneration.set(0L)
        editorInstanceSequence.set(0L)
        activeEditorInstance.set(0L)
        latestReadyEditorInstance.set(0L)
        liveEditorReadyGeneration.set(0L)
        liveEditorSnapshot.set(null)
        expectedEditorOracle.set(null)
        expectedOracleSeed.set("")
        expectedExportRunId.set(0L)
        livePreviewSource.set("")
        livePreviewGeneration.set(0L)
        sourceABaseline.set(null)
        previewCompletionEnteredGeneration.set(0L)
        previewCompletionCleanupGeneration.set(0L)
        overlayDraftEvidence.set(null)
        while (sourceProbeCommands.tryReceive().isSuccess) Unit
        while (exportProbeCommands.tryReceive().isSuccess) Unit
        while (previewCompletionCommands.tryReceive().isSuccess) Unit
        commands.value = NavigationRequest(commandSequence.incrementAndGet(), "")
    }

    @JvmStatic
    fun requestDestination(destination: String): Long {
        require(destination in Screen.entries.map(Screen::name)) { "unsupported destination" }
        val sequence = commandSequence.incrementAndGet()
        commands.value = NavigationRequest(sequence, destination)
        return sequence
    }

    @JvmStatic
    fun requestSourceProbe(label: String): Long {
        require(label == "A" || label == "B") { "unsupported source probe" }
        val sequence = sourceProbeSequence.incrementAndGet()
        check(sourceProbeCommands.trySend(SourceProbeRequest(sequence, label)).isSuccess) {
            "source probe command buffer is full"
        }
        return sequence
    }

    @JvmStatic
    fun requestExportProbe(): Long {
        val sequence = exportProbeSequence.incrementAndGet()
        check(exportProbeCommands.trySend(ExportProbeRequest(sequence)).isSuccess) {
            "export probe command buffer is full"
        }
        return sequence
    }

    @JvmStatic fun currentDestination(): String = currentDestination.get()
    @JvmStatic fun hostGeneration(): Long = hostGeneration.get()
    @JvmStatic fun checkpointGeneration(): Long = checkpointGeneration.get()
    @JvmStatic fun currentProbeSource(): String = currentProbeSource.get()
    @JvmStatic fun sourceRetirementGeneration(): Long = sourceRetirementGeneration.get()
    @JvmStatic fun exportProbeHandledGeneration(): Long = exportProbeHandledGeneration.get()
    @JvmStatic fun liveEditorReadyGeneration(): Long = liveEditorReadyGeneration.get()
    @JvmStatic fun livePreviewGeneration(): Long = livePreviewGeneration.get()
    @JvmStatic fun currentLivePreviewSource(): String = livePreviewSource.get()
    @JvmStatic fun previewCompletionEnteredGeneration(): Long =
        previewCompletionEnteredGeneration.get()
    @JvmStatic fun previewCompletionCleanupGeneration(): Long =
        previewCompletionCleanupGeneration.get()
    @JvmStatic fun overlayDraftGeneration(): Long = overlayDraftEvidence.get()?.generation ?: 0L
    @JvmStatic fun currentOverlayDraftTool(): String = overlayDraftEvidence.get()?.tool?.name ?: ""

    /** Bind a real process-owned export to the immutable Activity-recreation oracle. */
    @JvmStatic
    fun expectActivityExportRun(runId: Long) {
        require(runId > 0L) { "invalid Activity export run id" }
        expectedExportRunId.set(runId)
    }

    /**
     * Called once by the real CropOverlay instance after its remembered local draft exists. This is
     * inert outside the fixed #139 oracle and independently verifies that the draft equals the
     * committed crop passed into the Composable.
     */
    internal fun publishCropDraftInitialized(
        bitmapWidth: Int,
        bitmapHeight: Int,
        initialCrop: Boolean,
        initialCenter: Pair<Float, Float>,
        initialSize: Pair<Float, Float>,
        actualLeft: Float,
        actualTop: Float,
        actualRight: Float,
        actualBottom: Float,
    ) {
        if (!armed.get()) return
        if (expectedEditorOracle.get() == null) return
        require(bitmapWidth > 0 && bitmapHeight > 0) { "crop draft has no rendered preview" }
        val imgW = bitmapWidth.toFloat()
        val imgH = bitmapHeight.toFloat()
        val maxDim = maxOf(imgW, imgH)
        val expected = if (initialCrop) {
            val halfW = (initialSize.first * maxDim / imgW) / 2f
            val halfH = (initialSize.second * maxDim / imgH) / 2f
            floatArrayOf(
                (initialCenter.first - halfW).coerceIn(0f, 1f),
                (initialCenter.second - halfH).coerceIn(0f, 1f),
                (initialCenter.first + halfW).coerceIn(0f, 1f),
                (initialCenter.second + halfH).coerceIn(0f, 1f),
            )
        } else {
            floatArrayOf(0f, 0f, 1f, 1f)
        }
        val actual = floatArrayOf(actualLeft, actualTop, actualRight, actualBottom)
        check(actual.indices.all { kotlin.math.abs(actual[it] - expected[it]) <= 1e-6f }) {
            "restored crop overlay did not initialize a clean committed draft"
        }
        publishOverlayDraft(EditorOverlayTool.CROP, bitmapWidth, bitmapHeight)
    }

    /** Same real-composition proof for the mask geometry editor's remembered local draft. */
    internal fun publishMaskDraftInitialized(
        bitmapWidth: Int,
        bitmapHeight: Int,
        committed: MaskComponent,
        actual: MaskComponent,
    ) {
        if (!armed.get()) return
        if (expectedEditorOracle.get() == null) return
        require(bitmapWidth > 0 && bitmapHeight > 0) { "mask draft has no rendered preview" }
        check(actual == committed) {
            "restored mask overlay did not initialize a clean committed draft"
        }
        publishOverlayDraft(EditorOverlayTool.MASK_GEOMETRY, bitmapWidth, bitmapHeight)
    }

    @JvmStatic
    fun verifyOverlayDraft(generation: Long, expectedTool: String) {
        val evidence = checkNotNull(overlayDraftEvidence.get()) { "overlay draft never initialized" }
        check(evidence.generation == generation) { "overlay draft generation changed before verification" }
        check(evidence.tool == EditorOverlayTool.valueOf(expectedTool)) {
            "overlay draft initialized ${evidence.tool}, expected $expectedTool"
        }
        check(evidence.bitmapWidth > 0 && evidence.bitmapHeight > 0) {
            "overlay draft was not backed by a rendered preview"
        }
    }

    private fun publishOverlayDraft(tool: EditorOverlayTool, bitmapWidth: Int, bitmapHeight: Int) {
        val generation = overlayDraftSequence.incrementAndGet()
        overlayDraftEvidence.set(
            OverlayDraftEvidence(generation, tool, bitmapWidth, bitmapHeight),
        )
    }

    @JvmStatic
    fun prepareActivityProbe(context: Context) {
        val app = context.applicationContext
        requireArmed()
        verifyTransientDurabilityRecoveryContract(app)
        clearDurableSource(app)
        AppSettings.from(app).apply {
            seenOnboarding = true
            seenEditorCoach = true
            gpuPreview = false
        }
        // A previously finishing Activity can still run its final Compose checkpoint callback.
        // Fence it first, then drain everything accepted before installing the immutable oracle.
        EditorSessionCheckpointRuntime.retireOwners()
        EditorSessionCheckpointRuntime.flush()
        val fixture = activityFixtureOracle()
        expectedEditorOracle.set(fixture)
        EditorSessionStore.write(app, fixture)
    }

    /** Runs on the physical release path, but only after the explicit default-off probe arm. */
    private fun verifyTransientDurabilityRecoveryContract(context: Context) = runBlocking {
        check(
            editorSessionWriteAccess(EditorSessionReadResult.Unavailable("injected")) ==
                EditorSessionWriteAccess.RECOVERING,
        )
        check(editorSessionWritesProtected(EditorSessionReadResult.Unsupported("injected")))
        val recoveryReads = AtomicInteger(0)
        val recovered = awaitEditorSessionWriteRecovery(
            read = {
                if (recoveryReads.incrementAndGet() == 1) {
                    EditorSessionReadResult.Unavailable("injected fail once")
                } else {
                    EditorSessionReadResult.Missing
                }
            },
            onUnavailable = { },
        )
        check(recoveryReads.get() == 2)
        check(editorSessionWriteAccess(recovered) == EditorSessionWriteAccess.WRITABLE)
        val attempts = AtomicInteger(0)
        val retries = AtomicInteger(0)
        check(
            awaitDurableEditorSessionCheckpoint(
                checkpoint = { true },
                flush = {
                    if (attempts.incrementAndGet() == 1) throw IOException("injected fail once")
                },
                onRetryableFailure = { retries.incrementAndGet() },
            ),
        )
        check(attempts.get() == 2 && retries.get() == 1) {
            "transient editor-session durability did not retry exactly once"
        }
        verifyRecipeClassificationContract(context)
        verifyRecoveryConflictContract()
        verifyDemoTombstoneContract(context)
        verifySourceBoundExportContract(context)
    }

    private fun verifyRecipeClassificationContract(context: Context) {
        val futureKey = "1391391391391391391391391391391391391391391391391391391391391391"
        val loadedKey = "2392392392392392392392392392392392392392392392392392392392392392"
        try {
            Recipes.delete(context, futureKey)
            Recipes.saveJson(
                context,
                futureKey,
                Presets.toJsonString(richParams(1.25f)),
                "future.dng",
                expectedGeneration = Recipes.generation(futureKey),
            )
            val futureFile = File(File(context.filesDir, "recipes"), "$futureKey.json")
            val futureRoot = AtomicJsonStore.parseObject(
                AtomicJsonStore.readUtf8(futureFile, AtomicJsonStore.MAX_RECIPE_BYTES),
            ).put("recipeVersion", RecipeDocumentCodec.SCHEMA_VERSION + 100)
            AtomicJsonStore.writeUtf8(
                futureFile,
                futureRoot.toString(),
                AtomicJsonStore.MAX_RECIPE_BYTES,
            )
            val protectedBytes = futureFile.readBytes()
            repeat(2) {
                val before = Recipes.generation(futureKey)
                val read = Recipes.readResult(context, futureKey)
                val access = classifyEditorRecipeAccess(
                    futureKey,
                    before,
                    Recipes.generation(futureKey),
                    read,
                )
                check(read is RecipeReadResult.Unsupported)
                check(access is EditorRecipeAccess.Protected)
                check(access.writableGenerationFor(futureKey) == null)
                check(futureFile.readBytes().contentEquals(protectedBytes)) {
                    "future recipe bytes changed across recreate classification"
                }
            }
            val ioProtected = classifyEditorRecipeAccess(
                futureKey,
                Recipes.generation(futureKey),
                Recipes.generation(futureKey),
                RecipeReadResult.IoFailure("injected"),
            )
            check(ioProtected is EditorRecipeAccess.Protected)

            Recipes.delete(context, loadedKey)
            val richJson = Presets.toJsonString(richParams(1.25f))
            Recipes.saveJson(
                context,
                loadedKey,
                richJson,
                "source-b.dng",
                expectedGeneration = Recipes.generation(loadedKey),
            )
            val pending: EditorRecipeAccess = EditorRecipeAccess.Pending(
                loadedKey,
                Recipes.generation(loadedKey),
            )
            check(pending.writableGenerationFor(loadedKey) == null)
            val before = Recipes.generation(loadedKey)
            val loaded = Recipes.readResult(context, loadedKey)
            val writable = classifyEditorRecipeAccess(
                loadedKey,
                before,
                Recipes.generation(loadedKey),
                loaded,
            )
            check(loaded is RecipeReadResult.Loaded)
            check(writable.writableGenerationFor(loadedKey) == before)
            check(loaded.document.params.toString() == AtomicJsonStore.parseObject(richJson).toString())
        } finally {
            runCatching { Recipes.delete(context, futureKey) }
            runCatching { Recipes.delete(context, loadedKey) }
        }
    }

    private fun verifyRecoveryConflictContract() {
        val recovered = richDocument(durableSource = false, runningExport = false)
        val live = recovered.copy(
            current = recovered.current.copy(paramsJson = Presets.toJsonString(richParams(2f))),
        )
        val durableWins = resolveLoadedEditorSessionRecovery(
            recovered,
            SourceRestoreResult.Demo,
            EditorSavedFallback.Empty,
            live,
            liveMutated = false,
        )
        check(durableWins.policy == EditorRecoveryConflictPolicy.RECOVERED_DURABLE)
        check(durableWins.restoration.document == recovered)
        val liveWins = resolveLoadedEditorSessionRecovery(
            recovered,
            SourceRestoreResult.Demo,
            EditorSavedFallback.Empty,
            live,
            liveMutated = true,
        )
        check(liveWins.policy == EditorRecoveryConflictPolicy.LIVE_MUTATION)
        check(liveWins.restoration.document == live)
    }

    private fun verifyDemoTombstoneContract(context: Context) {
        val coordinator = sourceAccessRuntime(context).coordinator
        coordinator.selectDemo()
        val restored = coordinator.restore()
        check(restored == SourceRestoreResult.Demo)
        val staleUriSession = richDocument(durableSource = true, runningExport = false)
        val reconciled = reconcileEditorRestoration(
            staleUriSession,
            restored,
            EditorSavedFallback.Empty,
        )
        check(reconciled.source.kind == SourceKind.DEMO && reconciled.document == null) {
            "demo tombstone resurrected a stale URI session"
        }
    }

    private suspend fun verifySourceBoundExportContract(context: Context) {
        suspend fun runMismatch(revoke: Boolean) {
            check(ExportWorkRuntime.state.value is ExportRuntimeState.Idle)
            val uri = if (revoke) "content://ticket139/revoke" else "content://ticket139/a"
            val sourceA = ExportSourceIdentityAuthority.bind(uri, SourceKind.PHOTO, false)
            val bitmap = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888)
            val runId = withTimeout(5_000) {
                var accepted: Long?
                do {
                    accepted = ExportWorkRuntime.launch(
                        context,
                        ExportFormat.PNG16,
                        System.currentTimeMillis(),
                        sourceA,
                    ) {
                        ExportTerminalOutcome.Success(
                            ExportFormat.PNG16,
                            139L,
                            bitmap,
                            1L,
                            ExportPhaseSnapshot(0, 0, 0, 0, 0, 0),
                            publishedUri = null,
                            publishedMimeType = null,
                        )
                    }
                    if (accepted == null) delay(1)
                } while (accepted == null)
                requireNotNull(accepted)
            }
            val replacement = ExportSourceIdentityAuthority.bind(
                if (revoke) uri else "content://ticket139/b",
                SourceKind.PHOTO,
                authorizationRequired = revoke,
            )
            val finished = withTimeout(5_000) {
                ExportWorkRuntime.state.filter {
                    it is ExportRuntimeState.Finished && it.runId == runId
                }.first() as ExportRuntimeState.Finished
            }
            check(!exportPublicationAuthorized(finished.sourceIdentity, replacement))
            val claims = coroutineScope {
                (0 until 16).map {
                    async(Dispatchers.Default) { ExportWorkRuntime.claimFinished(runId) }
                }.awaitAll()
            }
            check(claims.count { it != null } == 1) { "mismatched export was not claimed once" }
            claims.filterNotNull().forEach(::recycleUnpublishedExport)
            check(bitmap.isRecycled) { "mismatched export bitmap was not recycled" }
            check(ExportWorkRuntime.claimFinished(runId) == null)
        }
        runMismatch(revoke = false)
        runMismatch(revoke = true)
    }

    @JvmStatic
    fun prepareAuthorizedMaskActivityProbe(context: Context) {
        val app = context.applicationContext
        requireArmed()
        clearDurableSource(app)
        AppSettings.from(app).apply {
            seenOnboarding = true
            seenEditorCoach = true
            gpuPreview = false
        }
        EditorSessionCheckpointRuntime.retireOwners()
        EditorSessionCheckpointRuntime.flush()
        val fixture = activityFixtureOracle().copy(
            tool = EditorToolState(
                Category.MASKS,
                EditorOverlayTool.MASK_GEOMETRY,
                EXPECTED_MASK_INDEX,
            ),
        )
        check(!fixture.source.authorizationRequired) { "mask fixture must be render-authorized" }
        expectedEditorOracle.set(fixture)
        EditorSessionStore.write(app, fixture)
    }

    @JvmStatic
    fun verifyActivityCursor(context: Context, expectedExportPhase: String) {
        EditorSessionCheckpointRuntime.flush()
        val document = requireLoaded(context)
        verifyAgainstExpectedOracle(document, expectedExportPhase)
        verifyRichCursor(document)
        check(document.source.kind == SourceKind.DEMO) { "activity probe source changed" }
        check(!document.source.authorizationRequired) { "demo unexpectedly needs authorization" }
        val expected = EditorExportPhase.valueOf(expectedExportPhase)
        check(document.export.phase == expected) {
            "activity export cursor was ${document.export.phase}, expected $expected"
        }
        if (expected != EditorExportPhase.RUNNING) {
            check(document.export.runtimeRunId == null) {
                "terminal activity export retained a runtime generation"
            }
        }
    }

    @JvmStatic
    fun prepareProcessDeathProbe(context: Context) {
        val app = context.applicationContext
        requireArmed()
        clearDurableSource(app)
        AppSettings.from(app).apply {
            seenOnboarding = true
            seenEditorCoach = true
            gpuPreview = false
        }
        EditorSessionCheckpointRuntime.retireOwners()
        EditorSessionCheckpointRuntime.flush()
        val seeded = richDocument(durableSource = true, runningExport = false)
        val expected = seeded.copy(
            source = seeded.source.copy(authorizationRequired = true),
        )
        val seedIdentity = UUID.randomUUID().toString()
        writeProcessOracle(app, seedIdentity, expected)
        expectedEditorOracle.set(expected)
        expectedOracleSeed.set(seedIdentity)
        EditorSessionStore.write(app, seeded)
    }

    /** Real process-owned RUNNING state that the host kills between seed and recovery phases. */
    @JvmStatic
    fun beginProcessDeathExportProbe(context: Context): Long {
        // Launch as the seeded editor's exact current source binding, never the legacy UNBOUND
        // ABI: the source-fenced UI refuses to track an unbound run, and this probe must appear
        // in the durable cursor as the editor's own in-flight export (ticket #139).
        val sourceIdentity = ExportSourceIdentityAuthority.current()
        check(sourceIdentity != ExportSourceIdentity.UNBOUND) {
            "process-death export probe requires a live editor source binding"
        }
        val runId = requireNotNull(
            ExportWorkRuntime.launch(
                context = context.applicationContext,
                format = ExportFormat.PNG16,
                startedAtMillis = System.currentTimeMillis(),
                sourceIdentity = sourceIdentity,
            ) {
                awaitCancellation()
            },
        ) { "process-death export probe could not start" }
        expectedExportRunId.set(runId)
        return runId
    }

    @JvmStatic
    fun verifyProcessDeathSeedAfterLaunch(context: Context, runId: Long): String {
        val app = context.applicationContext
        EditorSessionCheckpointRuntime.flush()
        val saved = requireLoaded(app)
        check(expectedExportRunId.get() == runId) { "process export oracle has the wrong run id" }
        verifyAgainstExpectedOracle(saved, EditorExportPhase.RUNNING.name)
        verifyRichCursor(saved)
        check(saved.source.kind == SourceKind.RAW) { "seeded editor changed source kind" }
        check(saved.source.authorizationRequired) { "seeded revoked source was not gated" }
        check(saved.export.phase == EditorExportPhase.RUNNING) {
            "seeded editor did not checkpoint RUNNING"
        }
        check(saved.export.runtimeRunId == runId) { "seeded editor checkpointed the wrong export" }
        val payload = JSONObject()
            .put("schema", "org.spektrafilm.ticket139-process-probe")
            .put("version", 1)
            .put("pid", Process.myPid())
            .put("rotation", EXPECTED_ROTATION)
            .put("maskIndex", EXPECTED_MASK_INDEX)
            .put("runId", runId)
            .put("oracleSeed", expectedOracleSeed.get())
            .put("buildIdentity", buildIdentity(app))
        AtomicJsonStore.writeUtf8(processMarker(app), payload.toString(), 4 * 1024)
        return "pid=${Process.myPid()} cursor=$EXPECTED_ROTATION/$EXPECTED_MASK_INDEX " +
            "completeOracle=true tool=MASK_GEOMETRY"
    }

    @JvmStatic
    fun abortProcessDeathExportProbe(runId: Long) {
        ExportWorkRuntime.cancel(runId)
    }

    @JvmStatic
    fun verifyProcessRecoveryBeforeLaunch(context: Context): String {
        val app = context.applicationContext
        val payload = AtomicJsonStore.parseObject(
            AtomicJsonStore.readUtf8(processMarker(app), 4 * 1024),
        )
        check(payload.getString("schema") == "org.spektrafilm.ticket139-process-probe")
        check(payload.getInt("version") == 1)
        check(payload.getString("buildIdentity") == buildIdentity(app)) {
            "ticket #139 process marker belongs to another build"
        }
        val oracleSeed = payload.getString("oracleSeed")
        val oracle = readProcessOracle(app, oracleSeed)
        expectedEditorOracle.set(oracle)
        expectedOracleSeed.set(oracleSeed)
        expectedExportRunId.set(payload.getLong("runId"))
        val seedPid = payload.getInt("pid")
        check(seedPid != Process.myPid()) {
            "ticket #139 recovery did not start in a new process"
        }
        val saved = requireLoaded(app)
        verifyCompleteEditorCursor(
            saved,
            oracle,
            EditorExportPhase.RUNNING,
            expectedRuntimeRunId = payload.getLong("runId"),
        )
        verifyRichCursor(saved)
        check(saved.export.phase == EditorExportPhase.RUNNING)
        check(saved.export.runtimeRunId == payload.getLong("runId"))
        val reconciled = reconcileEditorRestoration(
            saved,
            SourceRestoreResult.None,
            EditorSavedFallback.Empty,
        )
        check(reconciled.document != null) { "revoked session cursor was discarded" }
        check(reconciled.source.kind == SourceKind.RAW) { "revoked RAW changed source kind" }
        check(reconciled.source.authorizationRequired) { "revoked source was not gated" }
        check(reconciled.source.kind != SourceKind.DEMO) { "revoked source silently became demo" }
        val export = reconcileRestoredExport(saved.export, activeRuntimeRunId = null)
        check(export.phase == EditorExportPhase.RECONCILING)
        check(export.runtimeRunId == null)
        return "seedPid=$seedPid recoverPid=${Process.myPid()} completeOracle=true"
    }

    @JvmStatic
    fun verifyProcessRecoveryAfterLaunch(context: Context) {
        EditorSessionCheckpointRuntime.flush()
        val restored = requireLoaded(context)
        verifyAgainstExpectedOracle(restored, EditorExportPhase.RECONCILING.name)
        verifyRichCursor(restored)
        check(restored.source.kind == SourceKind.RAW) { "restored source kind changed" }
        check(restored.source.authorizationRequired) { "restored source was rendered without grant" }
        check(restored.export.phase == EditorExportPhase.RECONCILING) {
            "orphaned export did not reconcile"
        }
        check(restored.export.runtimeRunId == null) { "orphaned export retained a run id" }
        check(AtomicJsonStore.delete(processMarker(context.applicationContext))) {
            "ticket #139 process marker cleanup failed"
        }
        check(AtomicJsonStore.delete(processOracle(context.applicationContext))) {
            "ticket #139 process oracle cleanup failed"
        }
        reset()
    }

    @JvmStatic
    fun verifyLiveEditorCursor(
        context: Context,
        expectedFixture: String,
        expectedExportPhase: String,
    ) {
        EditorSessionCheckpointRuntime.flush()
        val live = checkNotNull(liveEditorSnapshot.get()) { "live editor cursor was never published" }
        val expectedPhase = EditorExportPhase.valueOf(expectedExportPhase)
        if (expectedFixture == "ACTIVITY" || expectedFixture == "PROCESS") {
            // The pre-launch oracle is the first authority consulted. A restored editor is never
            // allowed to checkpoint itself and then use that same disk value as its sole oracle.
            verifyAgainstExpectedOracle(live, expectedExportPhase)
        }
        val persisted = requireLoaded(context.applicationContext)
        check(live == persisted) { "live editor cursor differs from its durable checkpoint" }
        check(EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(live)) == live) {
            "live editor cursor is not a complete round-trippable session"
        }
        check(live.export.phase == expectedPhase) {
            "live export cursor was ${live.export.phase}, expected $expectedPhase"
        }
        when (expectedFixture) {
            "ACTIVITY" -> {
                verifyRichCursor(live)
                check(live.source.kind == SourceKind.DEMO)
                check(!live.source.authorizationRequired)
            }
            "PROCESS" -> {
                verifyRichCursor(live)
                check(live.source.kind == SourceKind.RAW)
                check(live.source.authorizationRequired)
                check(live.source.kind != SourceKind.DEMO)
            }
            "SOURCE_B" -> {
                check(live.source.uri == sourceProbeUri("B"))
                check(live.source.kind == SourceKind.PHOTO)
                check(!live.source.authorizationRequired)
                check(live.history.undo.isEmpty() && live.history.redo.isEmpty())
                check(live.tool.maskIndex in 0 until com.spectrafilm.app.masks.MaskJson.MAX_ADJUSTMENTS)
            }
            else -> error("unsupported live editor fixture")
        }
    }

    /**
     * Force one real settle render through EditorScreen, then pause its completed source-A native
     * result immediately before the production publication gate. No synthetic gate is involved.
     */
    @JvmStatic
    fun armCompletedPreviewProbe(): Long {
        check(currentProbeSource.get() == "A" && livePreviewSource.get() == "A") {
            "source A must be live before arming the completed-preview probe"
        }
        val baseline = checkNotNull(sourceABaseline.get()) {
            "source-A live admission baseline is missing"
        }
        val sequence = previewCompletionSequence.incrementAndGet()
        synchronized(previewCompletionLock) {
            check(previewCompletionProbe == null) { "completed-preview probe is already armed" }
            previewCompletionProbe = PreviewCompletionProbe(
                sequence = sequence,
                baseline = baseline,
                release = CompletableDeferred(),
            )
        }
        if (previewCompletionCommands.trySend(PreviewCompletionRequest(sequence)).isFailure) {
            synchronized(previewCompletionLock) {
                previewCompletionProbe?.takeIf { it.sequence == sequence }?.release?.complete(Unit)
                previewCompletionProbe = null
            }
            error("completed-preview command buffer is full")
        }
        return sequence
    }

    @JvmStatic
    fun releaseCompletedPreviewProbe(sequence: Long) {
        val release = synchronized(previewCompletionLock) {
            previewCompletionProbe?.takeIf { it.sequence == sequence }?.release
        }
        checkNotNull(release) { "completed-preview probe is not armed" }.complete(Unit)
    }

    @JvmStatic
    fun verifyCompletedPreviewProbe(sequence: Long) {
        val probe = synchronized(previewCompletionLock) {
            checkNotNull(previewCompletionProbe?.takeIf { it.sequence == sequence }) {
                "completed-preview probe is missing"
            }.copy()
        }
        val entered = checkNotNull(probe.entered) { "source-A real render never reached the barrier" }
        val bPublished = checkNotNull(probe.sourceBPublished) {
            "source B did not publish while source A was held"
        }
        val cleanup = checkNotNull(probe.cleanup) { "source-A cleanup did not complete" }
        check(probe.stalePublicationRejected) { "stale source-A publication was not rejected" }
        check(probe.ownerCancellationObserved) {
            "source B did not cancel the held source-A settle coroutine"
        }
        check(entered.totalBytes > probe.baseline.totalBytes) {
            "source-A barrier did not hold a live native result/admission"
        }
        check(bPublished.totalBytes > probe.baseline.totalBytes) {
            "source B was not live concurrently with the held source-A result"
        }
        check(cleanup == probe.baseline) {
            "source-A cleanup did not return every native/cache admission to the B-held baseline"
        }
        check(livePreviewSource.get() == "B") { "source B is not the published preview" }
    }

    internal fun onHostCreated() {
        if (!armed.get()) return
        hostGeneration.incrementAndGet()
    }

    internal fun beginLiveEditorInstance(): Long {
        if (!armed.get()) return 0L
        return editorInstanceSequence.incrementAndGet().also { activeEditorInstance.set(it) }
    }

    internal fun publishLiveEditorSnapshot(instance: Long, document: EditorSessionDocument) {
        if (!armed.get() || instance == 0L) return
        if (instance == activeEditorInstance.get()) liveEditorSnapshot.set(document)
    }

    internal fun publishLiveEditorReady(instance: Long, document: EditorSessionDocument) {
        if (!armed.get() || instance == 0L) return
        if (instance != activeEditorInstance.get()) return
        publishLiveEditorSnapshot(instance, document)
        while (true) {
            val previous = latestReadyEditorInstance.get()
            if (instance <= previous) return
            if (latestReadyEditorInstance.compareAndSet(previous, instance)) {
                liveEditorReadyGeneration.incrementAndGet()
                return
            }
        }
    }

    internal fun publishDestination(destination: Screen) {
        if (!armed.get()) return
        currentDestination.set(destination.name)
    }

    internal fun publishCheckpoint() {
        if (!armed.get()) return
        checkpointGeneration.incrementAndGet()
    }

    internal fun publishProbeSource(label: String) {
        if (!armed.get()) return
        currentProbeSource.set(label)
    }

    internal fun publishSourceRetirement() {
        if (!armed.get()) return
        sourceRetirementGeneration.incrementAndGet()
    }

    internal suspend fun awaitCompletedPreviewBeforePublication(label: String): Boolean {
        if (!armed.get()) return false
        if (label != "A") return false
        val signature = currentMemorySignature()
        val ownerJob = currentCoroutineContext()[Job]
        val release = synchronized(previewCompletionLock) {
            val probe = previewCompletionProbe ?: return false
            if (probe.entered != null) return false
            probe.entered = signature
            previewCompletionEnteredGeneration.set(probe.sequence)
            probe.release
        }
        // Source B cancels the old LaunchedEffect by design. Keep this one test-owned result alive
        // until the host releases it so production invalidation/claim/cleanup are actually tested.
        withContext(NonCancellable) {
            withTimeout(PREVIEW_COMPLETION_TIMEOUT_MILLIS) { release.await() }
            // Record this before returning to the cancelled owner context. Returning from the
            // outer Dispatchers.Default boundary has prompt-cancellation semantics, so evidence
            // collected after that boundary could itself be discarded.
            synchronized(previewCompletionLock) {
                previewCompletionProbe
                    ?.takeIf { it.release === release }
                    ?.ownerCancellationObserved = ownerJob?.isCancelled == true
            }
        }
        return true
    }

    internal fun publishLivePreview(label: String) {
        if (!armed.get()) return
        if (label != "A" && label != "B") return
        livePreviewSource.set(label)
        livePreviewGeneration.incrementAndGet()
        val signature = currentMemorySignature()
        if (label == "A") {
            sourceABaseline.set(signature)
        } else {
            synchronized(previewCompletionLock) {
                previewCompletionProbe
                    ?.takeIf { it.entered != null }
                    ?.sourceBPublished = signature
            }
        }
    }

    internal fun publishPreviewDecision(label: String, claimed: Boolean) {
        if (!armed.get()) return
        if (label != "A") return
        synchronized(previewCompletionLock) {
            previewCompletionProbe
                ?.takeIf { it.entered != null }
                ?.stalePublicationRejected = !claimed
        }
    }

    internal fun publishPreviewCleanup(label: String) {
        if (!armed.get()) return
        if (label != "A") return
        val signature = currentMemorySignature()
        synchronized(previewCompletionLock) {
            previewCompletionProbe?.takeIf { it.entered != null }?.let { probe ->
                probe.cleanup = signature
                previewCompletionCleanupGeneration.set(probe.sequence)
            }
        }
    }

    internal fun publishExportProbeResult(sequence: Long, sheetOpen: Boolean) {
        if (!armed.get()) return
        exportProbeSheetOpen.set(sheetOpen)
        exportProbeHandledGeneration.updateAndGet { handled -> maxOf(handled, sequence) }
    }

    @JvmStatic
    fun verifyRevokedExportProbe(sequence: Long) {
        check(exportProbeHandledGeneration.get() >= sequence) { "export probe was not handled" }
        check(!exportProbeSheetOpen.get()) { "revoked source opened the export sheet" }
    }

    internal fun sourceProbeUri(label: String): String {
        requireArmed()
        require(label == "A" || label == "B") { "unsupported source probe" }
        return "content://com.spectrafilm.app.test.ticket139.sources/source/${label.lowercase()}.png"
    }

    internal fun sourceProbeLabel(uri: String?): String {
        if (!armed.get()) return ""
        return when (uri) {
            sourceProbeUri("A") -> "A"
            sourceProbeUri("B") -> "B"
            else -> ""
        }
    }

    @JvmStatic
    fun verifySourceProbeAfterSwitch(context: Context, expectedLabel: String, retirementBefore: Long) {
        val expectedUri = sourceProbeUri(expectedLabel)
        EditorSessionCheckpointRuntime.flush()
        val restored = requireLoaded(context)
        check(restored.source.uri == expectedUri) { "source probe checkpoint has the wrong identity" }
        check(restored.source.kind == SourceKind.PHOTO) { "source probe checkpoint changed kind" }
        check(!restored.source.authorizationRequired) { "replacement source was not authorized" }
        val durable = sourceAccessRuntime(context.applicationContext).coordinator.restore()
        check(durable is SourceRestoreResult.Ready && durable.ref.uri == expectedUri) {
            "replacement source was not the durable SourceAccess authority"
        }
        check(currentProbeSource.get() == expectedLabel) { "replacement source did not reach editor state" }
        check(sourceRetirementGeneration.get() > retirementBefore) {
            "replacement source did not retire prior render resources"
        }
    }

    private fun currentMemorySignature(): MemoryCurrentSignature {
        val root = JSONObject(SpektraEngine.memoryBudgetSnapshotJson())
        fun currentBytes(arrayName: String): List<Long> {
            val values = root.getJSONArray(arrayName)
            return List(values.length()) { index ->
                values.getJSONObject(index).getLong("current_bytes")
            }
        }
        return MemoryCurrentSignature(
            totalBytes = root.getJSONObject("total").getLong("current_bytes"),
            domainBytes = currentBytes("domains"),
            stageBytes = currentBytes("stages"),
        )
    }

    private const val PREVIEW_COMPLETION_TIMEOUT_MILLIS = 90_000L

    private fun clearDurableSource(context: Context) = runBlocking {
        val runtime = sourceAccessRuntime(context)
        val generation = runtime.mutations.begin()
        check(runtime.submit(generation) { runtime.coordinator.clear() }.await() != null) {
            "source clear was superseded"
        }
    }

    private fun richDocument(
        durableSource: Boolean,
        runningExport: Boolean,
    ): EditorSessionDocument {
        val baseline = richParams(exposure = -0.5f)
        val current = richParams(exposure = 1.25f)
        val forward = richParams(exposure = 2f)
        val currentSnapshot = EditSnapshot(Presets.toJsonString(current), EXPECTED_ROTATION)
        val document = EditorSessionDocument(
            source = if (durableSource) {
                EditorSourceState(
                    uri = "content://com.spectrafilm.ticket139/revoked/source.dng",
                    kind = SourceKind.RAW,
                    displayName = "ticket139-source.dng",
                    authorizationRequired = false,
                )
            } else {
                EditorSourceState(null, SourceKind.DEMO, "synthetic demo image", false)
            },
            current = currentSnapshot,
            committed = currentSnapshot,
            history = EditHistoryState(
                undo = listOf(EditSnapshot(Presets.toJsonString(baseline), 90)),
                redo = listOf(EditSnapshot(Presets.toJsonString(forward), 180)),
            ),
            tool = if (durableSource) {
                EditorToolState(
                    Category.MASKS,
                    EditorOverlayTool.MASK_GEOMETRY,
                    EXPECTED_MASK_INDEX,
                )
            } else {
                EditorToolState(Category.INPUT, EditorOverlayTool.CROP, EXPECTED_MASK_INDEX)
            },
            preset = EditorPresetState(
                baseJson = Presets.toJsonString(baseline),
                fullJson = Presets.toJsonString(forward),
                amount = 0.4f,
                clipboardJson = Presets.toJsonString(current),
                selectedPreset = "ticket139 look",
                presetName = "ticket139 preset",
            ),
            export = EditorExportState(
                sheetOpen = runningExport,
                options = ExportOptions(ExportFormat.PNG16, 93, ExportSize.FULL, 2048, "ticket139"),
                keepGps = false,
                phase = if (runningExport) EditorExportPhase.RUNNING else EditorExportPhase.IDLE,
                runtimeRunId = if (runningExport) 139L else null,
            ),
        )
        // Freeze the oracle in the exact canonical representation the durable reader returns.
        // Comparing a pretty Params JSON producer string to its compact persisted equivalent would
        // test formatting rather than cursor identity and could make the pre-launch oracle fail
        // even though every parameter is identical.
        return EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(document))
    }

    /** Canonical immutable value written before an Activity is allowed to launch. */
    internal fun activityFixtureOracle(): EditorSessionDocument =
        richDocument(durableSource = false, runningExport = false)

    private fun verifyRichCursor(document: EditorSessionDocument) {
        check(document.current.rotationDegrees == EXPECTED_ROTATION) { "rotation was not restored" }
        check(document.history.undo.size == EXPECTED_UNDO) { "undo branch was not restored" }
        check(document.history.redo.size == EXPECTED_REDO) { "redo branch was not restored" }
        val expectedTool = checkNotNull(expectedEditorOracle.get()) {
            "complete tool oracle is missing"
        }.tool
        check(document.tool == expectedTool) { "complete editor tool cursor was not restored" }
        check(document.preset.baseJson != null && document.preset.fullJson != null) {
            "preset amount anchors were not restored"
        }
        check(document.preset.clipboardJson != null) { "settings clipboard was not restored" }
        val params = ParamsState()
        Presets.decode(AtomicJsonStore.parseObject(document.current.paramsJson), params)
        check(params.localAdjustments.size == 3) { "mask collection was not restored" }
        check(params.exposureCompensationEv == 1.25f) { "current edit cursor was not restored" }
        check(params.crop) { "enabled crop was not restored" }
        check(params.cropCenter == (0.43f to 0.57f)) { "asymmetric crop center was not restored" }
        check(params.cropSize == (0.62f to 0.38f)) { "asymmetric crop size was not restored" }

        verifyPersistedParamsSentinels(document.current.paramsJson)
    }

    /**
     * Exact, field-level oracle for every persisted ParamsState property. Arrays represent one
     * aggregate ParamsState property (pair/triple/curve), and the mask document represents the one
     * localAdjustments property. The static path set catches an encoder omission even if both the
     * default and rich encoders accidentally omit the same field.
     */
    internal fun verifyPersistedParamsSentinels(
        paramsJson: String,
        expectedExposure: String = "1.25",
    ) {
        val actual = persistedParamsFieldValues(AtomicJsonStore.parseObject(paramsJson))
        check(EXPECTED_PERSISTED_PARAMS_SENTINELS.keys == EXPECTED_PERSISTED_PARAMS_FIELDS) {
            "typed Params oracle drifted from the independent schema path inventory"
        }
        check(actual.keys == EXPECTED_PERSISTED_PARAMS_SENTINELS.keys) {
            "rich Params paths incomplete: missing=" +
                (EXPECTED_PERSISTED_PARAMS_SENTINELS.keys - actual.keys).sorted() +
                " unexpected=" + (actual.keys - EXPECTED_PERSISTED_PARAMS_SENTINELS.keys).sorted()
        }
        val expected = EXPECTED_PERSISTED_PARAMS_SENTINELS.toMutableMap().apply {
            this["camera.exposureCompensationEv"] = number(expectedExposure)
        }
        val mismatches = expected.keys.filter { path ->
            actual[path] != expected[path]
        }
        check(mismatches.isEmpty()) {
            "rich Params typed sentinel mismatch: " + mismatches.joinToString { path ->
                "$path expected=${expected[path]} actual=${actual[path]}"
            }
        }
    }

    private fun persistedParamsFieldValues(root: JSONObject): Map<String, PersistedSentinel> {
        val fields = linkedMapOf<String, PersistedSentinel>()
        fun visit(value: Any?, path: String) {
            if (value is JSONObject && path != "masks") {
                val keys = mutableListOf<String>()
                val iterator = value.keys()
                while (iterator.hasNext()) keys += iterator.next()
                keys.sorted().forEach { key ->
                    if (path.isEmpty() && (key == "schema" || key == "version")) return@forEach
                    visit(value.get(key), if (path.isEmpty()) key else "$path.$key")
                }
            } else {
                fields[path] = when (value) {
                    is String -> text(value)
                    is Boolean -> flag(value)
                    is Number -> PersistedSentinel.Number(canonicalNumber(value))
                    is JSONObject, is JSONArray -> PersistedSentinel.Json(canonicalJson(value))
                    else -> error("unsupported persisted Params value at $path")
                }
            }
        }
        visit(root, "")
        return fields
    }

    private fun canonicalNumber(value: Number): String {
        val asDouble = value.toDouble()
        check(asDouble.isFinite()) { "non-finite persisted number" }
        return if (asDouble % 1.0 == 0.0) {
            java.math.BigDecimal(value.toString()).stripTrailingZeros().toPlainString()
        } else {
            asDouble.toFloat().toString()
        }
    }

    private fun canonicalJson(value: Any?): String = when (value) {
        is JSONObject -> {
            val keys = mutableListOf<String>()
            val iterator = value.keys()
            while (iterator.hasNext()) keys += iterator.next()
            keys.sorted().joinToString(prefix = "{", postfix = "}") { key ->
                JSONObject.quote(key) + ":" + canonicalJson(value.get(key))
            }
        }
        is JSONArray -> (0 until value.length()).joinToString(prefix = "[", postfix = "]") {
            canonicalJson(value.get(it))
        }
        is String -> JSONObject.quote(value)
        is Boolean -> value.toString()
        is Number -> canonicalNumber(value)
        JSONObject.NULL, null -> "null"
        else -> error("unsupported JSON sentinel type ${value.javaClass.simpleName}")
    }

    private fun richParams(exposure: Float): ParamsState = ParamsState().apply {
        filmProfile = "kodak_ektar_100"
        printProfile = "kodak_supra_endura"

        // Native preview ingress is linear ProPhoto-only. Exercise the rest of the persisted
        // input family with non-default sentinels without making the real Activity fixture
        // unrenderable before its restored overlay can enter composition.
        inputColorSpace = "ProPhoto RGB"
        inputCctfDecoding = true
        spectralUpsampling = Rgb2Raw.MALLETT2019
        adaptationWindow = false
        adaptationSurface = true
        spectralGaussianBlur = 0.35f
        filterUv = Triple(0.1f, 405f, 7f)
        filterIr = Triple(0.2f, 680f, 14f)
        upscaleFactor = 1.25f
        crop = true
        cropCenter = 0.43f to 0.57f
        cropSize = 0.62f to 0.38f

        rawWhiteBalance = WhiteBalance.CUSTOM
        rawTemperature = 4_875f
        rawTint = 0.92f
        creativeWbTemp = 7f
        creativeWbTint = -4f
        balanceToFilmStock = true
        contrast = 8f
        saturation = 6f
        vibrance = 4f
        gamutCompress = 3f
        localAdjustments = List(3) { index -> localAdjustment(index) }

        exposureCompensationEv = exposure
        autoExposure = false
        autoExposureMethod = "matrix"
        filmFormatMm = 33f
        cameraLensBlurUm = 0.15f
        cameraDiffusionState.apply {
            family = "glimmerglass"
            strength = 0.41f
            spatialScale = 1.1f
            haloWarmth = 0.05f
            coreIntensity = 0.91f
            coreSize = 0.85f
            haloIntensity = 0.82f
            haloSize = 0.75f
            bloomIntensity = 0.73f
            bloomSize = 1.2f
        }

        printIlluminant = "TH-KG3"
        printExposure = 1.05f
        printExposureCompensation = false
        printYFilterShift = 0.03f
        printMFilterShift = -0.02f
        enlargerLensBlur = 0.12f
        printDiffusionState.apply {
            family = "cinebloom"
            strength = 0.37f
            spatialScale = 0.9f
            haloWarmth = -0.04f
            coreIntensity = 0.88f
            coreSize = 0.84f
            haloIntensity = 0.79f
            haloSize = 0.76f
            bloomIntensity = 0.71f
            bloomSize = 1.2f
        }
        preflashExposure = 0.04f
        preflashYFilterShift = -0.01f
        preflashMFilterShift = 0.02f

        scanLensBlur = 0.08f
        scanWhiteCorrection = true
        scanWhiteLevel = 0.97f
        scanBlackCorrection = true
        scanBlackLevel = 0.015f
        scanUnsharpMask = 0.55f to 0.82f

        outputColorSpace = ColorSpace.ADOBE_RGB
        savingCctfEncoding = false
        outputGamutCompress = OutputGamutCompress.ACES_RGC
        inputGamutCompress = InputGamutCompress.XY
        scanFilm = true

        // Keep the expensive stock-character stages inactive for deterministic device time while
        // retaining distinctive non-default parameter sentinels in each serialized family.
        grainActive = false
        grainSublayersActive = false
        grainParticleAreaUm2 = 0.3f
        grainParticleScale = Triple(0.9f, 1.1f, 1.9f)
        grainParticleScaleLayers = Triple(2.4f, 1.1f, 0.6f)
        grainDensityMin = Triple(0.08f, 0.09f, 0.13f)
        grainUniformity = Triple(0.96f, 0.95f, 0.98f)
        grainBlur = 0.61f
        grainBlurDyeCloudsUm = 0.9f
        grainMicroStructure = 0.24f to 28f
        grainNSubLayers = 2
        halationActive = false
        halScatterAmount = 0.83f
        halScatterSpatialScale = 0.91f
        halHalationAmount = 0.77f
        halHalationSpatialScale = 0.87f
        halBoostEv = 0.12f
        halProtectEv = 3.8f
        halBoostRange = 0.27f
        halScatterCoreUm = Triple(2.1f, 1.9f, 1.5f)
        halScatterTailUm = Triple(9.2f, 9.6f, 9f)
        halScatterTailWeightPct = Triple(77f, 64f, 66f)
        halHalationStrengthPct = Triple(4.9f, 1.4f, 0.1f)
        halFirstSigmaUm = Triple(64f, 63f, 62f)
        halNBounces = 2
        halBounceDecay = 0.47f
        halRenormalize = false
        couplersActive = false
        couplersAmount = 0.81f
        couplersInhibitionSamelayer = 0.91f
        couplersInhibitionInterlayer = 0.89f
        couplersGammaSamelayer = Triple(0.33f, 0.31f, 0.26f)
        couplersGammaRtoGb = 0.34f to 0.29f
        couplersGammaGtoRb = 0.14f to 0.35f
        couplersGammaBtoRg = 0.16f to 0.21f
        couplersDiffusionSizeUm = 21f
        couplersDiffusionTailUm = 195f
        couplersDiffusionTailWeight = 0.055f
        glareActive = false
        glarePercent = 0.04f
        glareRoughness = 0.62f
        glareBlur = 0.45f

        filmGammaFactor = 1.03f
        printGammaFactor = 0.97f
        morphGammaFactor = 1.02f
        morphGammaFactorFast = 1.01f
        morphGammaFactorSlow = 0.99f
        morphGammaFactorRed = 1.01f
        morphGammaFactorGreen = 0.98f
        morphGammaFactorBlue = 1.02f
        morphDeveloperExhaustion = 0.03f
        toneCurveActive = true
        toneCurveMaster = listOf(0f to 0f, 0.4f to 0.43f, 1f to 1f)
        toneCurveRed = listOf(0f to 0f, 1f to 0.98f)
        toneCurveGreen = listOf(0f to 0.01f, 1f to 1f)
        toneCurveBlue = listOf(0f to 0f, 0.7f to 0.72f, 1f to 1f)
    }

    private fun localAdjustment(index: Int): LocalAdjustment = LocalAdjustment(
        mask = Mask(
            components = listOf(
                Mask.Component(
                    mode = BlendMode.ADD,
                    shape = MaskComponent.Radial(
                        cx = 0.25f + index * 0.2f,
                        cy = 0.5f,
                        rx = 0.18f,
                        ry = 0.18f,
                    ),
                ),
            ),
        ),
        delta = TierADelta(exposureEv = index * 0.25f),
    )

    private fun verifyAgainstExpectedOracle(
        document: EditorSessionDocument,
        expectedExportPhase: String,
    ) {
        val oracle = checkNotNull(expectedEditorOracle.get()) {
            "pre-launch complete cursor oracle is missing"
        }
        val phase = EditorExportPhase.valueOf(expectedExportPhase)
        val runId = if (phase == EditorExportPhase.RUNNING) {
            expectedExportRunId.get().takeIf { it > 0L }
                ?: error("pre-launch cursor oracle is missing the running export id")
        } else {
            null
        }
        verifyCompleteEditorCursor(document, oracle, phase, runId)
    }

    private fun writeProcessOracle(
        context: Context,
        seedIdentity: String,
        document: EditorSessionDocument,
    ) {
        val payload = JSONObject()
            .put("schema", ORACLE_SCHEMA)
            .put("version", ORACLE_VERSION)
            .put("buildIdentity", buildIdentity(context))
            .put("seedIdentity", seedIdentity)
            .put("document", JSONObject(EditorSessionDocumentCodec.encode(document)))
        AtomicJsonStore.writeUtf8(
            processOracle(context),
            payload.toString(),
            PROCESS_ORACLE_MAX_BYTES,
        )
    }

    private fun readProcessOracle(context: Context, expectedSeedIdentity: String): EditorSessionDocument {
        val payload = AtomicJsonStore.parseObject(
            AtomicJsonStore.readUtf8(processOracle(context), PROCESS_ORACLE_MAX_BYTES),
        )
        check(payload.getString("schema") == ORACLE_SCHEMA) { "process oracle schema mismatch" }
        check(payload.getInt("version") == ORACLE_VERSION) { "process oracle version mismatch" }
        check(payload.getString("buildIdentity") == buildIdentity(context)) {
            "process oracle belongs to another build"
        }
        check(payload.getString("seedIdentity") == expectedSeedIdentity) {
            "process oracle belongs to another seed"
        }
        return EditorSessionDocumentCodec.decode(payload.getJSONObject("document").toString())
    }

    private fun buildIdentity(context: Context): String {
        val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
        @Suppress("DEPRECATION")
        val versionCode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            packageInfo.longVersionCode
        } else {
            packageInfo.versionCode.toLong()
        }
        val buildType = if (
            context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0
        ) {
            "debug"
        } else {
            "release"
        }
        return "${context.packageName}:$versionCode:$buildType"
    }

    private fun requireLoaded(context: Context): EditorSessionDocument =
        when (val restored = EditorSessionStore.read(context.applicationContext)) {
            is EditorSessionReadResult.Loaded -> restored.document
            else -> error("editor session unavailable: ${restored.javaClass.simpleName}")
        }

    private fun processMarker(context: Context): File = File(
        File(context.noBackupFilesDir, PROBE_DIRECTORY),
        PROCESS_MARKER,
    )

    private fun processOracle(context: Context): File = File(
        File(context.noBackupFilesDir, PROBE_DIRECTORY),
        PROCESS_ORACLE,
    )

    private const val PROCESS_ORACLE_MAX_BYTES = 4 * 1024 * 1024
}
