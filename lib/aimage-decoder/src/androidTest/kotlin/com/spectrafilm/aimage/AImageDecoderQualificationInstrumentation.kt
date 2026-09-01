/*
 * Spektrafilm for Android — API-30+ AImageDecoder qualification runner.
 * GPL-3.0-only.
 */
package com.spectrafilm.aimage

import android.app.Activity
import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ColorSpace
import android.graphics.ImageDecoder
import android.graphics.Matrix
import android.os.Build
import android.os.Bundle
import android.os.Debug
import android.os.ParcelFileDescriptor
import android.os.Process
import android.os.SystemClock
import android.util.Log
import com.spectrafilm.engine.NativeBufferOwner
import com.spectrafilm.engine.SpektraEngine
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.round

private val AIMAGE_PROCESS_NONCE: String = UUID.randomUUID().toString()
private val AIMAGE_PROCESS_START_TICKS: Long = readProcessStartTicks()
private const val PREVIEW_MAX_EDGE = 2_048
private const val EXPORT_MAX_EDGE = 16_384
private const val PHONE_WIDTH = 4_080
private const val PHONE_HEIGHT = 3_060
private const val BMP_HEADER_BYTES = 54
private const val PHONE_BMP_BYTES = 37_454_454L
private const val SAMPLE_CAPTURE_COUNT = 16
private const val HALF_SAMPLE_TOLERANCE = 0.001f
private const val P3_SAMPLE_TOLERANCE = 0.005f
private const val TRANSPARENT_EPSILON = 0.00001f
private const val EIGHT_BIT_EPSILON = 0.02f

/** Kernel process-start evidence (field 22 of /proc/self/stat), captured once. */
private fun readProcessStartTicks(): Long {
    val stat = File("/proc/self/stat").readText(Charsets.US_ASCII)
    val commandEnd = stat.lastIndexOf(')')
    check(commandEnd >= 0) { "could not parse /proc/self/stat command" }
    // The first token after the parenthesized command is field 3 (state), so
    // field 22 (starttime) is zero-based token 19 in this suffix.
    val suffix = stat.substring(commandEnd + 1).trim().split(Regex("\\s+"))
    return suffix.getOrNull(19)?.toLongOrNull()
        ?: error("could not parse /proc/self/stat starttime")
}

private val RGBA16_ORACLE: List<IntArray> = listOf(
    intArrayOf(1, 257, 65_534, 65_535),
    intArrayOf(1_000, 10_000, 40_000, 0),
    intArrayOf(32_768, 32_769, 32_770, 32_768),
    intArrayOf(65_535, 12_345, 54_321, 65_535),
    intArrayOf(513, 1_025, 2_049, 65_535),
    intArrayOf(4_095, 8_191, 16_383, 32_767),
    intArrayOf(22_222, 33_333, 44_444, 1),
    intArrayOf(65_000, 17, 30_000, 65_535),
)

/**
 * Platform-only runner: no AndroidX/JUnit dependency enters the experiment APK.
 *
 * A successful invocation proves one OS/device cell only. It never claims that
 * API 30/34/36, OEMs, release/R8, or archival-exact behavior have all passed.
 */
@Suppress("DEPRECATION")
class AImageDecoderQualificationInstrumentation : Instrumentation() {
    private var arguments: Bundle = Bundle.EMPTY

    override fun onCreate(arguments: Bundle?) {
        this.arguments = if (arguments == null) Bundle.EMPTY else Bundle(arguments)
        super.onCreate(arguments)
        start()
    }

    override fun onStart() {
        val results = Bundle()
        try {
            val report = runQualification()
            results.putString("stream", report)
            finish(Activity.RESULT_OK, results)
        } catch (failure: Throwable) {
            results.putString(
                "stream",
                "AIMAGE_QUALIFICATION: FAIL\n${Log.getStackTraceString(failure)}\n",
            )
            finish(Activity.RESULT_CANCELED, results)
        }
    }

    private fun runQualification(): String {
        check(Build.VERSION.SDK_INT >= 30) { "AImageDecoder qualification requires API 30+" }
        val expectedApi = argumentInt(ARG_EXPECTED_API, Build.VERSION.SDK_INT, 30, 10_000)
        check(Build.VERSION.SDK_INT == expectedApi) {
            "expected API $expectedApi, running API ${Build.VERSION.SDK_INT}"
        }
        val repeats = argumentInt(ARG_REPEATS, 7, 1, 50)
        val warmups = argumentInt(ARG_WARMUPS, 2, 0, 10)
        val maxEdge = argumentInt(ARG_MAX_EDGE, 16_384, 1, 16_384)
        val requireFullCorpus = argumentBoolean(ARG_REQUIRE_FULL_CORPUS, true)
        val requireMinified = argumentBoolean(ARG_REQUIRE_MINIFIED, false)
        check(!requireMinified || BuildConfig.AIMAGE_TARGET_MINIFIED) {
            "minified smoke requested, but target BuildConfig is not the R8 release variant"
        }
        val r8CanaryRuntimeName = AImageDecoderNative.r8CanaryRuntimeClassName()
        check(!requireMinified || r8CanaryRuntimeName != AIMAGE_R8_CANARY_ORIGINAL_NAME) {
            "minified smoke requested, but the retained R8 canary was not renamed: " +
                r8CanaryRuntimeName
        }
        val phase = arguments.getString(ARG_RECREATION_PHASE, "full")
        check(phase == "full" || phase == "seed" || phase == "recover" ||
            phase == "r8-smoke"
        ) {
            "$ARG_RECREATION_PHASE must be full, seed, recover, or r8-smoke"
        }
        check(phase != "r8-smoke" || requireMinified) {
            "r8-smoke requires $ARG_REQUIRE_MINIFIED=true"
        }

        SpektraEngine.configureMemoryBudget(DEFAULT_MEMORY_BUDGET_BYTES)
        return when (phase) {
            "seed" -> runRecreationSeed(maxEdge)
            "recover" -> runRecreationRecover(maxEdge)
            "r8-smoke" -> runR8Smoke()
            else -> runFull(repeats, warmups, maxEdge, requireFullCorpus)
        }
    }

    private fun runR8Smoke(): String {
        val report = StringBuilder()
        report.appendLine(
            record(
                "AIMAGE_R8_TARGET_V1",
                "target_minified" to BuildConfig.AIMAGE_TARGET_MINIFIED,
                "canary_runtime_name" to AImageDecoderNative.r8CanaryRuntimeClassName(),
                "canary_renamed" to true,
            ),
        )
        runMalformedAndMimeGates(report)
        assertNoNativeResources("after R8 typed-fallback smoke")
        assertMemoryCoordinatorIdle("after R8 typed-fallback smoke")
        report.append("AIMAGE_R8_SMOKE: PASS; NOT_A_QUALIFICATION_CELL\n")
        return report.toString()
    }

    private fun runFull(
        repeats: Int,
        warmups: Int,
        maxEdge: Int,
        requireFullCorpus: Boolean,
    ): String {
        val report = StringBuilder()
        report.appendLine(deviceRecord(repeats, warmups, maxEdge))
        val external = loadExternalCorpus(report)
        val missing = QualificationCorpus.external
            .map(ExternalFixtureSpec::fileName)
            .filterNot(external::containsKey)
        if (requireFullCorpus) {
            check(missing.isEmpty()) {
                "full qualification corpus is missing: ${missing.joinToString()}"
            }
        }

        val fixtures = ArrayList<QualificationFixture>()
        fixtures += QualificationCorpus.embedded
        fixtures += QualificationCorpus.external
            .filterNot(ExternalFixtureSpec::hostileOnly)
            .mapNotNull { external[it.fileName] }

        fixtures.forEach { fixture ->
            withMaterializedFixture(fixture) { file ->
                // Correctness, source hashing and sample oracles are intentionally
                // outside every timed/memory window.
                val contract = fixture.decodeContract()
                val ndk = decodeNdkFile(fixture, file, maxEdge)
                val java = decodeJavaCurrentSemantics(contract, file, maxEdge)

                if (fixture.bytes.size <= DIRECT_CONSISTENCY_LIMIT_BYTES) {
                    val direct = decodeNdkBuffer(fixture, maxEdge)
                    check(direct == ndk) {
                        "${fixture.name} fd/direct NDK outputs differ: fd=$ndk, " +
                            "buffer=$direct"
                    }
                }
                verifyOrientationEvidence(fixture, ndk, java)
                verifyDecodedSampleEvidence(fixture, ndk, java, report)
                val performanceQualified = isWorkingEndpointComparable(ndk, java) &&
                    maxEdge > PREVIEW_MAX_EDGE && contract.encodedOrientation == 1
                report.appendLine(
                    record(
                        "AIMAGE_CORRECTNESS_V2",
                        "fixture" to fixture.name,
                        "source_bytes" to fixture.bytes.size,
                        "source_sha256" to fixture.expectedSha256,
                        "source_digest_verified" to true,
                        "provenance" to fixture.provenance,
                        "declared_mime" to fixture.mime,
                        "sniffed_kind" to fixture.kind,
                        "encoded_orientation" to fixture.encodedOrientation,
                        "display_referred_dng_fallback" to
                            fixture.displayReferredDngFallback,
                        "header_geometry" to "${ndk.headerWidth}x${ndk.headerHeight}",
                        "output_geometry" to "${ndk.outputWidth}x${ndk.outputHeight}",
                        "platform_mime" to ndk.headerMime,
                        "ndk_default_config" to ndk.decodedConfig,
                        "ndk_final_config" to ndk.finalConfig,
                        "ndk_alpha" to ndk.alpha,
                        "ndk_dataspace" to ndk.dataSpace,
                        "ndk_color_space" to ndk.colorSpace,
                        "ndk_pixel_sha256" to ndk.pixelSha256,
                        "ndk_linear_sha256" to ndk.linearSha256,
                        "java_decoded_config" to java.decodedConfig,
                        "java_final_config" to java.finalConfig,
                        "java_route" to currentJavaRouteName(contract),
                        "java_alpha" to java.alpha,
                        "java_dataspace" to java.dataSpace,
                        "java_color_space" to java.colorSpace,
                        "java_pixel_sha256" to java.pixelSha256,
                        "java_linear_sha256" to java.linearSha256,
                        "latency_peak_memory_status" to if (performanceQualified) {
                            "AIMAGE_PERF_V2_follows"
                        } else {
                            "AIMAGE_PERF_UNQUALIFIED_V1_follows"
                        },
                        "hashing_timed" to false,
                    ),
                )
                report.appendLine(
                    routeInventoryRecord(
                        fixture,
                        "ndk_encoded_source_to_candidate_output",
                        ndk.inventory,
                    ),
                )
                report.appendLine(
                    routeInventoryRecord(
                        fixture,
                        currentJavaRouteName(contract),
                        java.inventory,
                    ),
                )

                if (performanceQualified) {
                    val subject = fixture.benchmarkSubject()
                    report.appendLine(
                        benchmark(
                            subject = subject,
                            caseName = "fixture_full",
                            route = "ndk_fd_to_linear_prophoto_f32",
                            expected = ProductionEndpoint.from(ndk),
                            repeats = repeats,
                            warmups = warmups,
                        ) { decodeNdkProduction(fixture, file, maxEdge) }.record(),
                    )
                    report.appendLine(
                        benchmark(
                            subject = subject,
                            caseName = "fixture_full",
                            route = currentJavaRouteName(contract),
                            expected = ProductionEndpoint.from(java),
                            repeats = repeats,
                            warmups = warmups,
                        ) { decodeJavaProduction(contract, file, maxEdge) }.record(),
                    )
                } else {
                    report.appendLine(
                        record(
                            "AIMAGE_PERF_UNQUALIFIED_V1",
                            "fixture" to fixture.name,
                            "reason" to when {
                                maxEdge <= PREVIEW_MAX_EDGE ->
                                    "current_preview_uses_gc_owned_direct_output_not_identical_ownership"
                                contract.encodedOrientation != 1 ->
                                    "encoded_orientation_not_pinned_upright_for_timing"
                                else ->
                                    "routes_do_not_share_an_admitted_opaque_srgb_f32_endpoint"
                            },
                            "timed" to false,
                            "latency_ns" to "not_measured",
                            "pss_peak_kb" to "not_measured",
                            "native_peak_bytes" to "not_measured",
                            "jvm_peak_bytes" to "not_measured",
                        ),
                    )
                }
                assertNoNativeResources("after ${fixture.name}")
                assertMemoryCoordinatorIdle("after ${fixture.name}")
            }
        }

        // Generated before its baseline/timing windows. The uncompressed BMP is
        // deterministic and phone-sized without a large binary download.
        runGeneratedPhonePerformance(report, repeats, warmups)

        runMalformedAndMimeGates(report)
        runDeterministicCancellationCleanup(report, maxEdge)
        runMemoryBudgetDenial(report, maxEdge)
        runRepeatedImportCleanup(report, maxEdge)
        external[REPRESENTATIVE_DNG]?.let { runDngPolicyGates(it, report) }
        external[HOSTILE_DNG]?.let { runHostileDng(it, report, maxEdge) }
        assertNoNativeResources("at runner completion")
        assertMemoryCoordinatorIdle("at runner completion")

        val corpusComplete = missing.isEmpty()
        report.appendLine(
            record(
                "AIMAGE_CELL_STATUS_V1",
                "api" to Build.VERSION.SDK_INT,
                "corpus_complete" to corpusComplete,
                // Process recreation must be proven by separate seed/force-stop/recover
                // invocations, so this single-process run cannot close the device cell.
                "process_recreation_separate" to true,
                "cell_complete" to false,
                "missing" to missing.joinToString(","),
                "hdr_headroom_tonemap_oracle_complete" to false,
                "phone_preview_correctness_executed" to true,
                "phone_preview_perf_qualified" to false,
                "phone_preview_perf_executed" to false,
                "phone_export_perf_executed" to true,
                "target_minified" to BuildConfig.AIMAGE_TARGET_MINIFIED,
                // An external aggregator must prove every required API/device cell.
                "matrix_complete" to false,
                "adoption_enabled" to false,
                "archival_exact_claim" to false,
            ),
        )
        report.append(
            if (corpusComplete) {
                "AIMAGE_QUALIFICATION: CORPUS_EXECUTION_PASS; " +
                    "HDR_HEADROOM_ORACLE_BLOCKED; RECREATION_SEPARATE; MATRIX_INCOMPLETE\n"
            } else {
                "AIMAGE_QUALIFICATION: PRELIMINARY_PASS; CORPUS_INCOMPLETE\n"
            },
        )
        return report.toString()
    }

    private fun runRecreationSeed(maxEdge: Int): String {
        val fixture = QualificationCorpus.embedded.first()
        val observation = withMaterializedFixture(fixture) { file ->
            decodeNdkFile(fixture, file, maxEdge)
        }
        assertNoNativeResources("before process-recreation seed")
        assertMemoryCoordinatorIdle("before process-recreation seed")
        val marker = recreationMarker()
        val text = listOf(
            "sfaimage.recreation.v3",
            Build.VERSION.SDK_INT.toString(),
            Build.FINGERPRINT,
            Process.myPid().toString(),
            AIMAGE_PROCESS_START_TICKS.toString(),
            AIMAGE_PROCESS_NONCE,
            fixture.expectedSha256,
            observation.pixelSha256,
            observation.linearSha256,
        ).joinToString("\t")
        FileOutputStream(marker, false).use { output ->
            output.write(text.toByteArray(Charsets.UTF_8))
            output.fd.sync()
        }
        return record(
            "AIMAGE_RECREATION_SEED_V3",
            "api" to Build.VERSION.SDK_INT,
            "fingerprint" to Build.FINGERPRINT,
            "pid" to Process.myPid(),
            "process_start_ticks" to AIMAGE_PROCESS_START_TICKS,
            "process_nonce" to AIMAGE_PROCESS_NONCE,
            "marker" to marker.absolutePath,
            "pixel_sha256" to observation.pixelSha256,
            "linear_sha256" to observation.linearSha256,
            "next" to "force-stop target, then run phase=recover",
        ) + "\n"
    }

    private fun runRecreationRecover(maxEdge: Int): String {
        val marker = recreationMarker()
        check(marker.isFile) { "process-recreation marker is absent" }
        val fields = marker.readText(Charsets.UTF_8).split('\t')
        check(fields.size == 9 && fields[0] == "sfaimage.recreation.v3") {
            "process-recreation marker is malformed"
        }
        check(fields[1].toIntOrNull() == Build.VERSION.SDK_INT) {
            "process-recreation API changed across phases"
        }
        check(fields[2] == Build.FINGERPRINT) {
            "build fingerprint changed across recreation phases"
        }
        val seedPid = fields[3].toIntOrNull()
            ?: error("process-recreation seed PID is malformed")
        val seedStartTicks = fields[4].toLongOrNull()
            ?: error("process-recreation seed start ticks are malformed")
        val seedNonce = fields[5]
        check(seedNonce.isNotBlank() && seedNonce != AIMAGE_PROCESS_NONCE) {
            "recover retained the seed process-lifetime nonce; process death was not proven " +
                "(seed=$seedPid/$seedStartTicks/$seedNonce " +
                "current=${Process.myPid()}/$AIMAGE_PROCESS_START_TICKS/$AIMAGE_PROCESS_NONCE)"
        }
        check(seedPid != Process.myPid() || seedStartTicks != AIMAGE_PROCESS_START_TICKS) {
            "recover retained both kernel process identifiers; process death was not proven " +
                "(pid=$seedPid start_ticks=$seedStartTicks)"
        }
        val fixture = QualificationCorpus.embedded.first()
        check(fields[6] == fixture.expectedSha256) { "recreation source digest changed" }
        val observation = withMaterializedFixture(fixture) { file ->
            decodeNdkFile(fixture, file, maxEdge)
        }
        check(fields[7] == observation.pixelSha256 && fields[8] == observation.linearSha256) {
            "decode digest changed after process recreation"
        }
        assertNoNativeResources("after process-recreation recovery")
        assertMemoryCoordinatorIdle("after process-recreation recovery")
        check(marker.delete() || !marker.exists()) { "could not remove recreation marker" }
        return record(
            "AIMAGE_RECREATION_RECOVER_V3",
            "api" to Build.VERSION.SDK_INT,
            "fingerprint" to Build.FINGERPRINT,
            "seed_pid" to seedPid,
            "recover_pid" to Process.myPid(),
            "pid_changed" to (seedPid != Process.myPid()),
            "seed_process_start_ticks" to seedStartTicks,
            "recover_process_start_ticks" to AIMAGE_PROCESS_START_TICKS,
            "process_start_changed" to (seedStartTicks != AIMAGE_PROCESS_START_TICKS),
            "seed_nonce" to seedNonce,
            "recover_nonce" to AIMAGE_PROCESS_NONCE,
            "process_nonce_changed" to true,
            "pixel_sha256" to observation.pixelSha256,
            "linear_sha256" to observation.linearSha256,
            "status" to "pass",
        ) + "\n"
    }

    private fun loadExternalCorpus(
        report: StringBuilder,
    ): Map<String, QualificationFixture> {
        val root = arguments.getString(ARG_CORPUS_DIR, "").trimEnd('/')
        if (root.isEmpty()) return emptyMap()
        check(root == DEVICE_CORPUS_DIR) {
            "$ARG_CORPUS_DIR must be exactly $DEVICE_CORPUS_DIR"
        }
        val loaded = linkedMapOf<String, QualificationFixture>()
        QualificationCorpus.external.forEach { spec ->
            val bytes = readShellFixture("$root/${spec.fileName}")
            if (bytes == null) return@forEach
            val digest = sha256(bytes)
            check(digest == spec.expectedSha256) {
                "${spec.fileName} digest $digest != ${spec.expectedSha256}"
            }
            loaded[spec.fileName] = QualificationFixture(
                name = spec.fileName,
                mime = spec.mime,
                kind = spec.kind,
                expectedSha256 = spec.expectedSha256,
                encodedOrientation = spec.encodedOrientation,
                provenance = spec.upstreamUrl,
                bytes = bytes,
                displayReferredDngFallback = spec.displayReferredDngFallback,
            )
            report.appendLine(
                record(
                    "AIMAGE_SOURCE_V1",
                    "name" to spec.fileName,
                    "bytes" to bytes.size,
                    "sha256" to digest,
                    "provenance" to spec.upstreamUrl,
                ),
            )
        }
        return loaded
    }

    /** Read an adb-pushed fixture through a shell-owned pipe without broad storage grants. */
    private fun readShellFixture(path: String): ByteArray? {
        check(path.matches(Regex("/data/local/tmp/sfaimage-corpus/[A-Za-z0-9._-]+")))
        // UiAutomation executes an argv-style shell command on every supported
        // API cell; do not depend on a device shell parsing `if ...; then`.
        // The strict path allow-list above makes this direct cat non-injectable.
        val descriptor = uiAutomation.executeShellCommand("cat $path")
        ParcelFileDescriptor.AutoCloseInputStream(descriptor).use { input ->
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(16 * 1024)
            var total = 0L
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                total += count
                check(total <= AIMAGE_MAX_ENCODED_BYTES) { "$path exceeds 128 MiB" }
                output.write(buffer, 0, count)
            }
            return output.toByteArray().takeIf { it.isNotEmpty() }
        }
    }

    private fun decodeNdkFile(
        fixture: QualificationFixture,
        file: File,
        maxEdge: Int,
    ): DecodeObservation = decodeNdkFile(fixture.decodeContract(), file, maxEdge)

    private fun decodeNdkFile(
        contract: DecodeSourceContract,
        file: File,
        maxEdge: Int,
    ): DecodeObservation = ParcelFileDescriptor.open(
        file,
        ParcelFileDescriptor.MODE_READ_ONLY,
    ).use { descriptor ->
        decodeNdk(
            contract,
            AImageEncodedSource.fromFileDescriptor(descriptor.fd),
            maxEdge,
        )
    }

    private fun decodeNdkBuffer(
        fixture: QualificationFixture,
        maxEdge: Int,
    ): DecodeObservation {
        val encoded = ByteBuffer.allocateDirect(fixture.bytes.size)
        encoded.put(fixture.bytes).flip()
        return decodeNdk(
            fixture.decodeContract(),
            AImageEncodedSource.fromDirectBuffer(encoded),
            maxEdge,
        )
    }

    private fun decodeNdk(
        contract: DecodeSourceContract,
        source: AImageEncodedSource,
        maxEdge: Int,
        cancellation: AImageDecodeCancellation = AImageDecodeCancellation(),
    ): DecodeObservation {
        val header = AImageDecoderExperiment.probe(
            source = source,
            declaredMime = contract.mime,
            allowDisplayReferredDngFallback = contract.displayReferredDngFallback,
        )
        check(header.inputKind == contract.kind) {
            "${contract.name}: ${header.inputKind} != ${contract.kind}"
        }
        val plan = planAImageDecode(header, maxEdge)
        val knownAllocatedBytes = Math.addExact(
            plan.pixelByteCount.toLong(),
            plan.linearProPhotoByteCount.toLong(),
        )
        val inventory = DecodeRouteInventory(
            measurementScope = "observable_owned_boundaries",
            knownDataAllocations = 1 + if (plan.canConvertToLinearProPhoto) 1 else 0,
            knownAllocatedBytesTotal = knownAllocatedBytes,
            decodeOutputWrites = 1,
            boundaryCopies = 0,
            transformWrites = if (plan.canConvertToLinearProPhoto) 1 else 0,
            operations = if (plan.canConvertToLinearProPhoto) {
                "encoded_source_probe>encoded_source_decode_reprobe>" +
                    "platform_decode_direct_to_owned_rgba>" +
                    "owned_rgba_to_owned_linear_prophoto_f32"
            } else {
                "encoded_source_probe>encoded_source_decode_reprobe>" +
                    "platform_decode_direct_to_owned_rgba_f16"
            },
        )
        AImageDecoderExperiment.decodePixels(source, plan, cancellation).use { decoded ->
            val pixelRead = decoded.acquirePixels().use { lease ->
                PixelRead(
                    sha256 = digest(lease.data),
                    samples = analyzeNativeSamples(
                        lease.data,
                        decoded.metadata.width,
                        decoded.metadata.height,
                        decoded.metadata.pixelFormat,
                    ),
                )
            }
            val linearDigest = if (plan.canConvertToLinearProPhoto) {
                decoded.toLinearProPhoto(cancellation).use { linear ->
                    linear.acquireDataLease().use { lease -> digest(lease.data) }
                }
            } else {
                "not-admitted"
            }
            return DecodeObservation(
                headerWidth = header.width,
                headerHeight = header.height,
                outputWidth = decoded.metadata.width,
                outputHeight = decoded.metadata.height,
                headerMime = header.platformMime,
                decodedConfig = header.platformDefaultFormat.toString(),
                finalConfig = decoded.metadata.pixelFormat.name,
                alpha = decoded.metadata.alphaFlags.toString(),
                dataSpace = decoded.metadata.dataSpace,
                colorSpace = "ADataSpace(${decoded.metadata.dataSpace})",
                pixelSha256 = pixelRead.sha256,
                linearSha256 = linearDigest,
                hiddenRgbPreserved = decoded.metadata.alphaFlags != ALPHA_PREMUL,
                samples = pixelRead.samples,
                inventory = inventory,
            )
        }
    }

    /** Select the app's actual current comparator for this source class. */
    private fun decodeJavaPrepared(
        contract: DecodeSourceContract,
        file: File,
        maxEdge: Int,
        inventory: MutableBitmapInventory? = null,
    ): JavaPreparedBitmap = if (contract.displayReferredDngFallback) {
        decodeImageDecoderFallbackPrepared(file, maxEdge, inventory)
    } else {
        decodeBitmapFactoryPrepared(contract, file, maxEdge, inventory)
    }

    /** Mirrors ImagePipeline.decodeViaPlatform's API-28+ DNG fallback. */
    private fun decodeImageDecoderFallbackPrepared(
        file: File,
        maxEdge: Int,
        inventory: MutableBitmapInventory?,
    ): JavaPreparedBitmap {
        var headerWidth = 0
        var headerHeight = 0
        var headerMime = ""
        var headerColorSpace = "unknown"
        val decoded = ImageDecoder.decodeBitmap(ImageDecoder.createSource(file)) { decoder, info, _ ->
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            headerWidth = info.size.width
            headerHeight = info.size.height
            headerMime = info.mimeType
            headerColorSpace = info.colorSpace?.name ?: "unknown"
            val longest = max(headerWidth, headerHeight).coerceAtLeast(1)
            var sample = 1
            while (longest / sample > maxEdge) sample *= 2
            if (sample > 1) decoder.setTargetSampleSize(sample)
        }
        inventory?.recordDecode(decoded, "imagedecoder_decode_to_bitmap")
        val decodedConfig = decoded.config?.name ?: "null"
        val safe = forceArgbAndScale(decoded, maxEdge, inventory)
        return JavaPreparedBitmap(
            bitmap = safe,
            headerWidth = headerWidth,
            headerHeight = headerHeight,
            headerMime = headerMime,
            headerColorSpace = headerColorSpace,
            decodedConfig = decodedConfig,
        )
    }

    /** Mirrors ImagePipeline.decodeDownscaled plus its downstream EXIF rotation. */
    private fun decodeBitmapFactoryPrepared(
        contract: DecodeSourceContract,
        file: File,
        maxEdge: Int,
        inventory: MutableBitmapInventory?,
    ): JavaPreparedBitmap {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        FileInputStream(file).use { input ->
            BitmapFactory.decodeStream(input, null, bounds)
        }
        check(bounds.outWidth > 0 && bounds.outHeight > 0) {
            "${contract.name}: BitmapFactory bounds decode failed"
        }
        val longest = max(bounds.outWidth, bounds.outHeight)
        var sample = 1
        while (longest / sample > maxEdge) sample *= 2
        val options = BitmapFactory.Options().apply {
            inSampleSize = sample
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }
        val decoded = FileInputStream(file).use { input ->
            BitmapFactory.decodeStream(input, null, options)
        } ?: error("${contract.name}: BitmapFactory pixel decode failed")
        inventory?.recordDecode(decoded, "bitmapfactory_decode_to_bitmap")
        val decodedConfig = decoded.config?.name ?: "null"
        val bounded = scaleToMaxEdge(decoded, maxEdge, inventory)
        val oriented = applyPinnedProductionOrientation(
            bounded,
            contract.encodedOrientation,
            inventory,
        )
        val swapsAxes = contract.encodedOrientation == 6
        return JavaPreparedBitmap(
            bitmap = oriented,
            headerWidth = if (swapsAxes) bounds.outHeight else bounds.outWidth,
            headerHeight = if (swapsAxes) bounds.outWidth else bounds.outHeight,
            headerMime = bounds.outMimeType ?: contract.mime,
            headerColorSpace = options.outColorSpace?.name ?: "unknown",
            decodedConfig = decodedConfig,
        )
    }

    private fun forceArgbAndScale(
        decoded: Bitmap,
        maxEdge: Int,
        inventory: MutableBitmapInventory?,
    ): Bitmap {
        val argb = if (decoded.config == Bitmap.Config.ARGB_8888) {
            decoded
        } else {
            decoded.copy(Bitmap.Config.ARGB_8888, false).also { copied ->
                inventory?.recordCopy(copied, "bitmap_config_copy_to_argb8888")
                decoded.recycle()
            }
        }
        return scaleToMaxEdge(argb, maxEdge, inventory)
    }

    private fun scaleToMaxEdge(
        decoded: Bitmap,
        maxEdge: Int,
        inventory: MutableBitmapInventory?,
    ): Bitmap {
        val longest = max(decoded.width, decoded.height)
        return if (longest <= maxEdge) {
            decoded
        } else {
            val scale = maxEdge.toFloat() / longest
            Bitmap.createScaledBitmap(
                decoded,
                (decoded.width * scale).toInt().coerceAtLeast(1),
                (decoded.height * scale).toInt().coerceAtLeast(1),
                true,
            ).also { scaled ->
                if (scaled !== decoded) {
                    inventory?.recordTransform(scaled, "bitmap_scale_transform")
                    decoded.recycle()
                }
            }
        }
    }

    private fun applyPinnedProductionOrientation(
        bitmap: Bitmap,
        orientation: Int,
        inventory: MutableBitmapInventory?,
    ): Bitmap =
        when (orientation) {
            0, 1 -> bitmap
            6 -> Bitmap.createBitmap(
                bitmap,
                0,
                0,
                bitmap.width,
                bitmap.height,
                Matrix().apply { setRotate(90f) },
                false,
            ).also { oriented ->
                if (oriented !== bitmap) {
                    inventory?.recordTransform(oriented, "bitmap_orientation_transform")
                    bitmap.recycle()
                }
            }
            else -> error("unsupported pinned comparator orientation $orientation")
        }

    private fun decodeJavaCurrentSemantics(
        contract: DecodeSourceContract,
        file: File,
        maxEdge: Int,
    ): DecodeObservation {
        val bitmapInventoryTracker = MutableBitmapInventory()
        val prepared = decodeJavaPrepared(
            contract,
            file,
            maxEdge,
            bitmapInventoryTracker,
        )
        try {
            val safe = prepared.bitmap
            val pixelDigest = canonicalRgba8888Digest(safe)
            val linearDigest = javaBitmapToLinearProPhoto(safe).use { linear ->
                linear.acquireDataLease().use { lease -> digest(lease.data) }
            }
            val colorSpace = safe.colorSpace
            val alpha = when {
                !safe.hasAlpha() -> "opaque"
                safe.isPremultiplied -> "premul"
                else -> "unpremul"
            }
            val bitmapInventory = bitmapInventoryTracker.snapshot()
            val rowsPerBand = (1024 * 1024 / safe.width).coerceIn(1, safe.height)
            val bandCopies = (safe.height + rowsPerBand - 1) / rowsPerBand
            val scratchBytes = Math.multiplyExact(
                Math.multiplyExact(safe.width.toLong(), rowsPerBand.toLong()),
                Int.SIZE_BYTES.toLong(),
            )
            val linearBytes = Math.multiplyExact(
                Math.multiplyExact(safe.width.toLong(), safe.height.toLong()),
                3L * Float.SIZE_BYTES,
            )
            val knownAllocatedBytes = Math.addExact(
                bitmapInventory.allocatedBytesTotal,
                Math.addExact(scratchBytes, linearBytes),
            )
            val inventory = DecodeRouteInventory(
                measurementScope =
                    "observable_production_route_boundaries_hashing_excluded",
                knownDataAllocations = bitmapInventory.allocations + 2,
                knownAllocatedBytesTotal = knownAllocatedBytes,
                decodeOutputWrites = 1,
                boundaryCopies = bitmapInventory.explicitCopies + bandCopies,
                transformWrites = bitmapInventory.transformWrites + 1,
                operations = (if (contract.displayReferredDngFallback) {
                    bitmapInventory.operations
                } else {
                    "bitmapfactory_bounds_probe>${bitmapInventory.operations}"
                }) +
                    ">bitmap_getpixels_${bandCopies}_bands>" +
                    "owned_linear_prophoto_f32_transform",
            )
            return DecodeObservation(
                headerWidth = prepared.headerWidth,
                headerHeight = prepared.headerHeight,
                outputWidth = safe.width,
                outputHeight = safe.height,
                headerMime = prepared.headerMime,
                decodedConfig = prepared.decodedConfig,
                finalConfig = safe.config?.name ?: "null",
                alpha = alpha,
                dataSpace = androidDataSpace(colorSpace),
                colorSpace =
                    "${prepared.headerColorSpace} -> ${colorSpace?.name ?: "unknown"}",
                pixelSha256 = pixelDigest,
                linearSha256 = linearDigest,
                hiddenRgbPreserved = !safe.hasAlpha() || !safe.isPremultiplied,
                samples = analyzeBitmapSamples(safe),
                inventory = inventory,
            )
        } finally {
            prepared.close()
        }
    }

    private fun javaBitmapToLinearProPhoto(
        bitmap: Bitmap,
    ): com.spectrafilm.engine.LinearImage {
        val sampleCount = Math.multiplyExact(
            Math.multiplyExact(bitmap.width.toLong(), bitmap.height.toLong()),
            3L,
        )
        val byteCount = Math.multiplyExact(sampleCount, Float.SIZE_BYTES.toLong())
        check(byteCount <= Int.MAX_VALUE.toLong()) {
            "Java comparator linear buffer exceeds direct-buffer limit: $byteCount"
        }
        // Normalize only the final hand-off owner: both candidates return the
        // same coordinator-owned, exactly closeable packed f32 image. Decoder,
        // Bitmap, scaling and conversion semantics remain today's Java route.
        val nativeOwner = NativeBufferOwner.allocate(byteCount)
        val nativeLease = try {
            nativeOwner.acquireDataLease()
        } catch (failure: Throwable) {
            nativeOwner.close()
            throw failure
        }
        val buffer = nativeLease.data.order(ByteOrder.nativeOrder())
        val image = try {
            com.spectrafilm.engine.LinearImage.fromDataLease(
                buffer,
                bitmap.width,
                bitmap.height,
                "ProPhoto RGB",
                nativeLease,
            )
        } catch (failure: Throwable) {
            nativeOwner.close()
            throw failure
        }
        nativeOwner.close()
        try {
            val floats = buffer.asFloatBuffer()
            // Match ImagePipeline.bitmapToLinearProPhoto's production scratch
            // band exactly; scratch sizing is part of both latency and memory.
            val rowsPerBand = (1024 * 1024 / bitmap.width).coerceIn(1, bitmap.height)
            val pixels = IntArray(Math.multiplyExact(bitmap.width, rowsPerBand))
            var y = 0
            while (y < bitmap.height) {
                val rows = minOf(rowsPerBand, bitmap.height - y)
                bitmap.getPixels(pixels, 0, bitmap.width, 0, y, bitmap.width, rows)
                var source = 0
                var target = y * bitmap.width * 3
                repeat(bitmap.width * rows) {
                    val argb = pixels[source++]
                    val red = srgbToLinear(((argb ushr 16) and 0xff) / 255f)
                    val green = srgbToLinear(((argb ushr 8) and 0xff) / 255f)
                    val blue = srgbToLinear((argb and 0xff) / 255f)
                    floats.put(
                        target,
                        0.5290825f * red + 0.3303437f * green + 0.1405738f * blue,
                    )
                    floats.put(
                        target + 1,
                        0.0982640f * red + 0.8734031f * green + 0.0283329f * blue,
                    )
                    floats.put(
                        target + 2,
                        0.0167029f * red + 0.1176946f * green + 0.8656026f * blue,
                    )
                    target += 3
                }
                y += rows
            }
            return image
        } catch (failure: Throwable) {
            image.close()
            throw failure
        }
    }

    private fun decodeNdkProduction(
        fixture: QualificationFixture,
        file: File,
        maxEdge: Int,
    ): ProductionResult = decodeNdkProduction(fixture.decodeContract(), file, maxEdge)

    private fun decodeNdkProduction(
        contract: DecodeSourceContract,
        file: File,
        maxEdge: Int,
    ): ProductionResult = ParcelFileDescriptor.open(
        file,
        ParcelFileDescriptor.MODE_READ_ONLY,
    ).use { descriptor ->
        val source = AImageEncodedSource.fromFileDescriptor(descriptor.fd)
        val header = AImageDecoderExperiment.probe(
            source,
            contract.mime,
            contract.displayReferredDngFallback,
        )
        val plan = planAImageDecode(header, maxEdge)
        check(plan.canConvertToLinearProPhoto) { "timed NDK endpoint is not admitted" }
        AImageDecoderExperiment.decodePixels(source, plan).use { decoded ->
            ProductionResult(decoded.toLinearProPhoto())
        }
    }

    private fun decodeJavaProduction(
        contract: DecodeSourceContract,
        file: File,
        maxEdge: Int,
    ): ProductionResult {
        val prepared = decodeJavaPrepared(contract, file, maxEdge)
        return try {
            if (!contract.displayReferredDngFallback) {
                check(prepared.decodedConfig == Bitmap.Config.ARGB_8888.name) {
                    "timed BitmapFactory comparator ignored inPreferredConfig: " +
                        prepared.decodedConfig
                }
            }
            ProductionResult(
                javaBitmapToLinearProPhoto(
                    prepared.bitmap,
                ),
            )
        } finally {
            prepared.close()
        }
    }

    private fun benchmark(
        subject: BenchmarkSubject,
        caseName: String,
        route: String,
        expected: ProductionEndpoint,
        repeats: Int,
        warmups: Int,
        operation: () -> ProductionResult,
    ): BenchmarkResult {
        repeat(warmups) {
            operation().use { result -> check(result.endpoint == expected) }
        }
        val latencies = LongArray(repeats)
        repeat(repeats) { index ->
            val started = SystemClock.elapsedRealtimeNanos()
            val result = operation()
            latencies[index] = SystemClock.elapsedRealtimeNanos() - started
            result.use {
                check(it.endpoint == expected) {
                    "${subject.name} $route did not reach the identical owned f32 endpoint"
                }
            }
        }
        val memory = PeakMemorySampler.measure(expected, operation)
        return BenchmarkResult(
            subject = subject,
            caseName = caseName,
            route = route,
            endpoint = expected,
            latenciesNanos = latencies,
            memory = memory,
        )
    }

    private fun runMalformedAndMimeGates(report: StringBuilder) {
        val fallbackBinaryName = AImageDecoderFallbackException::class.java.name
        check(fallbackBinaryName == FALLBACK_EXCEPTION_BINARY_NAME) {
            "typed fallback binary name changed: $fallbackBinaryName"
        }
        QualificationCorpus.embedded.forEach { fixture ->
            val prefixLength = when (fixture.kind) {
                AImageInputKind.JPEG -> 3
                AImageInputKind.PNG -> 8
                AImageInputKind.GIF -> 6
                AImageInputKind.WEBP -> 12
                AImageInputKind.BMP -> 2
                AImageInputKind.ICO -> 4
                AImageInputKind.WBMP -> 4
                else -> minOf(12, fixture.bytes.size)
            }
            val truncated = fixture.bytes.copyOf(prefixLength)
            expectFallback("${fixture.name} truncated") {
                val source = directSource(truncated)
                val header = AImageDecoderExperiment.probe(source, fixture.mime, false)
                AImageDecoderExperiment.decodePixels(source, planAImageDecode(header, 16_384)).close()
            }
        }
        val jpeg = QualificationCorpus.embedded.first { it.kind == AImageInputKind.JPEG }
        expectFallback("MIME/header mismatch") {
            AImageDecoderExperiment.probe(directSource(jpeg.bytes), "image/png", false)
        }
        expectFallback("oversized declared MIME") {
            AImageDecoderExperiment.probe(directSource(jpeg.bytes), "x".repeat(128), false)
        }
        val unknown = ByteArray(32) { index -> (index * 13 + 7).toByte() }
        expectFallback("unknown header") {
            AImageDecoderExperiment.probe(directSource(unknown), null, false)
        }
        val pipe = ParcelFileDescriptor.createPipe()
        try {
            expectFallback("non-seekable fd") {
                AImageDecoderExperiment.probe(
                    AImageEncodedSource.fromFileDescriptor(pipe[0].fd),
                    "image/jpeg",
                    false,
                )
            }
        } finally {
            pipe.forEach { it.close() }
        }
        assertNoNativeResources("after malformed/MIME/pipe gates")
        report.appendLine(
            record(
                "AIMAGE_HOSTILE_V2",
                "case" to "malformed-mime-pipe",
                "status" to "pass",
                "native_typed_fallback" to fallbackBinaryName,
                "target_minified" to BuildConfig.AIMAGE_TARGET_MINIFIED,
            ),
        )
    }

    private fun runDeterministicCancellationCleanup(report: StringBuilder, maxEdge: Int) {
        val fixture = QualificationCorpus.embedded.first()
        withMaterializedFixture(fixture) { file ->
            val header = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { pfd ->
                AImageDecoderExperiment.probe(
                    AImageEncodedSource.fromFileDescriptor(pfd.fd),
                    fixture.mime,
                    false,
                )
            }
            val plan = planAImageDecode(header, maxEdge)
            for (cancelAtPoll in listOf(3, 4)) {
                val cancellation = AImageDecodeCancellation()
                cancellation.testPollObserver = { poll ->
                    if (poll == cancelAtPoll) cancellation.cancel()
                }
                try {
                    val failure = runCatching {
                        ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { pfd ->
                            AImageDecoderExperiment.decodePixels(
                                AImageEncodedSource.fromFileDescriptor(pfd.fd),
                                plan,
                                cancellation,
                            ).close()
                        }
                    }.exceptionOrNull()
                    check(failure is CancellationException) {
                        "poll $cancelAtPoll expected CancellationException, got $failure"
                    }
                } finally {
                    cancellation.testPollObserver = null
                }
                assertNoNativeResources("after cancellation poll $cancelAtPoll")
            }
        }
        report.appendLine(
            record(
                "AIMAGE_CANCEL_V1",
                "before_decode_cleanup" to "pass",
                "after_platform_decode_discard_cleanup" to "pass",
                "platform_decode_interruptible" to false,
            ),
        )
    }

    private fun runMemoryBudgetDenial(report: StringBuilder, maxEdge: Int) {
        val fixture = QualificationCorpus.embedded.first { it.name == "project-gradient.png" }
        val header = AImageDecoderExperiment.probe(directSource(fixture.bytes), fixture.mime, false)
        val plan = planAImageDecode(header, maxEdge)
        check(plan.pixelByteCount > DENIAL_MEMORY_BUDGET_BYTES)
        SpektraEngine.configureMemoryBudget(DENIAL_MEMORY_BUDGET_BYTES)
        try {
            val failure = runCatching {
                AImageDecoderExperiment.decodePixels(directSource(fixture.bytes), plan).close()
            }.exceptionOrNull()
            check(failure is OutOfMemoryError) {
                "memory coordinator denial expected OutOfMemoryError, got $failure"
            }
            assertNoNativeResources("after memory budget denial")
            report.appendLine(
                record(
                    "AIMAGE_MEMORY_DENIAL_V1",
                    "requested_bytes" to plan.pixelByteCount,
                    "budget_bytes" to DENIAL_MEMORY_BUDGET_BYTES,
                    "failure" to failure.javaClass.name,
                    "snapshot" to SpektraEngine.memoryBudgetSnapshotJson(),
                ),
            )
        } finally {
            SpektraEngine.configureMemoryBudget(DEFAULT_MEMORY_BUDGET_BYTES)
        }
    }

    private fun runRepeatedImportCleanup(report: StringBuilder, maxEdge: Int) {
        val fixture = QualificationCorpus.embedded.first()
        var digest: String? = null
        repeat(REPEATED_IMPORT_COUNT) { index ->
            val current = withMaterializedFixture(fixture) { file ->
                decodeNdkFile(fixture, file, maxEdge).pixelSha256
            }
            digest?.let { check(it == current) { "repeated import $index changed digest" } }
            digest = current
            assertNoNativeResources("after repeated import $index")
        }
        report.appendLine(
            record(
                "AIMAGE_REPEAT_V1",
                "count" to REPEATED_IMPORT_COUNT,
                "pixel_sha256" to digest,
                "resources" to "0,0",
            ),
        )
    }

    private fun runDngPolicyGates(
        fixture: QualificationFixture,
        report: StringBuilder,
    ) {
        expectFallback("DNG without explicit fallback flag") {
            AImageDecoderExperiment.probe(
                directSource(fixture.bytes),
                fixture.mime,
                allowDisplayReferredDngFallback = false,
            )
        }
        expectFallback("DNG without exact declared MIME") {
            AImageDecoderExperiment.probe(
                directSource(fixture.bytes),
                declaredMime = null,
                allowDisplayReferredDngFallback = true,
            )
        }
        val legitimate = AImageDecoderExperiment.probe(
            directSource(fixture.bytes),
            fixture.mime,
            allowDisplayReferredDngFallback = true,
        )
        fun reboundHeader(declaredMime: String, allowDng: Boolean): AImageHeader =
            createAImageHeader(
                width = legitimate.width,
                height = legitimate.height,
                platformDefaultFormat = legitimate.platformDefaultFormat,
                alphaFlags = legitimate.alphaFlags,
                dataSpace = legitimate.dataSpace,
                encodedByteCount = legitimate.encodedByteCount,
                inputKind = legitimate.inputKind,
                platformMime = legitimate.platformMime,
                nativeWire = legitimate.nativeProbeWire(),
                declaredMimePolicy = declaredMime,
                displayReferredDngFallback = true,
                allowDngFallbackPolicy = allowDng,
            )
        expectFallback("DNG decode with rebound fallback flag") {
            val plan = planAImageDecode(
                reboundHeader(fixture.mime, allowDng = false),
                EXPORT_MAX_EDGE,
            )
            AImageDecoderExperiment.decodePixels(directSource(fixture.bytes), plan).close()
        }
        expectFallback("DNG decode with rebound declared MIME") {
            val plan = planAImageDecode(
                reboundHeader("image/tiff", allowDng = true),
                EXPORT_MAX_EDGE,
            )
            AImageDecoderExperiment.decodePixels(directSource(fixture.bytes), plan).close()
        }
        assertNoNativeResources("after DNG policy gates")
        report.appendLine(
            record(
                "AIMAGE_DNG_POLICY_V1",
                "libraw_first_required_for_future_integration" to true,
                "libraw_first_runtime_enforced" to false,
                "display_referred_fallback_only" to true,
                "exact_mime_required" to true,
                "decode_rebound_flag_rejected" to true,
                "decode_rebound_mime_rejected" to true,
                "scene_linear_claim" to false,
            ),
        )
    }

    private fun runHostileDng(
        fixture: QualificationFixture,
        report: StringBuilder,
        maxEdge: Int,
    ) {
        assertNoNativeResources("before hostile DNG")
        assertMemoryCoordinatorIdle("before hostile DNG")
        val fdBefore = openFileDescriptorCount()
        var fdAfter = fdBefore
        val outcome = try {
            withMaterializedFixture(fixture) { file ->
                try {
                    "bounded-success:${decodeNdkFile(fixture, file, maxEdge).pixelSha256}"
                } catch (fallback: AImageDecoderFallbackException) {
                    "typed-fallback:${fallback.javaClass.name}"
                }
                // Deliberately do not catch Throwable/Error/unknown RuntimeException.
                // A crash, OOM, JNI linkage error, or contract bug is a failed cell,
                // not a hostile-input success label.
            }
        } finally {
            // Budgets are checked even while an unexpected failure unwinds. If
            // these pass, the original Throwable/Error continues to the runner.
            assertNoNativeResources("after hostile DNG")
            assertMemoryCoordinatorIdle("after hostile DNG")
            fdAfter = openFileDescriptorCount()
            check(fdAfter == fdBefore) {
                "hostile DNG changed process fd budget: $fdBefore -> $fdAfter"
            }
        }
        report.appendLine(
            record(
                "AIMAGE_HOSTILE_DNG_V2",
                "outcome" to outcome,
                "native_resources" to "0,0",
                "coordinator_current_bytes" to 0,
                "fd_before" to fdBefore,
                "fd_after" to fdAfter,
            ),
        )
    }

    private fun verifyOrientationEvidence(
        fixture: QualificationFixture,
        ndk: DecodeObservation,
        java: DecodeObservation,
    ) {
        if (fixture.name != "project-gradient-orientation-6.jpg") return
        check(ndk.headerWidth == 12 && ndk.headerHeight == 16) {
            "NDK decoder did not report the EXIF-oriented 12x16 geometry: $ndk"
        }
        check(java.headerWidth == 12 && java.headerHeight == 16) {
            "Java decoder did not report the EXIF-oriented 12x16 geometry: $java"
        }
        check(ndk.outputWidth == 12 && ndk.outputHeight == 16)
        check(java.outputWidth == 12 && java.outputHeight == 16)
    }

    private fun verifyDecodedSampleEvidence(
        fixture: QualificationFixture,
        ndk: DecodeObservation,
        java: DecodeObservation,
        report: StringBuilder,
    ) {
        check(ndk.headerWidth == java.headerWidth && ndk.headerHeight == java.headerHeight) {
            "${fixture.name}: NDK/Java oriented header geometry differs"
        }
        check(ndk.outputWidth == java.outputWidth && ndk.outputHeight == java.outputHeight) {
            "${fixture.name}: NDK/Java output geometry differs"
        }

        val comparableOpaqueSrgb =
            ndk.finalConfig == AImagePixelFormat.RGBA_8888.name &&
                ndk.alpha == ALPHA_OPAQUE.toString() &&
                ndk.dataSpace == AImageDataSpace.SRGB &&
                java.finalConfig == Bitmap.Config.ARGB_8888.name &&
                java.dataSpace == AImageDataSpace.SRGB
        if (comparableOpaqueSrgb) {
            check(ndk.pixelSha256 == java.pixelSha256) {
                "${fixture.name}: opaque-sRGB decoded RGBA samples differ"
            }
            check(ndk.linearSha256 == java.linearSha256) {
                "${fixture.name}: opaque-sRGB linear-ProPhoto samples differ"
            }
        }

        if (fixture.name == "project-alpha.png" ||
            fixture.name == "translucent-green-p3.png"
        ) {
            check(ndk.alpha == ALPHA_UNPREMUL.toString() && ndk.hiddenRgbPreserved) {
                "${fixture.name}: NDK route did not preserve unpremultiplied alpha evidence"
            }
        }
        val requiredF16DataSpace = when (fixture.name) {
            // Android treats untagged 16-bit sRGB PNG as Extended sRGB.
            "blue-16bit-srgb.png" -> AImageDataSpace.SCRGB
            "project-rgba16-oracle.png" -> AImageDataSpace.SCRGB
            "translucent-green-p3.png" -> AImageDataSpace.DISPLAY_P3
            "red-hlg-profile.png" -> AImageDataSpace.BT2020_HLG
            "red-pq-profile.png" -> AImageDataSpace.BT2020_PQ
            "sample_1mp.dng" -> ndk.dataSpace
            else -> null
        }
        if (requiredF16DataSpace != null) {
            check(ndk.finalConfig == AImagePixelFormat.RGBA_F16.name) {
                "${fixture.name}: high-depth/wide/HDR fallback was not preserved as RGBA_F16"
            }
            check(ndk.dataSpace == requiredF16DataSpace) {
                "${fixture.name}: dataspace ${ndk.dataSpace} != $requiredF16DataSpace"
            }
            check(ndk.linearSha256 == "not-admitted") {
                "${fixture.name}: unqualified F16 input entered the working-color contract"
            }
        }

        val evidence = ndk.samples
        check(evidence.finite) { "${fixture.name}: decoded sample contains NaN/Infinity" }
        check(evidence.alphaMin >= 0f && evidence.alphaMax <= 1f) {
            "${fixture.name}: alpha escaped [0,1]: $evidence"
        }

        val oracleStatus: String
        val hdrHeadroomQualified: Boolean
        when (fixture.name) {
            "project-rgba16-oracle.png" -> {
                val expected = RGBA16_ORACLE.map { source ->
                    RgbaSample(
                        source[0] / 65_535f,
                        source[1] / 65_535f,
                        source[2] / 65_535f,
                        source[3] / 65_535f,
                    )
                }
                check(evidence.firstSamples.size >= expected.size)
                expected.forEachIndexed { index, sample ->
                    check(evidence.firstSamples[index].approximately(sample, HALF_SAMPLE_TOLERANCE)) {
                        "RGBA16 source oracle mismatch at $index: " +
                            "${evidence.firstSamples[index]} != $sample"
                    }
                }
                check(evidence.offEightBitRgbSamples > 0) {
                    "RGBA16 oracle collapsed onto the 8-bit sample lattice"
                }
                check(evidence.transparentNonZeroRgbSamples > 0) {
                    "RGBA16 oracle lost RGB beneath alpha=0"
                }
                oracleStatus = "source-sample-pass"
                hdrHeadroomQualified = false
            }
            "project-alpha.png" -> {
                check(evidence.transparentNonZeroRgbSamples > 0) {
                    "project alpha oracle lost hidden RGB at alpha=0"
                }
                oracleStatus = "source-alpha-pass"
                hdrHeadroomQualified = false
            }
            "translucent-green-p3.png" -> {
                // AOSP CTS pins 0x3a7d267f as this Bitmap's raw *premultiplied*
                // Display-P3 RGBA sample. This route explicitly requests
                // unpremultiplied output, so compare against the independently
                // published CTS sample divided by its alpha.
                val alpha = 0x7f / 255f
                val expected = RgbaSample(
                    (0x3a / 255f) / alpha,
                    (0x7d / 255f) / alpha,
                    (0x26 / 255f) / alpha,
                    alpha,
                )
                check(evidence.firstSamples.first().approximately(expected, P3_SAMPLE_TOLERANCE)) {
                    "P3 alpha source oracle mismatch: ${evidence.firstSamples.first()} != $expected"
                }
                oracleStatus = "source-sample-pass"
                hdrHeadroomQualified = false
            }
            "red-hlg-profile.png", "red-pq-profile.png" -> {
                check(evidence.firstSamples.isNotEmpty())
                // Exact first-texel RGBA code values from the digest-pinned PNG
                // payloads. Matching these proves channel/code preservation, but
                // does not provide the absolute-nits oracle needed to qualify HDR
                // headroom or absence of display tone mapping.
                val expected = when (fixture.name) {
                    "red-hlg-profile.png" -> RgbaSample(
                        130 / 255f,
                        43 / 255f,
                        21 / 255f,
                        1f,
                    )
                    else -> RgbaSample(
                        105 / 255f,
                        60 / 255f,
                        38 / 255f,
                        1f,
                    )
                }
                check(evidence.firstSamples.all { sample ->
                    sample.approximately(expected, HALF_SAMPLE_TOLERANCE)
                }) {
                    "${fixture.name}: encoded source-sample mismatch; " +
                        "actual=${evidence.firstSamples} expected=$expected"
                }
                // The pinned file proves transfer metadata and exact encoded samples,
                // but carries no independent absolute-nits/reference-output oracle.
                // Keep tone-map/headroom qualification explicitly blocked.
                oracleStatus = "source-code-sample-pass-headroom-unqualified"
                hdrHeadroomQualified = false
            }
            else -> {
                oracleStatus = "finite-alpha-bounds-only"
                hdrHeadroomQualified = false
            }
        }
        report.appendLine(
            record(
                "AIMAGE_SAMPLE_ORACLE_V2",
                "fixture" to fixture.name,
                "status" to oracleStatus,
                "finite" to evidence.finite,
                "alpha_min" to evidence.alphaMin,
                "alpha_max" to evidence.alphaMax,
                "off_8bit_rgb_samples" to evidence.offEightBitRgbSamples,
                "transparent_nonzero_rgb_samples" to evidence.transparentNonZeroRgbSamples,
                "hdr_headroom_tonemap_qualified" to hdrHeadroomQualified,
            ),
        )
    }

    private fun runGeneratedPhonePerformance(
        report: StringBuilder,
        repeats: Int,
        warmups: Int,
    ) {
        val file = File.createTempFile("sfaimage-phone-4080x3060-", ".bmp", targetContext.cacheDir)
        try {
            generatePhoneBmp(file)
            check(file.length() == PHONE_BMP_BYTES) {
                "generated phone fixture has ${file.length()} bytes, expected $PHONE_BMP_BYTES"
            }
            // Generation, file read and source digest all finish before the
            // benchmark establishes a memory baseline or starts a clock.
            val sourceDigest = sha256(file)
            val subject = BenchmarkSubject(
                name = "generated-phone-4080x3060.bmp",
                provenance = "Spektrafilm deterministic BMP generator v1",
                sourceSha256 = sourceDigest,
                sourceBytes = file.length(),
                mime = "image/bmp",
                kind = AImageInputKind.BMP,
                encodedOrientation = 1,
            )
            val contract = DecodeSourceContract(
                name = subject.name,
                mime = subject.mime,
                kind = subject.kind,
                encodedOrientation = subject.encodedOrientation,
                displayReferredDngFallback = false,
            )
            runGeneratedPhoneCase(
                report = report,
                subject = subject,
                contract = contract,
                file = file,
                caseName = "preview_2048_policy",
                ndkMaxEdge = PREVIEW_MAX_EDGE,
                javaMaxEdge = PREVIEW_MAX_EDGE,
                expectedNdkWidth = 2_048,
                expectedNdkHeight = 1_536,
                expectedJavaWidth = 2_040,
                expectedJavaHeight = 1_530,
                performanceEligible = false,
                repeats = repeats,
                warmups = warmups,
            )
            runGeneratedPhoneCase(
                report = report,
                subject = subject,
                contract = contract,
                file = file,
                caseName = "export_full_resolution",
                ndkMaxEdge = EXPORT_MAX_EDGE,
                javaMaxEdge = EXPORT_MAX_EDGE,
                expectedNdkWidth = PHONE_WIDTH,
                expectedNdkHeight = PHONE_HEIGHT,
                expectedJavaWidth = PHONE_WIDTH,
                expectedJavaHeight = PHONE_HEIGHT,
                performanceEligible = true,
                repeats = repeats,
                warmups = warmups,
            )
        } finally {
            check(file.delete() || !file.exists()) {
                "could not remove generated phone fixture ${file.absolutePath}"
            }
        }
    }

    private fun runGeneratedPhoneCase(
        report: StringBuilder,
        subject: BenchmarkSubject,
        contract: DecodeSourceContract,
        file: File,
        caseName: String,
        ndkMaxEdge: Int,
        javaMaxEdge: Int,
        expectedNdkWidth: Int,
        expectedNdkHeight: Int,
        expectedJavaWidth: Int,
        expectedJavaHeight: Int,
        performanceEligible: Boolean,
        repeats: Int,
        warmups: Int,
    ) {
        val ndk = decodeNdkFile(contract, file, ndkMaxEdge)
        val java = decodeJavaCurrentSemantics(contract, file, javaMaxEdge)
        check(ndk.outputWidth == expectedNdkWidth && ndk.outputHeight == expectedNdkHeight) {
            "$caseName NDK geometry ${ndk.outputWidth}x${ndk.outputHeight} != " +
                "${expectedNdkWidth}x$expectedNdkHeight"
        }
        check(java.outputWidth == expectedJavaWidth && java.outputHeight == expectedJavaHeight) {
            "$caseName Java geometry ${java.outputWidth}x${java.outputHeight} != " +
                "${expectedJavaWidth}x$expectedJavaHeight"
        }
        val comparable = isWorkingEndpointComparable(ndk, java)
        report.appendLine(
            record(
                "AIMAGE_PHONE_CORRECTNESS_V1",
                "case" to caseName,
                "source_geometry" to "${PHONE_WIDTH}x$PHONE_HEIGHT",
                "ndk_max_edge" to ndkMaxEdge,
                "java_max_edge" to javaMaxEdge,
                "ndk_output_geometry" to "${expectedNdkWidth}x$expectedNdkHeight",
                "java_output_geometry" to "${expectedJavaWidth}x$expectedJavaHeight",
                "source_sha256" to subject.sourceSha256,
                "java_route" to currentJavaRouteName(contract),
                "ndk_pixel_sha256" to ndk.pixelSha256,
                "java_pixel_sha256" to java.pixelSha256,
                "ndk_linear_sha256" to ndk.linearSha256,
                "java_linear_sha256" to java.linearSha256,
                "identical_endpoint" to comparable,
                "generation_before_timing" to true,
                "performance_measured" to performanceEligible,
            ),
        )
        report.appendLine(
            routeInventoryRecord(
                subject,
                caseName,
                "ndk_encoded_source_to_candidate_output",
                ndk.inventory,
            ),
        )
        report.appendLine(
            routeInventoryRecord(
                subject,
                caseName,
                currentJavaRouteName(contract),
                java.inventory,
            ),
        )
        if (!performanceEligible) {
            report.appendLine(
                record(
                    "AIMAGE_PHONE_PERF_UNQUALIFIED_V1",
                    "case" to caseName,
                    "reason" to
                        "actual_2048_bound_has_different_geometry_and_output_ownership",
                    "timed" to false,
                ),
            )
            assertNoNativeResources("after generated phone $caseName")
            assertMemoryCoordinatorIdle("after generated phone $caseName")
            return
        }
        check(comparable) {
            "$caseName routes do not reach the same opaque-sRGB linear f32 endpoint"
        }
        val endpoint = ProductionEndpoint.from(ndk)
        report.appendLine(
            benchmark(
                subject,
                caseName,
                "ndk_fd_to_linear_prophoto_f32",
                endpoint,
                repeats,
                warmups,
            ) { decodeNdkProduction(contract, file, ndkMaxEdge) }.record(),
        )
        report.appendLine(
            benchmark(
                subject,
                caseName,
                currentJavaRouteName(contract),
                endpoint,
                repeats,
                warmups,
            ) { decodeJavaProduction(contract, file, javaMaxEdge) }.record(),
        )
        assertNoNativeResources("after generated phone $caseName")
        assertMemoryCoordinatorIdle("after generated phone $caseName")
    }

    private fun generatePhoneBmp(file: File) {
        val rowBytes = Math.multiplyExact(PHONE_WIDTH, 3)
        check(rowBytes % 4 == 0)
        val imageBytes = Math.multiplyExact(rowBytes, PHONE_HEIGHT)
        val totalBytes = Math.addExact(BMP_HEADER_BYTES, imageBytes)
        check(totalBytes.toLong() == PHONE_BMP_BYTES)
        val header = ByteBuffer.allocate(BMP_HEADER_BYTES).order(ByteOrder.LITTLE_ENDIAN).apply {
            put('B'.code.toByte())
            put('M'.code.toByte())
            putInt(totalBytes)
            putInt(0)
            putInt(BMP_HEADER_BYTES)
            putInt(40)
            putInt(PHONE_WIDTH)
            putInt(PHONE_HEIGHT)
            putShort(1.toShort())
            putShort(24.toShort())
            putInt(0)
            putInt(imageBytes)
            putInt(2_835)
            putInt(2_835)
            putInt(0)
            putInt(0)
        }.array()
        val row = ByteArray(rowBytes)
        FileOutputStream(file, false).use { output ->
            output.write(header)
            repeat(PHONE_HEIGHT) { y ->
                var offset = 0
                repeat(PHONE_WIDTH) { x ->
                    row[offset++] = ((x * 13 + y * 7) and 0xff).toByte()
                    row[offset++] = ((x * 3 + y * 17) and 0xff).toByte()
                    row[offset++] = ((x * 19 + y * 5) and 0xff).toByte()
                }
                output.write(row)
            }
            output.fd.sync()
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().toHex()
    }

    private inline fun <T> withMaterializedFixture(
        fixture: QualificationFixture,
        block: (File) -> T,
    ): T {
        val suffix = "." + fixture.name.substringAfterLast('.', "img")
        val file = File.createTempFile("sfaimage-", suffix, targetContext.cacheDir)
        try {
            FileOutputStream(file, false).use { output ->
                output.write(fixture.bytes)
                output.fd.sync()
            }
            check(file.length() == fixture.bytes.size.toLong())
            return block(file)
        } finally {
            check(file.delete() || !file.exists()) { "could not remove ${file.absolutePath}" }
        }
    }

    private fun directSource(bytes: ByteArray): AImageEncodedSource {
        val buffer = ByteBuffer.allocateDirect(bytes.size)
        buffer.put(bytes).flip()
        return AImageEncodedSource.fromDirectBuffer(buffer)
    }

    private fun expectFallback(label: String, block: () -> Unit) {
        val failure = runCatching(block).exceptionOrNull()
        check(failure is AImageDecoderFallbackException) {
            "$label expected AImageDecoderFallbackException, got $failure"
        }
    }

    private fun assertNoNativeResources(label: String) {
        val outstanding = AImageDecoderExperiment.debugOutstandingNativeResources()
        check(outstanding.contentEquals(longArrayOf(0L, 0L))) {
            "$label leaked native resources: ${outstanding.joinToString()}"
        }
    }

    private fun assertMemoryCoordinatorIdle(label: String) {
        val snapshot = SpektraEngine.memoryBudgetSnapshotJson()
        val current = Regex("\\\"total\\\":\\{\\\"current_bytes\\\":(\\d+)")
            .find(snapshot)?.groupValues?.get(1)?.toLongOrNull()
            ?: error("$label could not parse memory coordinator snapshot: $snapshot")
        check(current == 0L) { "$label retained $current coordinator bytes: $snapshot" }
    }

    private fun openFileDescriptorCount(): Int =
        File("/proc/self/fd").list()?.size
            ?: error("could not inspect /proc/self/fd")

    private fun argumentInt(name: String, default: Int, min: Int, max: Int): Int {
        val value = arguments.getString(name)?.toIntOrNull() ?: default
        check(value in min..max) { "$name must be in [$min,$max]" }
        return value
    }

    private fun argumentBoolean(name: String, default: Boolean): Boolean {
        val raw = arguments.getString(name) ?: return default
        check(raw.equals("true", true) || raw.equals("false", true)) {
            "$name must be true or false"
        }
        return raw.toBooleanStrictOrNull() ?: raw.equals("true", true)
    }

    private fun recreationMarker(): File =
        File(targetContext.filesDir, "aimage-decoder-recreation-v3.tsv")

    private fun deviceRecord(repeats: Int, warmups: Int, maxEdge: Int): String = record(
        "AIMAGE_DEVICE_V1",
        "api" to Build.VERSION.SDK_INT,
        "release" to Build.VERSION.RELEASE,
        "fingerprint" to Build.FINGERPRINT,
        "manufacturer" to Build.MANUFACTURER,
        "model" to Build.MODEL,
        "abis" to Build.SUPPORTED_ABIS.joinToString(","),
        "repeats" to repeats,
        "warmups" to warmups,
        "max_edge" to maxEdge,
        "memory_budget" to DEFAULT_MEMORY_BUDGET_BYTES,
        "target_minified" to BuildConfig.AIMAGE_TARGET_MINIFIED,
        "r8_canary_runtime_name" to AImageDecoderNative.r8CanaryRuntimeClassName(),
        "r8_canary_renamed" to
            (AImageDecoderNative.r8CanaryRuntimeClassName() != AIMAGE_R8_CANARY_ORIGINAL_NAME),
    )

    private companion object {
        const val ARG_CORPUS_DIR = "aimage_corpus_dir"
        const val ARG_EXPECTED_API = "aimage_expected_api"
        const val ARG_MAX_EDGE = "aimage_max_edge"
        const val ARG_REPEATS = "aimage_repeats"
        const val ARG_WARMUPS = "aimage_warmups"
        const val ARG_REQUIRE_FULL_CORPUS = "aimage_require_full_corpus"
        const val ARG_REQUIRE_MINIFIED = "aimage_require_minified"
        const val ARG_RECREATION_PHASE = "aimage_recreation_phase"
        const val FALLBACK_EXCEPTION_BINARY_NAME =
            "com.spectrafilm.aimage.AImageDecoderFallbackException"
        const val DEVICE_CORPUS_DIR = "/data/local/tmp/sfaimage-corpus"
        const val REPRESENTATIVE_DNG = "sample_1mp.dng"
        const val HOSTILE_DNG = "bug_156261521.dng"
        const val DIRECT_CONSISTENCY_LIMIT_BYTES = 2 * 1024 * 1024
        const val DEFAULT_MEMORY_BUDGET_BYTES = 512L * 1024L * 1024L
        const val DENIAL_MEMORY_BUDGET_BYTES = 512L
        const val REPEATED_IMPORT_COUNT = 50
        const val ALPHA_PREMUL = 0
        const val ALPHA_OPAQUE = 1
        const val ALPHA_UNPREMUL = 2
    }
}

private data class DecodeSourceContract(
    val name: String,
    val mime: String,
    val kind: AImageInputKind,
    val encodedOrientation: Int,
    val displayReferredDngFallback: Boolean,
)

private fun QualificationFixture.decodeContract() = DecodeSourceContract(
    name = name,
    mime = mime,
    kind = kind,
    encodedOrientation = encodedOrientation,
    displayReferredDngFallback = displayReferredDngFallback,
)

private fun currentJavaRouteName(contract: DecodeSourceContract): String =
    if (contract.displayReferredDngFallback) {
        "java_current_imagedecoder_dng_fallback_to_owned_linear_prophoto_f32"
    } else {
        "java_current_bitmapfactory_to_owned_linear_prophoto_f32"
    }

private data class BenchmarkSubject(
    val name: String,
    val provenance: String,
    val sourceSha256: String,
    val sourceBytes: Long,
    val mime: String,
    val kind: AImageInputKind,
    val encodedOrientation: Int,
)

private fun QualificationFixture.benchmarkSubject() = BenchmarkSubject(
    name = name,
    provenance = provenance,
    sourceSha256 = expectedSha256,
    sourceBytes = bytes.size.toLong(),
    mime = mime,
    kind = kind,
    encodedOrientation = encodedOrientation,
)

private data class PixelRead(val sha256: String, val samples: PixelEvidence)

private data class RgbaSample(
    val r: Float,
    val g: Float,
    val b: Float,
    val a: Float,
) {
    fun approximately(other: RgbaSample, tolerance: Float): Boolean =
        abs(r - other.r) <= tolerance &&
            abs(g - other.g) <= tolerance &&
            abs(b - other.b) <= tolerance &&
            abs(a - other.a) <= tolerance
}

private data class PixelEvidence(
    val finite: Boolean,
    val alphaMin: Float,
    val alphaMax: Float,
    val offEightBitRgbSamples: Long,
    val transparentNonZeroRgbSamples: Long,
    val firstSamples: List<RgbaSample>,
)

private data class JavaPreparedBitmap(
    val bitmap: Bitmap,
    val headerWidth: Int,
    val headerHeight: Int,
    val headerMime: String,
    val headerColorSpace: String,
    val decodedConfig: String,
) : AutoCloseable {
    override fun close() {
        bitmap.recycle()
    }
}

private data class DecodeRouteInventory(
    val measurementScope: String,
    val knownDataAllocations: Int,
    val knownAllocatedBytesTotal: Long,
    val decodeOutputWrites: Int,
    val boundaryCopies: Int,
    val transformWrites: Int,
    val operations: String,
    val platformInternalAllocationsMeasured: Boolean = false,
)

private data class BitmapInventorySnapshot(
    val allocations: Int,
    val allocatedBytesTotal: Long,
    val explicitCopies: Int,
    val transformWrites: Int,
    val operations: String,
)

private class MutableBitmapInventory {
    private var allocations = 0
    private var allocatedBytesTotal = 0L
    private var explicitCopies = 0
    private var transformWrites = 0
    private val operations = ArrayList<String>()

    fun recordDecode(bitmap: Bitmap, operation: String) =
        recordAllocation(bitmap, operation)

    fun recordCopy(bitmap: Bitmap, operation: String) {
        explicitCopies++
        recordAllocation(bitmap, operation)
    }

    fun recordTransform(bitmap: Bitmap, operation: String) {
        transformWrites++
        recordAllocation(bitmap, operation)
    }

    fun snapshot(): BitmapInventorySnapshot = BitmapInventorySnapshot(
        allocations = allocations,
        allocatedBytesTotal = allocatedBytesTotal,
        explicitCopies = explicitCopies,
        transformWrites = transformWrites,
        operations = operations.joinToString(">"),
    )

    private fun recordAllocation(bitmap: Bitmap, operation: String) {
        allocations++
        allocatedBytesTotal = Math.addExact(
            allocatedBytesTotal,
            bitmap.allocationByteCount.toLong(),
        )
        operations += operation
    }
}

private data class DecodeObservation(
    val headerWidth: Int,
    val headerHeight: Int,
    val outputWidth: Int,
    val outputHeight: Int,
    val headerMime: String,
    val decodedConfig: String,
    val finalConfig: String,
    val alpha: String,
    val dataSpace: Int,
    val colorSpace: String,
    val pixelSha256: String,
    val linearSha256: String,
    val hiddenRgbPreserved: Boolean,
    val samples: PixelEvidence,
    val inventory: DecodeRouteInventory,
)

private fun isWorkingEndpointComparable(
    ndk: DecodeObservation,
    java: DecodeObservation,
): Boolean =
    ndk.outputWidth == java.outputWidth &&
        ndk.outputHeight == java.outputHeight &&
        ndk.finalConfig == AImagePixelFormat.RGBA_8888.name &&
        ndk.alpha == "1" &&
        ndk.dataSpace == AImageDataSpace.SRGB &&
        java.decodedConfig == Bitmap.Config.ARGB_8888.name &&
        java.finalConfig == Bitmap.Config.ARGB_8888.name &&
        java.alpha == "opaque" &&
        java.dataSpace == AImageDataSpace.SRGB &&
        ndk.pixelSha256 == java.pixelSha256 &&
        ndk.linearSha256 == java.linearSha256 &&
        ndk.linearSha256 != "not-admitted"

private data class ProductionEndpoint(
    val width: Int,
    val height: Int,
    val colorSpace: String,
    val sampleFormat: String,
    val byteCount: Long,
) {
    companion object {
        fun from(observation: DecodeObservation): ProductionEndpoint {
            check(observation.linearSha256 != "not-admitted")
            return ProductionEndpoint(
                width = observation.outputWidth,
                height = observation.outputHeight,
                colorSpace = "ProPhoto RGB",
                sampleFormat = "packed-rgb-f32",
                byteCount = Math.multiplyExact(
                    Math.multiplyExact(
                        observation.outputWidth.toLong(),
                        observation.outputHeight.toLong(),
                    ),
                    3L * Float.SIZE_BYTES,
                ),
            )
        }
    }
}

private class ProductionResult(
    private val image: com.spectrafilm.engine.LinearImage,
) : AutoCloseable {
    val endpoint = ProductionEndpoint(
        width = image.width,
        height = image.height,
        colorSpace = image.colorSpace,
        sampleFormat = "packed-rgb-f32",
        byteCount = Math.multiplyExact(
            Math.multiplyExact(image.width.toLong(), image.height.toLong()),
            3L * Float.SIZE_BYTES,
        ),
    )

    override fun close() {
        image.close()
    }
}

private data class MemoryEvidence(
    val baselinePssKb: Long,
    val peakPssKb: Long,
    val baselineNativeBytes: Long,
    val peakNativeBytes: Long,
    val baselineJvmBytes: Long,
    val peakJvmBytes: Long,
)

private data class BenchmarkResult(
    val subject: BenchmarkSubject,
    val caseName: String,
    val route: String,
    val endpoint: ProductionEndpoint,
    val latenciesNanos: LongArray,
    val memory: MemoryEvidence,
) {
    fun record(): String {
        val sorted = latenciesNanos.sortedArray()
        val p50 = percentile(sorted, 0.50)
        val p95 = percentile(sorted, 0.95)
        return record(
            "AIMAGE_PERF_V2",
            "fixture" to subject.name,
            "case" to caseName,
            "provenance" to subject.provenance,
            "source_sha256" to subject.sourceSha256,
            "source_bytes" to subject.sourceBytes,
            "mime_declared" to subject.mime,
            "kind" to subject.kind,
            "encoded_orientation" to subject.encodedOrientation,
            "route" to route,
            "endpoint_geometry" to "${endpoint.width}x${endpoint.height}",
            "endpoint_color_space" to endpoint.colorSpace,
            "endpoint_sample_format" to endpoint.sampleFormat,
            "endpoint_bytes" to endpoint.byteCount,
            "timed_source_generation" to false,
            "timed_source_hash" to false,
            "timed_corpus_materialization" to false,
            "timed_intrinsic_decode_input_io" to true,
            "timed_output_hash" to false,
            "timed_cleanup" to false,
            "latency_min_ns" to sorted.first(),
            "latency_p50_ns" to p50,
            "latency_p95_ns" to p95,
            "latency_max_ns" to sorted.last(),
            "pss_baseline_kb" to memory.baselinePssKb,
            "pss_peak_kb" to memory.peakPssKb,
            "native_baseline_bytes" to memory.baselineNativeBytes,
            "native_peak_bytes" to memory.peakNativeBytes,
            "jvm_baseline_bytes" to memory.baselineJvmBytes,
            "jvm_peak_bytes" to memory.peakJvmBytes,
        )
    }
}

private object PeakMemorySampler {
    fun measure(
        expected: ProductionEndpoint,
        operation: () -> ProductionResult,
    ): MemoryEvidence {
        val running = AtomicBoolean(true)
        val baseline = sample()
        val peakPss = AtomicLong(baseline.pssKb)
        val peakNative = AtomicLong(baseline.nativeBytes)
        val peakJvm = AtomicLong(baseline.jvmBytes)
        val sampler = thread(start = true, isDaemon = true, name = "aimage-memory-sampler") {
            while (running.get()) {
                val current = sample()
                peakPss.accumulateAndGet(current.pssKb) { left, right -> maxOf(left, right) }
                peakNative.accumulateAndGet(current.nativeBytes) { left, right -> maxOf(left, right) }
                peakJvm.accumulateAndGet(current.jvmBytes) { left, right -> maxOf(left, right) }
                SystemClock.sleep(1)
            }
        }
        var result: ProductionResult? = null
        try {
            result = operation()
            check(result.endpoint == expected)
            // Sample while the identical owned f32 endpoint is live. Cleanup is
            // intentionally outside the timed distribution but inside leak checks.
            val endpoint = sample()
            peakPss.accumulateAndGet(endpoint.pssKb) { left, right -> maxOf(left, right) }
            peakNative.accumulateAndGet(endpoint.nativeBytes) { left, right -> maxOf(left, right) }
            peakJvm.accumulateAndGet(endpoint.jvmBytes) { left, right -> maxOf(left, right) }
        } finally {
            // Stop the observer before output cleanup: cleanup and its memory
            // sampling are outside the measured production hand-off window.
            running.set(false)
            sampler.join(5_000)
            val samplerStopped = !sampler.isAlive
            result?.close()
            check(samplerStopped) { "memory sampler did not stop" }
        }
        return MemoryEvidence(
            baselinePssKb = baseline.pssKb,
            peakPssKb = peakPss.get(),
            baselineNativeBytes = baseline.nativeBytes,
            peakNativeBytes = peakNative.get(),
            baselineJvmBytes = baseline.jvmBytes,
            peakJvmBytes = peakJvm.get(),
        )
    }

    private data class Sample(val pssKb: Long, val nativeBytes: Long, val jvmBytes: Long)

    private fun sample(): Sample {
        val info = Debug.MemoryInfo()
        Debug.getMemoryInfo(info)
        val runtime = Runtime.getRuntime()
        return Sample(
            pssKb = info.totalPss.toLong(),
            nativeBytes = Debug.getNativeHeapAllocatedSize(),
            jvmBytes = runtime.totalMemory() - runtime.freeMemory(),
        )
    }
}

private fun percentile(sorted: LongArray, fraction: Double): Long {
    check(sorted.isNotEmpty())
    val index = (ceil(sorted.size * fraction).toInt() - 1).coerceIn(sorted.indices)
    return sorted[index]
}

private fun srgbToLinear(value: Float): Float = if (value <= 0.04045f) {
    value / 12.92f
} else {
    Math.pow(((value + 0.055) / 1.055), 2.4).toFloat()
}

private fun androidDataSpace(colorSpace: ColorSpace?): Int {
    if (colorSpace == null) return AImageDataSpace.UNKNOWN
    if (Build.VERSION.SDK_INT >= 33) return colorSpace.dataSpace
    return when (colorSpace) {
        ColorSpace.get(ColorSpace.Named.SRGB) -> AImageDataSpace.SRGB
        ColorSpace.get(ColorSpace.Named.LINEAR_SRGB) -> AImageDataSpace.SRGB_LINEAR
        ColorSpace.get(ColorSpace.Named.EXTENDED_SRGB) -> AImageDataSpace.SCRGB
        ColorSpace.get(ColorSpace.Named.LINEAR_EXTENDED_SRGB) -> AImageDataSpace.SCRGB_LINEAR
        ColorSpace.get(ColorSpace.Named.DISPLAY_P3) -> AImageDataSpace.DISPLAY_P3
        ColorSpace.get(ColorSpace.Named.BT2020_HLG) -> AImageDataSpace.BT2020_HLG
        ColorSpace.get(ColorSpace.Named.BT2020_PQ) -> AImageDataSpace.BT2020_PQ
        ColorSpace.get(ColorSpace.Named.BT2020) -> AImageDataSpace.BT2020
        else -> AImageDataSpace.UNKNOWN
    }
}

private fun analyzeNativeSamples(
    buffer: ByteBuffer,
    width: Int,
    height: Int,
    format: AImagePixelFormat,
): PixelEvidence {
    val pixelCount = Math.multiplyExact(width, height)
    val expectedBytes = Math.multiplyExact(pixelCount, format.bytesPerPixel)
    val source = buffer.duplicate().order(ByteOrder.nativeOrder()).apply {
        position(0)
        check(capacity() >= expectedBytes)
        limit(expectedBytes)
    }
    val samples = ArrayList<RgbaSample>(minOf(pixelCount, SAMPLE_CAPTURE_COUNT))
    var finite = true
    var alphaMin = Float.POSITIVE_INFINITY
    var alphaMax = Float.NEGATIVE_INFINITY
    var offEightBit = 0L
    var transparentNonZeroRgb = 0L
    repeat(pixelCount) { index ->
        val sample = when (format) {
            AImagePixelFormat.RGBA_8888 -> RgbaSample(
                (source.get().toInt() and 0xff) / 255f,
                (source.get().toInt() and 0xff) / 255f,
                (source.get().toInt() and 0xff) / 255f,
                (source.get().toInt() and 0xff) / 255f,
            )
            AImagePixelFormat.RGBA_F16 -> RgbaSample(
                halfToFloat(source.short),
                halfToFloat(source.short),
                halfToFloat(source.short),
                halfToFloat(source.short),
            )
        }
        if (index < SAMPLE_CAPTURE_COUNT) samples += sample
        finite = finite && sample.r.isFinite() && sample.g.isFinite() &&
            sample.b.isFinite() && sample.a.isFinite()
        alphaMin = minOf(alphaMin, sample.a)
        alphaMax = maxOf(alphaMax, sample.a)
        if (isOffEightBitLattice(sample.r) || isOffEightBitLattice(sample.g) ||
            isOffEightBitLattice(sample.b)
        ) {
            offEightBit++
        }
        if (abs(sample.a) <= TRANSPARENT_EPSILON &&
            (abs(sample.r) > TRANSPARENT_EPSILON ||
                abs(sample.g) > TRANSPARENT_EPSILON ||
                abs(sample.b) > TRANSPARENT_EPSILON)
        ) {
            transparentNonZeroRgb++
        }
    }
    return PixelEvidence(
        finite = finite,
        alphaMin = alphaMin,
        alphaMax = alphaMax,
        offEightBitRgbSamples = offEightBit,
        transparentNonZeroRgbSamples = transparentNonZeroRgb,
        firstSamples = samples,
    )
}

private fun analyzeBitmapSamples(bitmap: Bitmap): PixelEvidence {
    val rowsPerBand = (256 * 1024 / bitmap.width).coerceIn(1, bitmap.height)
    val pixels = IntArray(Math.multiplyExact(bitmap.width, rowsPerBand))
    val samples = ArrayList<RgbaSample>(SAMPLE_CAPTURE_COUNT)
    var alphaMin = 1f
    var alphaMax = 0f
    var transparentNonZeroRgb = 0L
    var y = 0
    while (y < bitmap.height) {
        val rows = minOf(rowsPerBand, bitmap.height - y)
        val count = Math.multiplyExact(bitmap.width, rows)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, y, bitmap.width, rows)
        repeat(count) { index ->
            val argb = pixels[index]
            val sample = RgbaSample(
                ((argb ushr 16) and 0xff) / 255f,
                ((argb ushr 8) and 0xff) / 255f,
                (argb and 0xff) / 255f,
                ((argb ushr 24) and 0xff) / 255f,
            )
            if (samples.size < SAMPLE_CAPTURE_COUNT) samples += sample
            alphaMin = minOf(alphaMin, sample.a)
            alphaMax = maxOf(alphaMax, sample.a)
            if (sample.a == 0f && (sample.r != 0f || sample.g != 0f || sample.b != 0f)) {
                transparentNonZeroRgb++
            }
        }
        y += rows
    }
    return PixelEvidence(
        finite = true,
        alphaMin = alphaMin,
        alphaMax = alphaMax,
        offEightBitRgbSamples = 0,
        transparentNonZeroRgbSamples = transparentNonZeroRgb,
        firstSamples = samples,
    )
}

private fun isOffEightBitLattice(value: Float): Boolean =
    value in 0f..1f && abs(value * 255f - round(value * 255f)) > EIGHT_BIT_EPSILON

private fun halfToFloat(raw: Short): Float {
    val bits = raw.toInt() and 0xffff
    val sign = if ((bits and 0x8000) == 0) 1f else -1f
    val exponent = (bits ushr 10) and 0x1f
    val fraction = bits and 0x03ff
    return when (exponent) {
        0 -> if (fraction == 0) sign * 0f else sign * Math.scalb(fraction.toFloat(), -24)
        0x1f -> if (fraction == 0) sign * Float.POSITIVE_INFINITY else Float.NaN
        else -> sign * Math.scalb((1_024 + fraction).toFloat(), exponent - 25)
    }
}

private fun digest(buffer: ByteBuffer): String {
    val source = buffer.duplicate()
    source.position(0)
    val digest = MessageDigest.getInstance("SHA-256")
    val chunk = ByteArray(16 * 1024)
    while (source.hasRemaining()) {
        val count = minOf(source.remaining(), chunk.size)
        source.get(chunk, 0, count)
        digest.update(chunk, 0, count)
    }
    return digest.digest().toHex()
}

private fun record(schema: String, vararg fields: Pair<String, Any?>): String = buildString {
    append(schema)
    fields.forEach { (name, value) ->
        append('\t')
        append(name)
        append('=')
        append(
            value.toString()
                .replace("\\", "\\\\")
                .replace("\t", "\\t")
                .replace("\r", "\\r")
                .replace("\n", "\\n"),
        )
    }
}

private fun routeInventoryRecord(
    fixture: QualificationFixture,
    route: String,
    inventory: DecodeRouteInventory,
): String = routeInventoryRecord(
    fixtureName = fixture.name,
    sourceSha256 = fixture.expectedSha256,
    caseName = "fixture_correctness",
    route = route,
    inventory = inventory,
)

private fun routeInventoryRecord(
    subject: BenchmarkSubject,
    caseName: String,
    route: String,
    inventory: DecodeRouteInventory,
): String = routeInventoryRecord(
    fixtureName = subject.name,
    sourceSha256 = subject.sourceSha256,
    caseName = caseName,
    route = route,
    inventory = inventory,
)

private fun routeInventoryRecord(
    fixtureName: String,
    sourceSha256: String,
    caseName: String,
    route: String,
    inventory: DecodeRouteInventory,
): String = record(
    "AIMAGE_ROUTE_INVENTORY_V1",
    "fixture" to fixtureName,
    "case" to caseName,
    "source_sha256" to sourceSha256,
    "route" to route,
    "measurement_scope" to inventory.measurementScope,
    "known_data_allocations" to inventory.knownDataAllocations,
    "known_allocated_bytes_total" to inventory.knownAllocatedBytesTotal,
    "decode_output_writes" to inventory.decodeOutputWrites,
    "boundary_copies" to inventory.boundaryCopies,
    "transform_writes" to inventory.transformWrites,
    "operations" to inventory.operations,
    "platform_internal_allocations_measured" to
        inventory.platformInternalAllocationsMeasured,
    "platform_internal_allocations" to "unobservable",
    "hashing_and_sample_analysis_included" to false,
)

private fun canonicalRgba8888Digest(bitmap: Bitmap): String {
    val digest = MessageDigest.getInstance("SHA-256")
    val rowsPerBand = (256 * 1024 / bitmap.width).coerceIn(1, bitmap.height)
    val pixelCapacity = Math.multiplyExact(bitmap.width, rowsPerBand)
    val pixels = IntArray(pixelCapacity)
    val rgba = ByteArray(Math.multiplyExact(pixelCapacity, 4))
    var y = 0
    while (y < bitmap.height) {
        val rows = minOf(rowsPerBand, bitmap.height - y)
        val count = Math.multiplyExact(bitmap.width, rows)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, y, bitmap.width, rows)
        var target = 0
        repeat(count) { index ->
            val argb = pixels[index]
            rgba[target++] = ((argb ushr 16) and 0xff).toByte()
            rgba[target++] = ((argb ushr 8) and 0xff).toByte()
            rgba[target++] = (argb and 0xff).toByte()
            rgba[target++] = ((argb ushr 24) and 0xff).toByte()
        }
        digest.update(rgba, 0, target)
        y += rows
    }
    return digest.digest().toHex()
}
