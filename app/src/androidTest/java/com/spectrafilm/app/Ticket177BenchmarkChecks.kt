/*
 * Spektrafilm for Android — release export-digest benchmark harness (#177). GPLv3.
 */
package com.spectrafilm.app

import android.app.ActivityManager
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.Debug
import android.os.PowerManager
import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.RenderCancellation
import com.spectrafilm.engine.RenderKind
import com.spectrafilm.engine.SimResult
import com.spectrafilm.app.masks.MaskCompositor
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import java.security.MessageDigest
import java.util.Locale
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject

/**
 * Runs the pinned #177 corpus against the exact installed release candidate and writes a
 * machine-readable capture. It measures the shipping export path — the same decode, engine,
 * grade, encoder and MediaStore publication the editor calls — never a re-implementation.
 *
 * Nothing here computes p50/p95 or decides a pass: the host reporter
 * (`tools/baseline/bench_report.py`) owns statistics and gating, so a smoke run can never
 * be mistaken for a baseline.
 */
object Ticket177BenchmarkChecks {
    private const val SCHEMA = "spk.bench_capture.v1"
    private const val EVIDENCE_DIR = "ticket177"

    /** Pinned instant for export metadata so a container digest is a function of the pixels. */
    private const val PINNED_CLOCK_MILLIS = 1_700_000_000_000L

    // Built without `to`/mapOf: the release test APK carries no Kotlin runtime and the
    // minified app keeps no kotlin.TuplesKt, so an infix pair here is a device-only
    // NoClassDefFoundError. Every stdlib helper below is avoided for the same reason.
    private val FORMATS = LinkedHashMap<String, ExportFormat>().apply {
        put("JPEG_Q95", ExportFormat.JPEG)
        put("PNG16", ExportFormat.PNG16)
        put("TIFF16", ExportFormat.TIFF)
    }

    @JvmStatic
    fun run(
        context: Context,
        corpusPath: String,
        sourcePath: String,
        runs: Int,
        cellFilter: String,
        expectedAppSha256: String,
    ): String {
        val corpus = JSONObject(String(readAllBytes(File(corpusPath)), Charsets.UTF_8))
        require(corpus.optString("schema") == "spk.bench_corpus.v1") {
            "unexpected corpus schema ${corpus.optString("schema")}"
        }
        val identity = appIdentity(context, expectedAppSha256)
        val source = verifiedSource(corpus, sourcePath)
        val cells = selectedCells(corpus, cellFilter)
        val protocol = corpus.getJSONObject("protocol")
        val gateRuns = protocol.getInt("gate_runs")

        val evidence = File(context.getExternalFilesDir(null), EVIDENCE_DIR)
        val stale = evidence.listFiles()
        if (stale != null) for (i in stale.indices) stale[i].delete()
        require(evidence.mkdirs() || evidence.isDirectory) { "cannot create $evidence" }

        val capture = JSONObject()
            .put("schema", SCHEMA)
            .put("captured_at_millis", System.currentTimeMillis())
            .put("app", identity)
            .put("device", deviceFingerprint(context))
            .put("corpus", JSONObject()
                .put("source_sha256", source.sha256)
                .put("source_bytes", source.bytes)
                .put("width", source.width)
                .put("height", source.height))
            .put("protocol", JSONObject()
                .put("requested_runs", runs)
                .put("gate_runs", gateRuns)
                // The host refuses to publish a baseline from a capture marked smoke.
                .put("smoke", runs < gateRuns))

        // Create the engine once up front, exactly like the editor does before its first
        // render: asset wiring is process setup, not part of any measured export.
        EngineHolder.get(context)

        // Protocol idle binds gate captures only (a smoke pass stays fast), and the
        // cool-down runs BEFORE each measured export so the previous one cannot heat it.
        val gating = runs >= gateRuns
        val idleMs = if (gating) protocol.optInt("idle_between_runs_s", 0) * 1000L else 0L
        // A fixed idle does NOT keep a phone cool: a full matrix is ~45 min of sustained
        // 12 MP work, which walks the SoC into thermal throttling no matter how long the
        // gaps are, and a throttled sample runs materially slower than an unthrottled one.
        // So the gate waits for the declared thermal state instead of merely waiting.
        val requireThermal = if (gating) protocol.optInt("require_thermal_status", -1) else -1
        val maxThermalWaitMs = protocol.optInt("max_thermal_wait_s", 300) * 1000L
        val samples = JSONArray()
        var firstRender = true
        // Runs outermost: samples of one cell/format never run back-to-back, so thermal
        // drift spreads across every cell instead of biasing whichever block ran hot.
        for (index in 0 until runs) {
            for (cell in cells) {
                for (entry in FORMATS.entries) {
                    if (!cellHasFormat(cell, entry.key)) continue
                    if (!firstRender && idleMs > 0) Thread.sleep(idleMs)
                    val cooled = awaitThermal(context, requireThermal, maxThermalWaitMs)
                    val sample = measure(
                        context, cell, entry.value, entry.key, sourcePath, index,
                        cold = firstRender, thermalWait = cooled,
                    )
                    firstRender = false
                    samples.put(sample)
                    writeText(
                        File(evidence, java.lang.String.format(
                            Locale.ROOT, "sample-%03d.json", samples.length() - 1)),
                        sample.toString(2),
                    )
                }
            }
        }
        capture.put("samples", samples)
        capture.put("journeys", journeys(context, cells[0], sourcePath))

        val captureFile = File(evidence, "capture.json")
        writeText(captureFile, capture.toString(2))
        return StringBuilder()
            .append("TICKET177_BENCH_CAPTURE: ").append(captureFile.absolutePath).append('\n')
            .append("TICKET177_BENCH_SAMPLES: ").append(samples.length()).append('\n')
            .append("TICKET177_BENCH: PASS\n")
            .toString()
    }

    // ---- one measured export ---------------------------------------------------------------

    private fun measure(
        context: Context,
        cell: JSONObject,
        format: ExportFormat,
        formatId: String,
        sourcePath: String,
        index: Int,
        cold: Boolean,
        thermalWait: JSONObject,
    ): JSONObject {
        val state = stateFor(context, cell)
        val params = state.toParams()
        val descriptor = ExportOptions(
            format = format,
            jpegQuality = 95,
            size = ExportSize.FULL,
            customLongEdge = 0,
            customName = "",
            // Output space and CCTF come from the same state the render used, like the
            // editor's export -- a constant here would silently measure a different output
            // contract than the preset asks for.
        ).outputDescriptor(state.outputColorSpace, state.savingCctfEncoding, Build.VERSION.SDK_INT)

        // The engine owns System.loadLibrary for libspektra: decode allocates through that
        // native boundary, so the engine must exist before the first byte is decoded.
        val engine = EngineHolder.get(context)
        Debug.getMemoryInfo(Debug.MemoryInfo())
        val startMs = System.currentTimeMillis()
        val decodeStart = System.currentTimeMillis()
        val image = decodeToLinearProPhoto(
            context, Uri.fromFile(File(sourcePath)), maxEdge = EXPORT_MAX_EDGE_PX,
        )
        val decodeMs = System.currentTimeMillis() - decodeStart

        var renderId = 0L
        var engineDigest = ""
        var simulateMs = 0L
        var gradeMs = 0L
        var encodeMs = 0L
        val name = "spk-bench-${cell.getString("id")}-$formatId-$index"
        val published: Uri
        val result = try {
            val simulateStart = System.currentTimeMillis()
            engine.simulate(image, params, RenderKind.EXPORT, null).also {
                simulateMs = System.currentTimeMillis() - simulateStart
                renderId = it.renderId
            }
        } finally {
            image.close()
        }
        var totalMs: Long
        try {
            val gradeStart = System.currentTimeMillis()
            val bitmap = gradedBitmapOrNull(result, descriptor, state)
            gradeMs = System.currentTimeMillis() - gradeStart
            val encodeStart = System.currentTimeMillis()
            published = ExportClock.pinned(PINNED_CLOCK_MILLIS) {
                runBlocking {
                    when (descriptor.encoder) {
                        OutputEncoder.NATIVE_TIFF_UINT16 ->
                            saveSimResultAsTiff(context, result, descriptor, displayName = name)
                        OutputEncoder.NATIVE_PNG16 ->
                            saveSimResultAsPng16(context, result, descriptor, displayName = name)
                        OutputEncoder.ANDROID_BITMAP_JPEG, OutputEncoder.ANDROID_BITMAP_PNG ->
                            saveToGallery(
                                context, requireNotNull(bitmap) { "bitmap encoder without bitmap" },
                                descriptor, jpegQuality = 95, displayName = name,
                            )
                        else -> error("unexpected encoder ${descriptor.encoder} for $formatId")
                    }
                }
            }
            encodeMs = System.currentTimeMillis() - encodeStart
            bitmap?.recycle()
            // The export is finished here. Everything after this line is measurement:
            // hashing the 150 MB float buffer costs ~120 ms of ARMv8 SHA-256 that no real
            // export performs, and leaving it inside the total both inflates every number
            // and shows up as an unaccountable stage-reconciliation gap.
            totalMs = System.currentTimeMillis() - startMs
            engineDigest = digestOf(result)
        } finally {
            result.close()
        }

        val digests = publishedDigests(context, published, format)
        context.contentResolver.delete(published, null, null)

        val memory = Debug.MemoryInfo().also { Debug.getMemoryInfo(it) }
        return JSONObject()
            .put("cell", cell.getString("id"))
            .put("format", formatId)
            .put("run_index", index)
            .put("state", if (cold) "cold" else "warm")
            // What the sample started from, and what it cost to get there: a reader can
            // separate "measured cool" from "measured throttled" without inferring it.
            .put("thermal_wait", thermalWait)
            // The SLO binds the pre-rendered/cache-hit path (#179). Nothing serves an export
            // from a content-addressed cache yet, so every sample here is a full re-render and
            // says so; the reporter refuses to read an SLO out of a full render.
            .put("served_from_cache", false)
            .put("render_id", renderId)
            .put("total_ms", totalMs)
            .put("phases_ms", JSONObject()
                .put("decode", decodeMs)
                .put("simulate", simulateMs)
                .put("grade", gradeMs)
                .put("encode", encodeMs))
            // Stage reconciliation: the host checks the phases against the total, so an
            // unaccounted gap cannot hide inside a headline number.
            .put("phases_sum_ms", decodeMs + simulateMs + gradeMs + encodeMs)
            // The post-engine grade is a no-op at neutral values: record what was actually
            // asked for so no reader has to guess which path the grade number covers.
            .put("grade_inputs", JSONObject()
                .put("saturation", state.saturation)
                .put("vibrance", state.vibrance)
                .put("gamut_compress", state.gamutCompress)
                .put("active", ColorGrade.isActive(state.saturation, state.vibrance) ||
                    state.gamutCompress != 0f))
            .put("engine_sample_sha256", engineDigest)
            .put("decoded_sample_sha256", digests.getString("decoded_sample_sha256"))
            .put("normalized_metadata_sha256", digests.getString("normalized_metadata_sha256"))
            .put("container_sha256", digests.getString("container_sha256"))
            .put("container_bytes", digests.getInt("container_bytes"))
            .put("memory", JSONObject()
                .put("total_pss_kb", memory.totalPss)
                .put("total_private_dirty_kb", memory.totalPrivateDirty)
                .put("native_heap_alloc_kb", Debug.getNativeHeapAllocatedSize() / 1024L)
                .put("vm_hwm_kb", procStatusKb("VmHWM:"))
                .put("vm_rss_kb", procStatusKb("VmRSS:")))
            .put("environment", environment(context))
    }

    /**
     * Bitmap encoders need a graded ARGB Bitmap; native writers grade the float buffer.
     *
     * The grade values come from the SAME ParamsState the render used, exactly as the editor's
     * export does (MainActivity passes state.saturation/vibrance/gamutCompress). Passing
     * constants here would measure a different product: ColorGrade.applyInPlace returns
     * immediately when all three are neutral, so a hard-coded non-zero saturation silently adds
     * a full per-pixel Oklab round-trip that a default export never performs.
     */
    private fun gradedBitmapOrNull(
        result: SimResult,
        descriptor: OutputDescriptor,
        state: ParamsState,
    ): Bitmap? =
        when (descriptor.encoder) {
            OutputEncoder.ANDROID_BITMAP_JPEG, OutputEncoder.ANDROID_BITMAP_PNG ->
                simResultToBitmapGraded(
                    result, true, state.saturation, state.vibrance, state.gamutCompress,
                    java.util.Collections.emptyList(), null,
                )
            else -> {
                result.acquireDataLease().use { lease ->
                    ColorGrade.applyInPlace(
                        lease.data, result.width, result.height, result.colorSpace,
                        cctfEncoded = true, saturation = state.saturation,
                        vibrance = state.vibrance, gamutCompress = state.gamutCompress,
                    )
                    MaskCompositor.applyInPlace(
                        lease.data, result.width, result.height, result.colorSpace,
                        cctfEncoded = true, adjustments = java.util.Collections.emptyList(),
                    )
                }
                null
            }
        }

    // ---- digests ----------------------------------------------------------------------------

    private fun digestOf(result: SimResult): String = result.acquireDataLease().use { lease ->
        val digest = MessageDigest.getInstance("SHA-256")
        digest.update(lease.data.duplicate())
        hex(digest.digest())
    }

    /** Container SHA-256, decoded-sample SHA-256, and a normalized-metadata SHA-256. */
    private fun publishedDigests(
        context: Context,
        uri: Uri,
        format: ExportFormat,
    ): JSONObject {
        val bytes = context.contentResolver.openInputStream(uri).use { input ->
            drain(requireNotNull(input) { "published $uri is not readable" })
        }
        val decoded = if (format == ExportFormat.TIFF) {
            // No platform TIFF decoder: digest the pixel payload the writer emitted, i.e.
            // the container minus its header/IFD, which is what C3 is about.
            sha256(java.util.Arrays.copyOfRange(bytes, if (bytes.size < 8) bytes.size else 8,
                                                bytes.size))
        } else {
            val bitmap = requireNotNull(BitmapFactory.decodeByteArray(bytes, 0, bytes.size)) {
                "published $format did not decode"
            }
            try {
                val buffer = java.nio.ByteBuffer.allocate(bitmap.byteCount)
                bitmap.copyPixelsToBuffer(buffer)
                sha256(buffer.array())
            } finally {
                bitmap.recycle()
            }
        }
        val metadata = normalizedMetadata(context, uri, format, bytes.size)
        return JSONObject()
            .put("container_sha256", sha256(bytes))
            .put("container_bytes", bytes.size)
            .put("decoded_sample_sha256", decoded)
            .put("normalized_metadata_sha256", sha256(metadata.toByteArray()))
    }

    /**
     * Documented normalized metadata: MIME, declared size and the published display name with
     * its run-varying suffix removed. Volatile fields (date added/modified, row id) are excluded
     * by construction rather than filtered after the fact.
     */
    private fun normalizedMetadata(
        context: Context,
        uri: Uri,
        format: ExportFormat,
        bytes: Int,
    ): String {
        val projection = arrayOf(
            android.provider.MediaStore.Images.Media.MIME_TYPE,
            android.provider.MediaStore.Images.Media.WIDTH,
            android.provider.MediaStore.Images.Media.HEIGHT,
        )
        var mime = format.mime
        var width = 0
        var height = 0
        context.contentResolver.query(uri, projection, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                mime = cursor.getString(0) ?: mime
                width = cursor.getInt(1)
                height = cursor.getInt(2)
            }
        }
        return "mime=$mime;width=$width;height=$height;bytes=$bytes"
    }

    // ---- identity, corpus, environment -------------------------------------------------------

    private class Source(val sha256: String, val bytes: Long, val width: Int, val height: Int)

    private fun verifiedSource(corpus: JSONObject, sourcePath: String): Source {
        val spec = corpus.getJSONObject("source")
        val file = File(sourcePath)
        require(file.isFile) { "corpus source missing at $sourcePath" }
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val read = input.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
            }
        }
        val actual = hex(digest.digest())
        require(actual == spec.getString("sha256") && file.length() == spec.getLong("bytes")) {
            "corpus source drift: $actual/${file.length()} != " +
                "${spec.getString("sha256")}/${spec.getLong("bytes")}"
        }
        return Source(actual, file.length(), spec.getInt("width"), spec.getInt("height"))
    }

    private fun selectedCells(corpus: JSONObject, filter: String): ArrayList<JSONObject> {
        val wanted = HashSet<String>()
        @Suppress("PLATFORM_CLASS_MAPPED_TO_KOTLIN")
        val parts = (filter as java.lang.String).split(",")
        for (i in parts.indices) {
            val trimmed = parts[i].trim()
            if (trimmed.isNotEmpty()) wanted.add(trimmed)
        }
        val cells = corpus.getJSONArray("cells")
        val selected = ArrayList<JSONObject>()
        for (i in 0 until cells.length()) {
            val cell = cells.getJSONObject(i)
            if (wanted.isEmpty() || wanted.contains(cell.getString("id"))) selected.add(cell)
        }
        require(!selected.isEmpty()) { "no corpus cell matched '$filter'" }
        return selected
    }

    private fun paramsFor(context: Context, cell: JSONObject): com.spectrafilm.engine.SpektraParams =
        stateFor(context, cell).toParams()

    private fun stateFor(context: Context, cell: JSONObject): ParamsState {
        val state = ParamsState()
        val presetName = cell.getString("preset")
        var preset: BuiltInPreset? = null
        for (candidate in BuiltInPresets.load(context)) {
            if (candidate.name == presetName) preset = candidate
        }
        requireNotNull(preset) { "corpus preset '$presetName' is not a built-in preset" }
        BuiltInPresets.apply(preset, state)
        val effects = HashSet<String>()
        val declared = cell.getJSONArray("effects")
        for (i in 0 until declared.length()) effects.add(declared.getString(i))
        state.grainActive = effects.contains("grain")
        state.halationActive = effects.contains("halation")
        state.couplersActive = effects.contains("dir_couplers")
        // The corpus route picks the pipeline: "print" is the default negative->print->scan
        // chain; "scan" flips the film-scan toggle (UI: slide mode), skipping the print
        // stage, and stays reachable for any stock from the editor's simulation panel.
        if (cell.has("route")) state.scanFilm = "scan" == cell.getString("route")
        return state
    }

    /** A cell without a formats array measures every known format (legacy corpus). */
    private fun cellHasFormat(cell: JSONObject, formatId: String): Boolean {
        val declared = cell.optJSONArray("formats") ?: return true
        for (i in 0 until declared.length()) {
            if (formatId == declared.getString(i)) return true
        }
        return false
    }

    /** Fails closed when the installed APK is not the artifact the caller pinned. */
    private fun appIdentity(context: Context, expectedSha256: String): JSONObject {
        require(expectedSha256.length == 64) {
            "ticket177_expect_app_sha256 must be the 64-hex digest of the installed APK"
        }
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        val apk = File(info.applicationInfo.sourceDir)
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(apk).use { input ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val read = input.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
            }
        }
        val actual = hex(digest.digest())
        require(equalsIgnoreCase(actual, expectedSha256)) {
            "stale APK: installed " + actual + " != pinned " +
                expectedSha256.toLowerCase(Locale.ROOT)
        }
        val debuggable = (info.applicationInfo.flags and
            android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0
        require(!debuggable) { "benchmark refuses a debuggable build" }
        @Suppress("DEPRECATION")
        val versionCode = info.versionCode
        return JSONObject()
            .put("package", context.packageName)
            .put("apk_sha256", actual)
            .put("version_code", versionCode)
            .put("version_name", info.versionName ?: "")
            .put("debuggable", false)
    }

    private fun deviceFingerprint(context: Context): JSONObject = JSONObject()
        .put("model", Build.MODEL)
        .put("device", Build.DEVICE)
        .put("manufacturer", Build.MANUFACTURER)
        .put("sdk_int", Build.VERSION.SDK_INT)
        .put("release", Build.VERSION.RELEASE)
        .put("build_fingerprint", Build.FINGERPRINT)
        .put("abis", abiArray())
        .put("cpu_count", Runtime.getRuntime().availableProcessors())
        .put("gpu_renderer", gpuRenderer(context))

    private fun abiArray(): JSONArray {
        val array = JSONArray()
        val abis = Build.SUPPORTED_ABIS
        for (i in abis.indices) array.put(abis[i])
        return array
    }

    /** Driver identity, when the platform will tell us without an EGL context. */
    private fun gpuRenderer(context: Context): String = runCatching {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val info = manager.deviceConfigurationInfo
        "gles=" + info.glEsVersion
    }.getOrElse { "unknown" }

    private fun environment(context: Context): JSONObject {
        val power = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        val thermal = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            power.currentThermalStatus
        } else {
            -1
        }
        val battery = context.registerReceiver(
            null, IntentFilter(Intent.ACTION_BATTERY_CHANGED),
        )
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val plugged = battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) ?: -1
        val importance = ActivityManager.RunningAppProcessInfo().also {
            ActivityManager.getMyMemoryState(it)
        }.importance
        return JSONObject()
            .put("thermal_status", thermal)
            .put("battery_pct", if (level >= 0 && scale > 0) level * 100 / scale else -1)
            .put("plugged", plugged)
            .put("cpuset", readProc("/proc/self/cpuset"))
            .put("process_importance", importance)
    }

    private fun readProc(path: String): String = runCatching {
        String(readAllBytes(File(path)), Charsets.UTF_8).trim()
    }.getOrElse { "unknown" }

    // ---- lifecycle journeys ------------------------------------------------------------------

    /**
     * Machine-readable lifecycle evidence for the flows the SLO cannot see. Process death and
     * Activity recreation stay with the existing #139/#170 phases the same device gate runs;
     * duplicating them here would give a second, weaker copy of a stronger gate.
     */
    private fun journeys(context: Context, cell: JSONObject, sourcePath: String): JSONObject {
        val results = JSONObject()
        results.put("cancellation", runCatching {
            EngineHolder.get(context)
            val state = stateFor(context, cell)
            val params = state.toParams()
            val image = decodeToLinearProPhoto(
                context, Uri.fromFile(File(sourcePath)), maxEdge = EXPORT_MAX_EDGE_PX,
            )
            val cancellation = RenderCancellation()
            val worker = Thread {
                Thread.sleep(150L)
                cancellation.cancel()
            }
            worker.start()
            val outcome = try {
                runCatching {
                    EngineHolder.get(context)
                        .simulate(image, params, RenderKind.EXPORT, cancellation)
                        .use { "completed_before_cancel" }
                }.getOrElse { "cancelled:${it.javaClass.simpleName}" }
            } finally {
                worker.join()
                image.close()
            }
            outcome
        }.getOrElse { "error:${it.javaClass.simpleName}" })

        results.put("foreground_during_export", ActivityManager.RunningAppProcessInfo().also {
            ActivityManager.getMyMemoryState(it)
        }.importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE)

        results.put("reopen_published", runCatching {
            val state = stateFor(context, cell)
            val params = state.toParams()
            val image = decodeToLinearProPhoto(
                context, Uri.fromFile(File(sourcePath)), maxEdge = MAX_EDGE_PX,
            )
            val descriptor = ExportOptions(
                format = ExportFormat.JPEG, jpegQuality = 95, size = ExportSize.FULL,
                customLongEdge = 0, customName = "",
            ).outputDescriptor(state.outputColorSpace, state.savingCctfEncoding, Build.VERSION.SDK_INT)
            val uri = image.use { source ->
                EngineHolder.get(context).simulate(source, params, RenderKind.EXPORT, null)
                    .use { result ->
                        val bitmap = requireNotNull(
                            gradedBitmapOrNull(result, descriptor, state))
                        try {
                            runBlocking {
                                saveToGallery(
                                    context, bitmap, descriptor, jpegQuality = 95,
                                    displayName = "spk-bench-reopen",
                                )
                            }
                        } finally {
                            bitmap.recycle()
                        }
                    }
            }
            try {
                val reopened = context.contentResolver.openInputStream(uri).use { input ->
                    BitmapFactory.decodeStream(requireNotNull(input))
                }
                val ok = reopened != null
                reopened?.recycle()
                // Share is a read grant on this same published URI; prove it is grantable.
                val share = Intent(Intent.ACTION_SEND)
                    .setType(ExportFormat.JPEG.mime)
                    .putExtra(Intent.EXTRA_STREAM, uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                val shareable = share.resolveTypeIfNeeded(context.contentResolver) != null &&
                    ContentResolver.SCHEME_CONTENT == uri.scheme
                if (ok && shareable) "pass" else "fail:reopen=$ok share=$shareable"
            } finally {
                context.contentResolver.delete(uri, null, null)
            }
        }.getOrElse { "error:${it.javaClass.simpleName}:${it.message}" })
        return results
    }

    // ---- small helpers -----------------------------------------------------------------------

    private fun readAllBytes(file: File): ByteArray =
        FileInputStream(file).use { input -> drain(input) }

    private fun drain(input: InputStream): ByteArray {
        val sink = ByteArrayOutputStream()
        val buffer = ByteArray(1 shl 16)
        while (true) {
            val read = input.read(buffer)
            if (read <= 0) break
            sink.write(buffer, 0, read)
        }
        return sink.toByteArray()
    }

    private fun writeText(file: File, text: String) {
        FileOutputStream(file).use { sink -> sink.write(text.toByteArray(Charsets.UTF_8)) }
    }

    @Suppress("PLATFORM_CLASS_MAPPED_TO_KOTLIN")
    private fun equalsIgnoreCase(left: String, right: String): Boolean =
        (left as java.lang.String).equalsIgnoreCase(right)

    private fun sha256(bytes: ByteArray): String =
        hex(MessageDigest.getInstance("SHA-256").digest(bytes))

    private fun hex(bytes: ByteArray): String {
        val out = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            val v = b.toInt() and 0xFF
            out.append("0123456789abcdef"[v ushr 4]).append("0123456789abcdef"[v and 0x0F])
        }
        return out.toString()
    }

    /**
     * Block until PowerManager reports [required] or better, or [maxWaitMs] elapses.
     *
     * [required] < 0 disables the wait (smoke captures stay fast). The returned object records
     * the thermal status seen on entry, the status the sample actually starts at, how long the
     * wait took, and whether the cap was hit -- so a capture that could not reach the declared
     * state says so per sample instead of averaging a throttled run into the baseline.
     */
    private fun awaitThermal(context: Context, required: Int, maxWaitMs: Long): JSONObject {
        val power = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        if (required < 0 || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return JSONObject().put("required", required).put("waited_ms", 0L)
                .put("timed_out", false)
        }
        val entry = power.currentThermalStatus
        val start = System.currentTimeMillis()
        var status = entry
        while (status > required && System.currentTimeMillis() - start < maxWaitMs) {
            Thread.sleep(5000L)
            status = power.currentThermalStatus
        }
        return JSONObject()
            .put("required", required)
            .put("entry_status", entry)
            .put("start_status", status)
            .put("waited_ms", System.currentTimeMillis() - start)
            .put("timed_out", status > required)
    }

    /** Peak ("VmHWM:") / current ("VmRSS:") resident set from /proc/self/status, in kB. */
    @Suppress("PLATFORM_CLASS_MAPPED_TO_KOTLIN")
    private fun procStatusKb(prefix: String): Long {
        val lines = (readProc("/proc/self/status") as java.lang.String).split("\n")
        for (i in lines.indices) {
            val line = lines[i] as java.lang.String
            if (!line.startsWith(prefix)) continue
            val rest = (line.substring(prefix.length) as java.lang.String).trim()
            val space = (rest as java.lang.String).indexOf(32)
            val digits = if (space > 0) rest.substring(0, space) else rest
            return try {
                java.lang.Long.parseLong(digits)
            } catch (ignored: NumberFormatException) {
                -1L
            }
        }
        return -1L
    }

    // ---- #119 preview-latency probe ----------------------------------------------------------

    /**
     * Slider-drag settle at the default 640 px preview: times engine.simulatePreview on the
     * already-decoded source (the editor re-renders without re-decoding while a slider drags),
     * print vs film-scan route, alternating a +/-0.1 EV exposure nudge so no render can serve
     * a memoized result. Compose/present cost is deliberately outside this number and the
     * evidence says so.
     */
    @JvmStatic
    fun preview(
        context: Context,
        corpusPath: String,
        sourcePath: String,
        runs: Int,
        expectedAppSha256: String,
    ): String {
        val corpus = JSONObject(String(readAllBytes(File(corpusPath)), Charsets.UTF_8))
        require(corpus.optString("schema") == "spk.bench_corpus.v1") {
            "unexpected corpus schema ${corpus.optString("schema")}"
        }
        val identity = appIdentity(context, expectedAppSha256)
        val source = verifiedSource(corpus, sourcePath)
        val baseCell = corpus.getJSONArray("cells").getJSONObject(0)

        val evidence = File(context.getExternalFilesDir(null), EVIDENCE_DIR)
        require(evidence.mkdirs() || evidence.isDirectory) { "cannot create $evidence" }

        val engine = EngineHolder.get(context)
        val image = decodeToLinearProPhoto(
            context, Uri.fromFile(File(sourcePath)), maxEdge = EXPORT_MAX_EDGE_PX,
        )
        val routes = JSONArray()
        try {
            val routeIds = arrayOf("print", "scan")
            for (r in routeIds.indices) {
                val routeId = routeIds[r]
                val state = stateFor(context, baseCell)
                state.scanFilm = "scan" == routeId
                // Two unrecorded warm-ups absorb one-time allocation and LUT-memo work.
                for (warm in 0 until 2) {
                    state.exposureCompensationEv = 0f
                    engine.simulatePreview(image, state.toParams(), RenderKind.PREVIEW, null)
                        .close()
                }
                // A slider drag is not settled when the engine returns: the editor still turns
                // the float result into the ARGB bitmap it draws. Both halves are timed, and the
                // settle total is what a user actually waits for.
                val engineMs = JSONArray()
                val bitmapMs = JSONArray()
                val settleMs = JSONArray()
                for (i in 0 until runs) {
                    state.exposureCompensationEv = if (i % 2 == 0) 0.1f else -0.1f
                    val params = state.toParams()
                    val start = System.nanoTime()
                    engine.simulatePreview(image, params, RenderKind.PREVIEW, null).use { res ->
                        require(res.width > 0 && res.height > 0) { "empty preview result" }
                        val rendered = System.nanoTime()
                        val bitmap = simResultToBitmapGraded(
                            res, true, state.saturation, state.vibrance, state.gamutCompress,
                            java.util.Collections.emptyList(), null,
                        )
                        val done = System.nanoTime()
                        bitmap.recycle()
                        engineMs.put((rendered - start) / 1000000L)
                        bitmapMs.put((done - rendered) / 1000000L)
                        settleMs.put((done - start) / 1000000L)
                    }
                }
                routes.put(JSONObject()
                    .put("route", routeId)
                    .put("preset", baseCell.getString("preset"))
                    .put("preview_max_size", 640)
                    .put("engine_ms", engineMs)
                    .put("bitmap_ms", bitmapMs)
                    .put("samples_ms", settleMs))
            }
        } finally {
            image.close()
        }
        val capture = JSONObject()
            .put("schema", "spk.bench_preview.v1")
            .put("captured_at_millis", System.currentTimeMillis())
            .put("app", identity)
            .put("device", deviceFingerprint(context))
            .put("source_sha256", source.sha256)
            .put("decode_max_edge", EXPORT_MAX_EDGE_PX)
            .put("environment", environment(context))
            .put("routes", routes)
        val out = File(evidence, "preview.json")
        writeText(out, capture.toString(2))
        return StringBuilder()
            .append("TICKET177_PREVIEW_CAPTURE: ").append(out.absolutePath).append('\n')
            .append("TICKET177_PREVIEW: PASS\n")
            .toString()
    }

}
