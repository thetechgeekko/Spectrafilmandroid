package com.spectrafilm.engine

import android.app.Activity
import android.app.Instrumentation
import android.os.Bundle
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread

@Suppress("DEPRECATION")
class EngineBoundaryInstrumentation : Instrumentation() {
    private val params = SpektraParams(
        filmProfile = "kodak_portra_400",
        printProfile = "kodak_portra_endura",
    )

    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        start()
    }

    override fun onStart() {
        val results = Bundle()
        try {
            logicalDirectBufferRangeAndCancellationMapping()
            meterRejectsMismatchedLinearImageColorSpace()
            latestParamsCrossTheActualJniMarshallerAndDefaultsSurviveAbsence()
            nativeResultCloseIsConcurrentAndForeignBufferSafe()
            activeRenderCancellationAndEngineCloseLease()
            results.putString("stream", "ENGINE_BOUNDARY_INSTRUMENTATION: PASS\n")
            finish(Activity.RESULT_OK, results)
        } catch (failure: Throwable) {
            results.putString(
                "stream",
                "ENGINE_BOUNDARY_INSTRUMENTATION: FAIL\n${Log.getStackTraceString(failure)}\n",
            )
            finish(Activity.RESULT_CANCELED, results)
        }
    }

    private fun meterRejectsMismatchedLinearImageColorSpace() {
        SpektraEngine.fromAssets(targetContext.assets).use { engine ->
            val data = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
                .order(ByteOrder.nativeOrder())
            data.asFloatBuffer().apply {
                put(0, 0.18f)
                put(1, 0.18f)
                put(2, 0.18f)
            }
            val mismatched = LinearImage(
                data = data,
                width = 1,
                height = 1,
                colorSpace = "ACES2065-1",
            )
            val failure = expect<RuntimeException> {
                engine.meterExposureEv(mismatched, params)
            }
            check(failure.message.orEmpty().contains("unsupported input color space")) {
                "meter silently reinterpreted ${mismatched.colorSpace}: $failure"
            }
        }
    }

    private fun latestParamsCrossTheActualJniMarshallerAndDefaultsSurviveAbsence() {
        // Every SpektraParams leaf receives a non-default sentinel. The native
        // seam emits every spk_params field by name using ABI-neutral encodings,
        // while expectedNamedInventory derives the independent Kotlin oracle.
        // This is intentionally exhaustive:
        // camera + both diffusion filters, enlarger, scanner, grain, halation,
        // DIR, film/print glare, print morph, IO/gamut, settings/GPU/LUT, and all
        // four packed tone-curve channels. Removing any getter/assignment creates
        // a named value mismatch rather than blessing an already-omitted getter.
        val customized = SpektraParams(
            filmProfile = "film-jni-sentinel",
            printProfile = "print-jni-sentinel",
            camera = CameraParams(
                exposureCompensationEv = 1.25f,
                autoExposure = false,
                autoExposureMethod = "matrix",
                lensBlurUm = 2.5f,
                filmFormatMm = 46.5f,
                filterUv = Triple(0.11f, 411.25f, 8.75f),
                filterIr = Triple(0.22f, 676.5f, 15.25f),
                diffusionFilter = DiffusionFilterParams(
                    active = true,
                    filterFamily = "cinebloom",
                    strength = 0.31f,
                    spatialScale = 1.31f,
                    haloWarmth = 0.32f,
                    coreIntensity = 1.32f,
                    coreSize = 1.33f,
                    haloIntensity = 1.34f,
                    haloSize = 1.35f,
                    bloomIntensity = 1.36f,
                    bloomSize = 1.37f,
                ),
            ),
            enlarger = EnlargerParams(
                illuminant = "D50",
                printExposure = 1.41f,
                printExposureCompensation = false,
                normalizePrintExposure = false,
                yFilterShift = 2.42f,
                mFilterShift = 3.43f,
                yFilterNeutral = 54.4f,
                mFilterNeutral = 64.5f,
                cFilterNeutral = 4.46f,
                lensBlur = 5.47f,
                diffusionFilter = DiffusionFilterParams(
                    active = true,
                    filterFamily = "glimmerglass",
                    strength = 0.51f,
                    spatialScale = 1.52f,
                    haloWarmth = 0.53f,
                    coreIntensity = 1.54f,
                    coreSize = 1.55f,
                    haloIntensity = 1.56f,
                    haloSize = 1.57f,
                    bloomIntensity = 1.58f,
                    bloomSize = 1.59f,
                ),
                preflashExposure = 0.61f,
                preflashYFilterShift = 6.62f,
                preflashMFilterShift = 7.63f,
            ),
            scanner = ScannerParams(
                lensBlur = 0.71f,
                whiteCorrection = true,
                blackCorrection = true,
                whiteLevel = 0.972f,
                blackLevel = 0.023f,
                unsharpMask = 0.74f to 0.75f,
            ),
            filmRender = FilmRenderingParams(
                densityCurveGamma = 1.81f,
                grain = GrainParams(
                    active = false,
                    sublayersActive = false,
                    agxParticleAreaUm2 = 0.82f,
                    agxParticleScale = Triple(0.83f, 1.84f, 2.85f),
                    agxParticleScaleLayers = Triple(2.86f, 1.87f, 0.88f),
                    densityMin = Triple(0.089f, 0.091f, 0.092f),
                    uniformity = Triple(0.913f, 0.924f, 0.935f),
                    blur = 0.846f,
                    blurDyeCloudsUm = 1.857f,
                    microStructure = 0.868f to 31.879f,
                    nSubLayers = 4,
                ),
                halation = HalationParams(
                    active = false,
                    scatterAmount = 0.93f,
                    scatterSpatialScale = 1.94f,
                    halationAmount = 0.95f,
                    halationSpatialScale = 1.96f,
                    scatterCoreUm = Triple(2.97f, 2.98f, 1.99f),
                    scatterTailUm = Triple(9.01f, 9.02f, 9.03f),
                    scatterTailWeight = Triple(0.704f, 0.705f, 0.706f),
                    boostEv = 1.07f,
                    boostRange = 0.408f,
                    protectEv = 4.09f,
                    halationStrength = Triple(0.061f, 0.026f, 0.017f),
                    halationFirstSigmaUm = Triple(61.1f, 62.2f, 63.3f),
                    halationNBounces = 5,
                    halationBounceDecay = 0.44f,
                    halationRenormalize = false,
                ),
                dirCouplers = DirCouplersParams(
                    active = false,
                    amount = 0.51f,
                    inhibitionSamelayer = 0.52f,
                    inhibitionInterlayer = 0.53f,
                    gammaSamelayerRgb = Triple(0.354f, 0.335f, 0.286f),
                    gammaInterlayerRToGb = 0.367f to 0.318f,
                    gammaInterlayerGToRb = 0.169f to 0.371f,
                    gammaInterlayerBToRg = 0.182f to 0.239f,
                    diffusionSizeUm = 21.4f,
                    diffusionTailUm = 201.5f,
                    diffusionTailWeight = 0.076f,
                ),
                glare = GlareParams(
                    active = false,
                    percent = 0.041f,
                    roughness = 0.72f,
                    blur = 0.63f,
                ),
            ),
            printRender = PrintRenderingParams(
                densityCurveGamma = 1.23f,
                glare = GlareParams(
                    active = false,
                    percent = 0.052f,
                    roughness = 0.83f,
                    blur = 0.74f,
                ),
                densityCurvesMorph = PrintCurvesMorphParams(
                    active = true,
                    gammaFactor = 1.11f,
                    gammaFactorFast = 1.12f,
                    gammaFactorSlow = 1.13f,
                    gammaFactorRed = 1.14f,
                    gammaFactorGreen = 1.15f,
                    gammaFactorBlue = 1.16f,
                    developerExhaustion = 0.17f,
                ),
            ),
            io = IoParams(
                inputColorSpace = "Adobe RGB",
                inputCctfDecoding = true,
                outputColorSpace = ColorSpace.REC2020,
                outputCctfEncoding = false,
                outputGamutCompress = OutputGamutCompress.OKLRAB,
                inputGamutCompress = InputGamutCompress.XY,
                crop = true,
                cropCenter = 0.26f to 0.67f,
                cropSize = 0.38f to 0.49f,
                upscaleFactor = 1.75f,
                scanFilm = true,
            ),
            settings = SettingsParams(
                rgbToRawMethod = Rgb2Raw.MALLETT2019,
                applyHanatos2025AdaptationWindow = false,
                applyHanatos2025AdaptationSurface = true,
                spectralGaussianBlur = 0.81f,
                useEnlargerLut = true,
                useScannerLut = true,
                lutResolution = 29,
                previewMaxSize = 713,
                neutralPrintFiltersFromDatabase = false,
                gpuPreview = true,
                gpuExport = true,
            ),
            toneCurve = ToneCurveParams(
                active = true,
                master = ToneCurveChannel(listOf(0.01f to 0.02f, 0.91f to 0.92f)),
                red = ToneCurveChannel(
                    listOf(0.03f to 0.04f, 0.53f to 0.54f, 0.93f to 0.94f),
                ),
                green = ToneCurveChannel(
                    listOf(
                        0.05f to 0.06f, 0.35f to 0.36f,
                        0.65f to 0.66f, 0.95f to 0.96f,
                    ),
                ),
                blue = ToneCurveChannel(
                    listOf(
                        0.07f to 0.08f, 0.27f to 0.28f, 0.47f to 0.48f,
                        0.67f to 0.68f, 0.97f to 0.98f,
                    ),
                ),
            ),
        )
        val customManifest = parseNamedInventory(
            SpektraEngine.debugMarshalledParams(customized),
        )
        val nullManifest = parseNamedInventory(
            SpektraEngine.debugMarshalledParams(null),
        )
        // This object has only the two historical profile getters. Every nested
        // getter is genuinely absent, exercising GetMethodID failure/clear rather
        // than merely a null top-level reference.
        val legacyManifest = parseNamedInventory(
            SpektraEngine.debugMarshalledParams(
                LegacyTopLevelParams("legacy-film", "legacy-print"),
            ),
        )
        assertNamedInventory(
            label = "custom Kotlin sentinels",
            actual = customManifest,
            expected = expectedNamedInventory(customized),
        )

        // A null jobject and a legacy top-level object with every nested getter
        // absent must retain the independently constructed native defaults. The
        // native default auto-exposure method is a null pointer (semantically the
        // same center-weighted default), so override that one string explicitly.
        assertNamedInventory(
            label = "null jobject defaults",
            actual = nullManifest,
            expected = expectedNamedInventory(
                SpektraParams("", ""),
                autoExposureMethod = null,
            ),
        )
        assertNamedInventory(
            label = "legacy missing-getter defaults",
            actual = legacyManifest,
            expected = expectedNamedInventory(
                SpektraParams("legacy-film", "legacy-print"),
                autoExposureMethod = null,
            ),
        )
    }

    private data class LegacyTopLevelParams(
        val filmProfile: String,
        val printProfile: String,
    )

    private fun parseNamedInventory(encoded: String): Map<String, String> {
        val parsed = linkedMapOf<String, String>()
        encoded.lineSequence().filter(String::isNotEmpty).forEach { line ->
            val separator = line.indexOf('=')
            check(separator > 0) { "malformed native parameter inventory line: $line" }
            val key = line.substring(0, separator)
            val previous = parsed.put(key, line.substring(separator + 1))
            check(previous == null) { "duplicate native parameter inventory key: $key" }
        }
        return parsed
    }

    private fun assertNamedInventory(
        label: String,
        actual: Map<String, String>,
        expected: Map<String, String>,
    ) {
        if (actual == expected) return
        val differences = (actual.keys + expected.keys).toSortedSet().mapNotNull { key ->
            val a = actual[key]
            val e = expected[key]
            if (a == e) null else "$key expected=$e actual=$a"
        }
        error(
            "$label JNI inventory mismatch (${differences.size} differences, " +
                "expected ${expected.size} fields, actual ${actual.size}): " +
                differences.take(24).joinToString(),
        )
    }

    private fun MutableMap<String, String>.putI32(name: String, value: Int) {
        put(name, "i$value")
    }

    private fun MutableMap<String, String>.putBool(name: String, value: Boolean) {
        putI32(name, if (value) 1 else 0)
    }

    private fun MutableMap<String, String>.putF32(name: String, value: Float) {
        put(name, "f${Integer.toHexString(value.toRawBits()).padStart(8, '0')}")
    }

    private fun MutableMap<String, String>.putStringValue(name: String, value: String?) {
        if (value == null) {
            put(name, "null")
            return
        }
        val hex = "0123456789abcdef"
        put(
            name,
            buildString(1 + value.length * 2) {
                append('s')
                value.toByteArray(Charsets.UTF_8).forEach { byte ->
                    val unsigned = byte.toInt() and 0xff
                    append(hex[unsigned ushr 4])
                    append(hex[unsigned and 0x0f])
                }
            },
        )
    }

    private fun MutableMap<String, String>.putF32Array(
        name: String,
        values: List<Float>,
    ) {
        values.forEachIndexed { index, value -> putF32("$name[$index]", value) }
    }

    /**
     * Independent oracle for every current spk_params field. Values come from
     * the Kotlin tree, while native emits the post-marshalling C fields by name.
     * Exact key-set equality makes an omitted manifest field fail; distinct
     * non-default values in the custom case make an omitted getter fail.
     */
    private fun expectedNamedInventory(
        params: SpektraParams,
        autoExposureMethod: String? = params.camera.autoExposureMethod,
    ): Map<String, String> = linkedMapOf<String, String>().apply {
        val camera = params.camera
        val cameraDiffusion = camera.diffusionFilter
        val enlarger = params.enlarger
        val enlargerDiffusion = enlarger.diffusionFilter
        val scanner = params.scanner
        val film = params.filmRender
        val grain = film.grain
        val halation = film.halation
        val dir = film.dirCouplers
        val print = params.printRender
        val io = params.io
        val settings = params.settings

        put("schema", "spk_params_named_v2")
        putStringValue("film_profile", params.filmProfile)
        putStringValue("print_profile", params.printProfile)
        putF32("exposure_compensation_ev", camera.exposureCompensationEv)
        putBool("auto_exposure", camera.autoExposure)
        putF32("lens_blur_um", camera.lensBlurUm)
        putF32("film_format_mm", camera.filmFormatMm)
        putStringValue("auto_exposure_method", autoExposureMethod)

        putF32("y_filter_shift", enlarger.yFilterShift)
        putF32("m_filter_shift", enlarger.mFilterShift)
        putF32("preflash_exposure", enlarger.preflashExposure)
        putBool("normalize_print_exposure", enlarger.normalizePrintExposure)

        putF32("density_curve_gamma", film.densityCurveGamma)
        putBool("grain_active", grain.active)
        putBool("halation_active", halation.active)
        putBool("dir_couplers_active", dir.active)
        putBool("glare_active", film.glare.active)

        putBool("scan_film", io.scanFilm)
        putI32("output_color_space", io.outputColorSpace.ordinal)
        putBool("output_cctf_encoding", io.outputCctfEncoding)
        putI32("rgb_to_raw_method", settings.rgbToRawMethod.ordinal)
        putI32("preview_max_size", settings.previewMaxSize)

        putF32Array(
            "camera_filter_uv",
            listOf(camera.filterUv.first, camera.filterUv.second, camera.filterUv.third),
        )
        putF32Array(
            "camera_filter_ir",
            listOf(camera.filterIr.first, camera.filterIr.second, camera.filterIr.third),
        )
        putBool("camera_diffusion_active", cameraDiffusion.active)
        putF32("camera_diffusion_strength", cameraDiffusion.strength)
        putF32("camera_diffusion_spatial_scale", cameraDiffusion.spatialScale)
        putF32("camera_diffusion_halo_warmth", cameraDiffusion.haloWarmth)
        putF32("camera_diffusion_core_intensity", cameraDiffusion.coreIntensity)
        putF32("camera_diffusion_core_size", cameraDiffusion.coreSize)
        putF32("camera_diffusion_halo_intensity", cameraDiffusion.haloIntensity)
        putF32("camera_diffusion_halo_size", cameraDiffusion.haloSize)
        putF32("camera_diffusion_bloom_intensity", cameraDiffusion.bloomIntensity)
        putF32("camera_diffusion_bloom_size", cameraDiffusion.bloomSize)

        putF32("print_exposure", enlarger.printExposure)
        putBool("print_exposure_compensation", enlarger.printExposureCompensation)
        putF32("y_filter_neutral", enlarger.yFilterNeutral)
        putF32("m_filter_neutral", enlarger.mFilterNeutral)
        putF32("c_filter_neutral", enlarger.cFilterNeutral)
        putF32("enlarger_lens_blur", enlarger.lensBlur)
        putF32("preflash_y_filter_shift", enlarger.preflashYFilterShift)
        putF32("preflash_m_filter_shift", enlarger.preflashMFilterShift)
        putBool("enlarger_diffusion_active", enlargerDiffusion.active)
        putF32("enlarger_diffusion_strength", enlargerDiffusion.strength)
        putF32("enlarger_diffusion_spatial_scale", enlargerDiffusion.spatialScale)
        putF32("enlarger_diffusion_halo_warmth", enlargerDiffusion.haloWarmth)
        putF32("enlarger_diffusion_core_intensity", enlargerDiffusion.coreIntensity)
        putF32("enlarger_diffusion_core_size", enlargerDiffusion.coreSize)
        putF32("enlarger_diffusion_halo_intensity", enlargerDiffusion.haloIntensity)
        putF32("enlarger_diffusion_halo_size", enlargerDiffusion.haloSize)
        putF32("enlarger_diffusion_bloom_intensity", enlargerDiffusion.bloomIntensity)
        putF32("enlarger_diffusion_bloom_size", enlargerDiffusion.bloomSize)

        putF32("scanner_lens_blur", scanner.lensBlur)
        putF32Array("scanner_unsharp", listOf(scanner.unsharpMask.first, scanner.unsharpMask.second))
        putBool("scanner_white_correction", scanner.whiteCorrection)
        putBool("scanner_black_correction", scanner.blackCorrection)
        putF32("scanner_white_level", scanner.whiteLevel)
        putF32("scanner_black_level", scanner.blackLevel)

        putBool("grain_sublayers_active", grain.sublayersActive)
        putF32("grain_particle_area_um2", grain.agxParticleAreaUm2)
        putF32Array(
            "grain_particle_scale",
            listOf(grain.agxParticleScale.first, grain.agxParticleScale.second, grain.agxParticleScale.third),
        )
        putF32Array(
            "grain_particle_scale_layers",
            listOf(
                grain.agxParticleScaleLayers.first,
                grain.agxParticleScaleLayers.second,
                grain.agxParticleScaleLayers.third,
            ),
        )
        putF32Array(
            "grain_density_min",
            listOf(grain.densityMin.first, grain.densityMin.second, grain.densityMin.third),
        )
        putF32Array(
            "grain_uniformity",
            listOf(grain.uniformity.first, grain.uniformity.second, grain.uniformity.third),
        )
        putF32("grain_blur", grain.blur)
        putF32("grain_blur_dye_clouds_um", grain.blurDyeCloudsUm)
        putF32Array(
            "grain_micro_structure",
            listOf(grain.microStructure.first, grain.microStructure.second),
        )
        putI32("grain_n_sub_layers", grain.nSubLayers)

        putF32("halation_scatter_amount", halation.scatterAmount)
        putF32("halation_scatter_spatial_scale", halation.scatterSpatialScale)
        putF32("halation_halation_amount", halation.halationAmount)
        putF32("halation_halation_spatial_scale", halation.halationSpatialScale)
        putF32Array(
            "halation_scatter_core_um",
            listOf(halation.scatterCoreUm.first, halation.scatterCoreUm.second, halation.scatterCoreUm.third),
        )
        putF32Array(
            "halation_scatter_tail_um",
            listOf(halation.scatterTailUm.first, halation.scatterTailUm.second, halation.scatterTailUm.third),
        )
        putF32Array(
            "halation_scatter_tail_weight",
            listOf(
                halation.scatterTailWeight.first,
                halation.scatterTailWeight.second,
                halation.scatterTailWeight.third,
            ),
        )
        putF32("halation_boost_ev", halation.boostEv)
        putF32("halation_boost_range", halation.boostRange)
        putF32("halation_protect_ev", halation.protectEv)
        putF32Array(
            "halation_strength",
            listOf(
                halation.halationStrength.first,
                halation.halationStrength.second,
                halation.halationStrength.third,
            ),
        )
        putF32Array(
            "halation_first_sigma_um",
            listOf(
                halation.halationFirstSigmaUm.first,
                halation.halationFirstSigmaUm.second,
                halation.halationFirstSigmaUm.third,
            ),
        )
        putI32("halation_n_bounces", halation.halationNBounces)
        putF32("halation_bounce_decay", halation.halationBounceDecay)
        putBool("halation_renormalize", halation.halationRenormalize)

        putF32("dir_amount", dir.amount)
        putF32("dir_inhibition_samelayer", dir.inhibitionSamelayer)
        putF32("dir_inhibition_interlayer", dir.inhibitionInterlayer)
        putF32Array(
            "dir_gamma_samelayer_rgb",
            listOf(dir.gammaSamelayerRgb.first, dir.gammaSamelayerRgb.second, dir.gammaSamelayerRgb.third),
        )
        putF32Array(
            "dir_gamma_interlayer_r_to_gb",
            listOf(dir.gammaInterlayerRToGb.first, dir.gammaInterlayerRToGb.second),
        )
        putF32Array(
            "dir_gamma_interlayer_g_to_rb",
            listOf(dir.gammaInterlayerGToRb.first, dir.gammaInterlayerGToRb.second),
        )
        putF32Array(
            "dir_gamma_interlayer_b_to_rg",
            listOf(dir.gammaInterlayerBToRg.first, dir.gammaInterlayerBToRg.second),
        )
        putF32("dir_diffusion_size_um", dir.diffusionSizeUm)
        putF32("dir_diffusion_tail_um", dir.diffusionTailUm)
        putF32("dir_diffusion_tail_weight", dir.diffusionTailWeight)

        putF32("glare_percent", film.glare.percent)
        putF32("glare_roughness", film.glare.roughness)
        putF32("glare_blur", film.glare.blur)
        putBool("print_glare_active", print.glare.active)
        putF32("print_glare_percent", print.glare.percent)
        putF32("print_glare_roughness", print.glare.roughness)
        putF32("print_glare_blur", print.glare.blur)
        putF32("print_density_curve_gamma", print.densityCurveGamma)

        val morph = print.densityCurvesMorph
        putBool("print_morph_active", morph.active)
        putF32("print_morph_gamma_factor", morph.gammaFactor)
        putF32("print_morph_gamma_factor_fast", morph.gammaFactorFast)
        putF32("print_morph_gamma_factor_slow", morph.gammaFactorSlow)
        putF32("print_morph_gamma_factor_red", morph.gammaFactorRed)
        putF32("print_morph_gamma_factor_green", morph.gammaFactorGreen)
        putF32("print_morph_gamma_factor_blue", morph.gammaFactorBlue)
        putF32("print_morph_developer_exhaustion", morph.developerExhaustion)

        putBool("input_cctf_decoding", io.inputCctfDecoding)
        putBool("crop", io.crop)
        putF32Array("crop_center", listOf(io.cropCenter.first, io.cropCenter.second))
        putF32Array("crop_size", listOf(io.cropSize.first, io.cropSize.second))
        putF32("upscale_factor", io.upscaleFactor)

        putBool("apply_hanatos_window", settings.applyHanatos2025AdaptationWindow)
        putBool("apply_hanatos_surface", settings.applyHanatos2025AdaptationSurface)
        putF32("spectral_gaussian_blur", settings.spectralGaussianBlur)
        putBool("use_enlarger_lut", settings.useEnlargerLut)
        putBool("use_scanner_lut", settings.useScannerLut)
        putI32("lut_resolution", settings.lutResolution)
        putBool("neutral_print_filters_from_database", settings.neutralPrintFiltersFromDatabase)
        putBool("gpu_preview", settings.gpuPreview)
        putBool("gpu_export", settings.gpuExport)
        putI32("allow_gpu_scan", 0)

        putI32("output_gamut_compress", io.outputGamutCompress.ordinal)
        putI32("input_gamut_compress", io.inputGamutCompress.ordinal)

        val tone = params.toneCurve
        putBool("tone_curve_active", tone.active)
        putToneChannel("tone_curve_master", tone.master)
        putToneChannel("tone_curve_rgb[0]", tone.red)
        putToneChannel("tone_curve_rgb[1]", tone.green)
        putToneChannel("tone_curve_rgb[2]", tone.blue)

        putI32("disable_buffer_memos", 0)
        putStringValue("enlarger_illuminant", enlarger.illuminant)
        putStringValue("input_color_space", io.inputColorSpace)
        putI32("camera_diffusion_family", diffusionFamilyOrdinal(cameraDiffusion.filterFamily))
        putI32("enlarger_diffusion_family", diffusionFamilyOrdinal(enlargerDiffusion.filterFamily))
    }

    private fun MutableMap<String, String>.putToneChannel(
        baseName: String,
        channel: ToneCurveChannel,
    ) {
        val points = channel.points.take(TONE_CURVE_MAX_POINTS)
        val isMaster = baseName == "tone_curve_master"
        val countName = if (isMaster) {
            "tone_curve_master_n"
        } else {
            baseName.replace("tone_curve_rgb", "tone_curve_rgb_n")
        }
        val xName = if (isMaster) "tone_curve_master_x" else baseName.replace("rgb", "rgb_x")
        val yName = if (isMaster) "tone_curve_master_y" else baseName.replace("rgb", "rgb_y")
        putI32(countName, points.size)
        repeat(TONE_CURVE_MAX_POINTS) { index ->
            putF32("$xName[$index]", points.getOrNull(index)?.first ?: 0.0f)
            putF32("$yName[$index]", points.getOrNull(index)?.second ?: 0.0f)
        }
    }

    private fun diffusionFamilyOrdinal(family: String): Int = when (family) {
        "glimmerglass" -> 1
        "black_pro_mist" -> 2
        "pro_mist" -> 3
        "cinebloom" -> 4
        else -> error("unsupported expected diffusion family: $family")
    }

    private fun logicalDirectBufferRangeAndCancellationMapping() {
        SpektraEngine.fromAssets(targetContext.assets).use { engine ->
            val accepted = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
            accepted.putFloat(4, 0.18f)
            accepted.putFloat(8, 0.18f)
            accepted.putFloat(12, 0.18f)
            accepted.position(4)
            accepted.limit(16)
            val token = RenderCancellation().also { it.cancel() }
            expect<CancellationException> {
                engine.simulate(LinearImage(accepted, 1, 1), params,
                                cancellation = token)
            }

            val short = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
            short.position(4)
            short.limit(12)
            val shortFailure = expect<RuntimeException> {
                engine.simulate(LinearImage(short, 1, 1), params,
                                cancellation = token)
            }
            check(shortFailure.message.orEmpty().contains("ByteBuffer range"))

            val unaligned = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
            unaligned.position(2)
            unaligned.limit(14)
            val alignmentFailure = expect<RuntimeException> {
                engine.simulate(LinearImage(unaligned, 1, 1), params,
                                cancellation = token)
            }
            check(alignmentFailure.message.orEmpty().contains("unaligned"))

            val oversized = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder())
            val overflowFailure = expect<RuntimeException> {
                engine.simulate(
                    LinearImage(oversized, Int.MAX_VALUE, Int.MAX_VALUE),
                    params,
                    cancellation = token,
                )
            }
            check(overflowFailure.message.orEmpty().contains("oversized"))
        }
    }

    private fun nativeResultCloseIsConcurrentAndForeignBufferSafe() {
        SpektraEngine.fromAssets(targetContext.assets).use {
            val owned = NativeBufferOwner.allocate(64)
                ?: throw AssertionError("native allocation failed")
            val start = CountDownLatch(1)
            val workers = List(16) {
                thread {
                    start.await()
                    repeat(100) { owned.close() }
                }
            }
            start.countDown()
            workers.forEach { it.join(5_000) }
            check(workers.none { it.isAlive })

            // A foreign direct buffer is not a native allocation owner and cannot be freed.
            SimResult(
                ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()),
                1, 1, ColorSpace.SRGB, 0L, 1L,
            ).close()
        }
    }

    private fun activeRenderCancellationAndEngineCloseLease() {
        val edge = 1_024
        val buffer = ByteBuffer.allocateDirect(edge * edge * 3 * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        val floats = buffer.asFloatBuffer()
        for (index in 0 until floats.capacity()) floats.put(index, 0.18f)
        val image = LinearImage(buffer, edge, edge)
        val engine = SpektraEngine.fromAssets(targetContext.assets)
        val cancellation = RenderCancellation()
        val nativePollEntered = CountDownLatch(1)
        val releaseNativePoll = CountDownLatch(1)
        val renderFinished = CountDownLatch(1)
        val closeStarted = CountDownLatch(1)
        val closeFinished = CountDownLatch(1)
        val result = AtomicReference<SimResult?>()
        val failure = AtomicReference<Throwable?>()
        val renderOwner = AtomicReference<Thread?>()
        val pollOwner = AtomicReference<Thread?>()

        cancellation.testPollObserver = {
            pollOwner.compareAndSet(null, Thread.currentThread())
            nativePollEntered.countDown()
            try {
                releaseNativePoll.await(30, TimeUnit.SECONDS)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }

        val renderThread = thread(name = "jni-active-cancel") {
            renderOwner.set(Thread.currentThread())
            try {
                result.set(engine.simulate(image, params, cancellation = cancellation))
            } catch (caught: Throwable) {
                failure.set(caught)
            } finally {
                renderFinished.countDown()
            }
        }
        var closeThread: Thread? = null
        try {
            check(nativePollEntered.await(5, TimeUnit.SECONDS)) {
                "native render never polled cancellation"
            }
            check(pollOwner.get() === renderOwner.get()) {
                "JNI cancellation callback escaped render owner: " +
                    "render=${renderOwner.get()?.name}, poll=${pollOwner.get()?.name}"
            }
            val closingThread = thread(name = "jni-close-race") {
                closeStarted.countDown()
                engine.close()
                closeFinished.countDown()
            }
            closeThread = closingThread
            check(closeStarted.await(5, TimeUnit.SECONDS)) { "close thread did not start" }
            val queuedDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
            while (!engine.isLifecycleCloseQueuedForTest(closingThread) &&
                System.nanoTime() < queuedDeadline
            ) {
                Thread.yield()
            }
            // hasQueuedThread is true only after close actually attempted the write
            // lock while the owner remains blocked inside a native cancellation poll.
            check(engine.isLifecycleCloseQueuedForTest(closingThread)) {
                "engine close never queued behind the active render lease"
            }
            check(closeFinished.count == 1L) { "queued engine close finished prematurely" }

            cancellation.cancel()
            releaseNativePoll.countDown()
            check(renderFinished.await(30, TimeUnit.SECONDS)) {
                "active native render did not observe cancellation"
            }
            check(closeFinished.await(30, TimeUnit.SECONDS)) {
                "engine close did not finish after cancelled render released its lease"
            }
            renderThread.join(5_000)
            closingThread.join(5_000)
            check(!renderThread.isAlive && !closingThread.isAlive)
            check(result.get() == null) { "cancelled native render published a stale result" }
            check(failure.get() is CancellationException) {
                "expected CancellationException from active render, got ${failure.get()}"
            }
        } finally {
            cancellation.cancel()
            releaseNativePoll.countDown()
            cancellation.testPollObserver = null
            renderThread.join(30_000)
            closeThread?.join(30_000)
            result.getAndSet(null)?.close()
            engine.close()
            image.close()
        }
    }

    private inline fun <reified T : Throwable> expect(block: () -> Unit): T {
        try {
            block()
        } catch (failure: Throwable) {
            if (failure is T) return failure
            throw AssertionError("expected ${T::class.java.name}, got $failure", failure)
        }
        throw AssertionError("expected ${T::class.java.name}")
    }
}
