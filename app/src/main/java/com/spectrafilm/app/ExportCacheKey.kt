/*
 * Spektrafilm for Android — content-addressed export cache key (issue #179). GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The key must cover EVERY input that can change an exported byte. An incomplete key does not
 * fail loudly: it serves a stale image that looks plausible, which is the same silent-wrong-output
 * class of defect as the R8 keep-rule bug and the benchmark's hard-coded grade constant. So the
 * rule here is that nothing is assumed to be irrelevant, and `ExportCacheKeyTest` re-derives the
 * covered set independently.
 *
 * What goes in, and why each part is load-bearing:
 *   * the SOURCE IMAGE CONTENT, digested — not the URI. Photo-picker URIs are ephemeral (see
 *     Recipes.keyFor), so a URI key both misses on a re-picked file and, far worse, can HIT when
 *     a reused content URI now points at different bytes.
 *   * every engine parameter, via SpektraParams.toString(). That tree is pure data classes with
 *     no array fields (the property GradeCache.Key already depends on), so its toString is a
 *     complete, deterministic rendering of every field, and a new engine parameter joins the key
 *     the day it is added rather than the day someone remembers to update this file.
 *   * the post-engine grade and mask edits, which the engine parameters deliberately exclude.
 *   * the full OutputDescriptor. Its own toString prints only 5 of its 16 fields, so the fields
 *     are enumerated here; equals/hashCode cover all 16 but a 32-bit hash is not a safe key.
 *   * the output geometry and JPEG quality, which live outside the descriptor.
 *   * a contract version covering the app build. Profiles, spectral LUTs and ICC assets ship
 *     inside the APK and cannot change without it, so one version string covers all of them
 *     along with the engine binary and the numeric contract.
 */
package com.spectrafilm.app

import java.io.File
import java.io.InputStream
import java.security.MessageDigest

internal object ExportCacheKey {

    /**
     * Identifies the pixels that reach the engine. [contentSha256] digests the source bytes;
     * [decodeIdentity] renders every decode-time input that changes them — orientation, RAW
     * white balance and development, the creative temperature/tint applied during decode, and
     * the source kind. It is rendered by the caller rather than taken as a whole
     * SourceDecodeRequest because that type carries a Context, whose toString is an identity
     * and would differ between two otherwise identical exports.
     */
    class Source(
        val contentSha256: String,
        val bytes: Long,
        val decodeMaxEdge: Int,
        val decodeIdentity: String,
    )

    /** Post-engine, app-side edits. The engine parameters do not carry these. */
    class Grade(
        val cctfEncoded: Boolean,
        val saturation: Float,
        val vibrance: Float,
        val gamutCompress: Float,
        val localAdjustments: List<com.spectrafilm.app.masks.LocalAdjustment>,
    )

    /**
     * The key for one exported artifact: a lowercase hex SHA-256 over a canonical, newline
     * delimited rendering of every input. Field labels are included so two adjacent values can
     * never be confused for one another when one of them is empty.
     */
    fun compute(
        source: Source,
        params: com.spectrafilm.engine.SpektraParams,
        grade: Grade,
        descriptor: OutputDescriptor,
        targetLongEdge: Int?,
        jpegQuality: Int,
        contractVersion: String,
    ): String {
        val text = buildString {
            line("schema", SCHEMA)
            line("contract", contractVersion)

            line("source.sha256", source.contentSha256)
            line("source.bytes", source.bytes)
            line("source.decodeMaxEdge", source.decodeMaxEdge)
            line("source.decode", source.decodeIdentity)

            // The whole engine parameter tree, in one deterministic rendering.
            line("params", params.toString())

            line("grade.cctf", grade.cctfEncoded)
            line("grade.saturation", grade.saturation)
            line("grade.vibrance", grade.vibrance)
            line("grade.gamutCompress", grade.gamutCompress)
            line("grade.adjustments", grade.localAdjustments.size)
            grade.localAdjustments.forEachIndexed { index, adjustment ->
                line("grade.adjustment.$index", adjustment.toString())
            }

            // Enumerated rather than delegated: OutputDescriptor.toString() is a summary.
            line("out.format", descriptor.format.name)
            line("out.primaries", descriptor.primaries.name)
            line("out.whitePoint", descriptor.whitePoint.name)
            line("out.transfer", descriptor.transfer.name)
            line("out.reference", descriptor.reference.name)
            line("out.bitDepth", descriptor.bitDepth.name)
            line("out.alpha", descriptor.alpha.name)
            line("out.range", descriptor.range.name)
            line("out.encoder", descriptor.encoder.name)
            line("out.quantizer", descriptor.quantizer.name)
            // A data class, not an enum: its own toString renders every field.
            line("out.metadata", descriptor.metadata)
            line("out.exportClass", descriptor.existingExportClass.id)
            line("out.engineColorSpace", descriptor.engineColorSpace?.name ?: "-")
            line("out.engineCctf", descriptor.engineCctfEncoding?.toString() ?: "-")
            line("out.minimumApi", descriptor.minimumApi)
            line("out.releaseStatus", descriptor.releaseStatus.name)

            line("geometry.longEdge", targetLongEdge?.toString() ?: "full")
            line("encoder.jpegQuality", jpegQuality)
        }
        return sha256Hex(text.toByteArray(Charsets.UTF_8))
    }

    /** One label=value line per input. No value here renders across lines, so the
     *  newline terminator keeps adjacent fields unambiguous. */
    private fun StringBuilder.line(label: String, value: Any?) {
        append(label).append('=').append(value).append('\n')
    }

    /**
     * Digest of the source bytes. Reads through [open] so it works for any Uri the app can read
     * without needing a File. Returns null when the source cannot be read — the caller must then
     * treat the export as uncacheable rather than fall back to a weaker identity.
     */
    fun digestSource(open: () -> InputStream?): Pair<String, Long>? = runCatching {
        val digest = MessageDigest.getInstance("SHA-256")
        var total = 0L
        val stream = open() ?: return null
        stream.use { input ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val read = input.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
                total += read
            }
        }
        // hex of the accumulated digest — NOT sha256Hex, which would hash the hash.
        if (total <= 0L) null else ExportCache.hex(digest.digest()) to total
    }.getOrNull()

    fun digestFile(file: File): Pair<String, Long>? =
        if (!file.isFile) null else digestSource { file.inputStream() }

    /**
     * The contract version: any change to the app build can change exported bytes, because the
     * engine binary, the bundled profiles/LUTs/ICC assets and the numeric contract all ship
     * inside it. Callers pass BuildConfig values rather than this reading them, so the unit
     * tests can pin a version.
     */
    fun contractVersion(versionCode: Int, versionName: String, numericContract: String): String =
        "app$versionCode/$versionName/$numericContract"

    internal fun sha256Hex(bytes: ByteArray): String =
        ExportCache.hex(MessageDigest.getInstance("SHA-256").digest(bytes))

    /** Bumped if the layout of the key material itself changes. */
    private const val SCHEMA = "spk.export_cache_key.v1"

    /**
     * Bumped when the engine's numeric output changes for unchanged parameters — a new engine
     * revision, a rebuilt LUT asset, a compiler flag change. It is deliberately manual: nothing
     * else can know that two builds produce different pixels from the same inputs.
     */
    const val NUMERIC_CONTRACT = "engine-2026-09-02"
}
