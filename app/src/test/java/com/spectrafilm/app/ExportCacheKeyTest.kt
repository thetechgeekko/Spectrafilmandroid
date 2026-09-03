/*
 * Spektrafilm for Android — export cache key completeness tests (issue #179). GPLv3.
 *
 * An incomplete cache key serves a stale image that looks plausible. Nothing crashes and no log
 * line appears, so these tests are the only thing standing between a future parameter and a
 * silently wrong export. Each input is perturbed one at a time and REQUIRED to move the key —
 * the same discipline the engine's own LUT memo gate (test_lut_cache_e2e.cpp) applies natively.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SpektraParams
import java.io.ByteArrayInputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ExportCacheKeyTest {

    private val contract = ExportCacheKey.contractVersion(11, "0.9.0", "engine-test")

    private fun params() = SpektraParams(
        filmProfile = "kodak_portra_400",
        printProfile = "kodak_portra_endura",
    )

    private fun source(
        sha: String = "a".repeat(64),
        bytes: Long = 37_460_417L,
        maxEdge: Int = 16384,
        decode: String = "kind=IMAGE;rot=NONE;wb=AS_SHOT",
    ) = ExportCacheKey.Source(sha, bytes, maxEdge, decode)

    private fun grade(
        cctf: Boolean = true,
        saturation: Float = 0f,
        vibrance: Float = 0f,
        gamut: Float = 0f,
    ) = ExportCacheKey.Grade(cctf, saturation, vibrance, gamut, emptyList())

    private fun descriptor(format: ExportFormat = ExportFormat.TIFF) =
        ExportOptions(
            format = format,
            jpegQuality = 95,
            size = ExportSize.FULL,
            customLongEdge = 0,
            customName = "",
        ).outputDescriptor(ColorSpace.SRGB, outputCctfEncoding = true, apiLevel = 36)

    private fun key(
        source: ExportCacheKey.Source = source(),
        params: SpektraParams = params(),
        grade: ExportCacheKey.Grade = grade(),
        descriptor: OutputDescriptor = descriptor(),
        longEdge: Int? = null,
        jpegQuality: Int = 95,
        contractVersion: String = contract,
    ) = ExportCacheKey.compute(
        source, params, grade, descriptor, longEdge, jpegQuality, contractVersion,
    )

    @Test
    fun `the key is a stable lowercase sha256 for identical inputs`() {
        val first = key()
        assertEquals(first, key())
        assertEquals(64, first.length)
        assertTrue("expected lowercase hex, got $first", first.matches(Regex("[0-9a-f]{64}")))
    }

    // ---- every input must move the key -------------------------------------------------------

    @Test
    fun `source identity participates`() {
        val base = key()
        assertNotEquals("different bytes", base, key(source = source(sha = "b".repeat(64))))
        assertNotEquals("different length", base, key(source = source(bytes = 1L)))
        assertNotEquals("different decode cap", base, key(source = source(maxEdge = 2048)))
        assertNotEquals("different orientation", base,
            key(source = source(decode = "kind=IMAGE;rot=CW90;wb=AS_SHOT")))
        assertNotEquals("different RAW development", base,
            key(source = source(decode = "kind=RAW;rot=NONE;wb=5200/3")))
    }

    @Test
    fun `post-engine grade participates`() {
        val base = key()
        assertNotEquals("cctf", base, key(grade = grade(cctf = false)))
        // The defect that invalidated the first #119 capture was a saturation of 1f being
        // treated as neutral. A key that ignored it would serve the ungraded image.
        assertNotEquals("saturation", base, key(grade = grade(saturation = 1f)))
        assertNotEquals("vibrance", base, key(grade = grade(vibrance = 1f)))
        assertNotEquals("gamut compression", base, key(grade = grade(gamut = 1f)))
    }

    @Test
    fun `output descriptor and geometry participate`() {
        val base = key()
        assertNotEquals("format", base, key(descriptor = descriptor(ExportFormat.PNG16)))
        assertNotEquals("long edge", base, key(longEdge = 2048))
        assertNotEquals("jpeg quality", base, key(jpegQuality = 80))
    }

    @Test
    fun `the contract version participates`() {
        assertNotEquals(
            key(),
            key(contractVersion = ExportCacheKey.contractVersion(12, "0.9.1", "engine-test")),
        )
        assertNotEquals(
            "a numeric-contract bump must invalidate every entry",
            key(),
            key(contractVersion = ExportCacheKey.contractVersion(11, "0.9.0", "engine-next")),
        )
    }

    @Test
    fun `engine parameters participate, including deeply nested ones`() {
        val base = key()
        assertNotEquals("film profile", base, key(params = params().copy(filmProfile = "other")))
        assertNotEquals("print profile", base, key(params = params().copy(printProfile = "other")))

        val nested = params().let { p ->
            p.copy(camera = p.camera.copy(exposureCompensationEv = p.camera.exposureCompensationEv + 0.25f))
        }
        assertNotEquals("a nested camera parameter", base, key(params = nested))

        val deeper = params().let { p ->
            p.copy(filmRender = p.filmRender.copy(grain = p.filmRender.grain.copy(active = !p.filmRender.grain.active)))
        }
        assertNotEquals("a twice-nested grain parameter", base, key(params = deeper))
    }

    /**
     * The key folds the whole engine tree in via `SpektraParams.toString()`. That is complete
     * ONLY while the tree stays free of array-typed properties: an array's toString is its
     * identity, so an array field would render as `[F@1b6d3586` — different on every run (making
     * the key useless) and blind to its own contents (making it dangerous). Adding one must fail
     * here rather than silently in an export.
     */
    @Test
    fun `no engine parameter is array-typed, which toString cannot render`() {
        val offenders = mutableListOf<String>()
        val seen = mutableSetOf<Class<*>>()

        fun walk(type: Class<*>, path: String) {
            if (!seen.add(type)) return
            for (field in type.declaredFields) {
                if (field.isSynthetic || java.lang.reflect.Modifier.isStatic(field.modifiers)) continue
                val fieldType = field.type
                if (fieldType.isArray) offenders += "$path.${field.name}: ${fieldType.simpleName}"
                if (fieldType.name.startsWith("com.spectrafilm.engine")) {
                    walk(fieldType, "$path.${field.name}")
                }
            }
        }
        walk(SpektraParams::class.java, "SpektraParams")

        assertTrue(
            "array-typed engine parameters cannot be rendered by toString, so the export cache " +
                "key would silently stop tracking them: $offenders",
            offenders.isEmpty(),
        )
    }

    @Test
    fun `the rendered parameter text actually contains every top-level group`() {
        // Guards the same assumption from the other side: if toString were ever overridden to
        // a summary (as OutputDescriptor's is), the key would quietly narrow.
        val text = params().toString()
        for (group in listOf(
            "filmProfile", "printProfile", "filmRender", "printRender",
            "camera", "enlarger", "scanner", "io", "settings", "toneCurve",
        )) {
            assertTrue("SpektraParams.toString() omits $group", text.contains(group))
        }
    }

    // ---- source digesting --------------------------------------------------------------------

    @Test
    fun `digestSource hashes content and reports length`() {
        val bytes = ByteArray(4096) { (it and 0xFF).toByte() }
        val digested = ExportCacheKey.digestSource { ByteArrayInputStream(bytes) }
        assertTrue(digested != null)
        val (sha, length) = requireNotNull(digested)
        assertEquals(bytes.size.toLong(), length)
        assertEquals(
            ExportCache.hex(java.security.MessageDigest.getInstance("SHA-256").digest(bytes)),
            sha,
        )
    }

    @Test
    fun `digestSource returns null rather than a weak identity when unreadable`() {
        assertNull(ExportCacheKey.digestSource { null })
        assertNull("an empty source is not a usable identity",
            ExportCacheKey.digestSource { ByteArrayInputStream(ByteArray(0)) })
        assertNull(ExportCacheKey.digestSource { error("stream blew up") })
    }
}
