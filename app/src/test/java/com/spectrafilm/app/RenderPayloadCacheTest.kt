/*
 * Spektrafilm for Android — idle pre-render payload store tests (issue #179). GPLv3.
 *
 * The defect to guard against is a HIT that returns the wrong pixels: a payload from another
 * edit, another app build, or a half-written file. Every one of those must read as a miss,
 * because a miss only costs a re-render while a wrong hit exports someone else's image.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SimResult
import com.spectrafilm.engine.SpektraParams
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RenderPayloadCacheTest {

    private val root = File(System.getProperty("java.io.tmpdir"), "spk-prerender-${System.nanoTime()}")

    @After
    fun cleanup() {
        // Best effort: a mapped payload stays locked on Windows until the buffer is collected.
        root.listFiles()?.forEach { it.delete() }
        root.delete()
    }

    private fun cache(contract: String = "app11/0.9.0/engine-2026-09-02") =
        RenderPayloadCache(root, contract, log = { _, _ -> })

    private fun result(width: Int, height: Int, fill: (Int) -> Float): SimResult {
        val buffer = ByteBuffer
            .allocateDirect(width * height * 3 * 4)
            .order(ByteOrder.nativeOrder())
        val floats = buffer.asFloatBuffer()
        for (i in 0 until width * height * 3) floats.put(i, fill(i))
        return SimResult.fromExternalBuffer(buffer, width, height, ColorSpace.SRGB) { }
    }

    @Test
    fun `a stored render comes back with the same pixels and geometry`() {
        val store = cache()
        result(4, 3) { it * 0.25f }.use { assertTrue(store.put("k", it)) }

        val restored = store.get("k")
        assertNotNull("the payload must be readable", restored)
        restored!!.use {
            assertEquals(4, it.width)
            assertEquals(3, it.height)
            assertEquals(ColorSpace.SRGB, it.colorSpace)
            it.acquireDataLease().use { lease ->
                val floats = lease.data.duplicate().order(ByteOrder.nativeOrder()).asFloatBuffer()
                for (i in 0 until 4 * 3 * 3) assertEquals(i * 0.25f, floats.get(i), 0f)
            }
        }
    }

    @Test
    fun `a different edit is a miss, not another edit's pixels`() {
        val store = cache()
        result(2, 2) { 1f }.use { store.put("first", it) }
        assertNull(store.get("second"))
    }

    @Test
    fun `a payload from another app build is a miss`() {
        result(2, 2) { 1f }.use { cache("app11/0.9.0/engine-A").put("k", it) }
        assertNull(cache("app12/0.9.1/engine-B").get("k"))
    }

    @Test
    fun `a truncated payload is a miss`() {
        val store = cache()
        result(2, 2) { 1f }.use { store.put("k", it) }
        val payload = File(root, "payload.bin")
        payload.writeBytes(payload.readBytes().copyOf(8))
        assertNull(store.get("k"))
        assertTrue("a payload that failed its length check must not linger", !payload.exists())
    }

    @Test
    fun `a payload whose metadata never landed is a miss`() {
        val store = cache()
        result(2, 2) { 1f }.use { store.put("k", it) }
        File(root, "payload.json").delete()
        assertNull(store.get("k"))
    }

    @Test
    fun `unreadable metadata is a miss rather than a crash`() {
        val store = cache()
        result(2, 2) { 1f }.use { store.put("k", it) }
        File(root, "payload.json").writeText("{ not json")
        assertNull(store.get("k"))
    }

    @Test
    fun `an empty store is a miss`() {
        assertNull(cache().get("k"))
        assertEquals(0L, cache().sizeBytes())
    }

    @Test
    fun `the key ignores container choices and tracks the engine inputs`() {
        val source = ExportCacheKey.Source("abc", 10L, 4096, "kind=IMAGE")
        val params = SpektraParams(
            filmProfile = "kodak_portra_400",
            printProfile = "kodak_portra_endura",
        )
        val base = RenderPayloads.key(source, params, ColorSpace.SRGB, "c1")
        assertEquals(base, RenderPayloads.key(source, params, ColorSpace.SRGB, "c1"))
        assertTrue(base != RenderPayloads.key(source, params, ColorSpace.PROPHOTO, "c1"))
        assertTrue(base != RenderPayloads.key(source, params, ColorSpace.SRGB, "c2"))
        assertTrue(
            base != RenderPayloads.key(
                ExportCacheKey.Source("abd", 10L, 4096, "kind=IMAGE"), params, ColorSpace.SRGB, "c1",
            ),
        )
    }
}
