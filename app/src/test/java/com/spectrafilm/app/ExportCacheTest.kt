/*
 * Spektrafilm for Android — export cache storage tests (issue #179). GPLv3.
 *
 * A false hit publishes a WRONG image with nothing to announce it, so these tests are mostly
 * about the ways a hit must NOT happen: partial writes, corruption, a stale contract version,
 * an expired entry, an orphaned payload.
 */
package com.spectrafilm.app

import java.io.File
import java.io.OutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ExportCacheTest {

    @get:Rule
    val temp = TemporaryFolder()

    private lateinit var root: File

    @Before
    fun setUp() {
        root = temp.newFolder("export-cache")
    }

    private fun cache(
        contract: String = "v1",
        budget: ExportCache.Budget = ExportCache.Budget(),
        // No-op logger: the miss paths log, and android.util.Log throws on the plain JVM.
    ) = ExportCache(root, budget, contract, log = { _, _ -> })

    private fun payload(size: Int, seed: Int = 7): ByteArray =
        ByteArray(size) { ((it * 31 + seed) and 0xFF).toByte() }

    private fun ExportCache.putBytes(key: String, bytes: ByteArray): Boolean =
        put(key) { sink: OutputStream -> sink.write(bytes) }

    @Test
    fun `a committed entry reads back byte-for-byte`() {
        val bytes = payload(64 * 1024)
        val subject = cache()
        assertTrue(subject.putBytes("k1", bytes))
        val entry = subject.get("k1")
        assertNotNull("expected a hit", entry)
        entry!!
        assertEquals(bytes.size.toLong(), entry.bytes)
        assertArrayEquals(bytes, entry.payload.readBytes())
    }

    @Test
    fun `an unknown key misses`() {
        assertNull(cache().get("never-written"))
    }

    @Test
    fun `a corrupted payload misses and is discarded`() {
        val subject = cache()
        subject.putBytes("k1", payload(4096))
        val entry = subject.get("k1")!!
        // Flip one byte: the length still matches, so only the digest can catch this.
        val corrupted = entry.payload.readBytes()
        corrupted[100] = (corrupted[100] + 1).toByte()
        entry.payload.writeBytes(corrupted)
        assertNull("a corrupted payload must not be served", subject.get("k1"))
        assertFalse("the bad entry must be removed", File(root, "k1.bin").exists())
    }

    @Test
    fun `a truncated payload misses`() {
        val subject = cache()
        subject.putBytes("k1", payload(8192))
        File(root, "k1.bin").writeBytes(payload(8192).copyOf(4096))
        assertNull(subject.get("k1"))
    }

    @Test
    fun `a payload with no metadata is an orphan and misses`() {
        val subject = cache()
        subject.putBytes("k1", payload(4096))
        // Exactly the state a process death between the two renames would leave behind.
        assertTrue(File(root, "k1.json").delete())
        assertNull(subject.get("k1"))
    }

    @Test
    fun `a contract version change invalidates every entry`() {
        cache(contract = "v1").putBytes("k1", payload(4096))
        assertNotNull(cache(contract = "v1").get("k1"))
        assertNull("a new numeric contract must not serve old bytes", cache(contract = "v2").get("k1"))
    }

    @Test
    fun `an entry past the age budget misses`() {
        val subject = cache(budget = ExportCache.Budget(maxAgeMillis = 0L))
        subject.putBytes("k1", payload(4096))
        Thread.sleep(5L)
        assertNull(subject.get("k1"))
    }

    @Test
    fun `a failing writer commits nothing`() {
        val subject = cache()
        val committed = subject.put("k1") { sink ->
            sink.write(payload(1024))
            throw java.io.IOException("encoder blew up")
        }
        assertFalse(committed)
        assertNull(subject.get("k1"))
        assertTrue("no temp file may survive", root.listFiles()!!.none { it.name.endsWith(".tmp") })
    }

    @Test
    fun `an empty payload is refused`() {
        val subject = cache()
        assertFalse(subject.put("k1") { /* write nothing */ })
        assertNull(subject.get("k1"))
    }

    @Test
    fun `the entry budget evicts the oldest and keeps the newest`() {
        val subject = cache(budget = ExportCache.Budget(maxEntries = 2))
        subject.putBytes("k1", payload(1024, seed = 1))
        Thread.sleep(5L)
        subject.putBytes("k2", payload(1024, seed = 2))
        Thread.sleep(5L)
        subject.putBytes("k3", payload(1024, seed = 3))
        assertNotNull("the entry just written must survive its own eviction pass",
            subject.get("k3"))
        assertNotNull(subject.get("k2"))
        assertNull("the oldest entry must be evicted", subject.get("k1"))
    }

    @Test
    fun `the byte budget evicts the oldest and keeps the newest`() {
        val subject = cache(budget = ExportCache.Budget(maxBytes = 10_000L, maxEntries = 100))
        subject.putBytes("k1", payload(6000, seed = 1))
        Thread.sleep(5L)
        subject.putBytes("k2", payload(6000, seed = 2))
        assertNotNull(subject.get("k2"))
        assertNull(subject.get("k1"))
    }

    @Test
    fun `eviction sweeps orphaned payloads and temp files`() {
        val subject = cache()
        File(root, "stray.bin").writeBytes(payload(128))
        File(root, "stray.json.tmp").writeBytes(payload(16))
        subject.evict()
        assertFalse(File(root, "stray.bin").exists())
        assertFalse(File(root, "stray.json.tmp").exists())
    }

    @Test
    fun `copyTo reproduces the exact container bytes`() {
        val bytes = payload(32 * 1024)
        val subject = cache()
        subject.putBytes("k1", bytes)
        val entry = subject.get("k1")!!
        val sink = java.io.ByteArrayOutputStream()
        assertTrue(subject.copyTo(entry, sink))
        assertArrayEquals(bytes, sink.toByteArray())
    }

    @Test
    fun `a missing cache directory is created on write`() {
        val nested = File(root, "does/not/exist/yet")
        val subject = ExportCache(nested, ExportCache.Budget(), "v1", log = { _, _ -> })
        assertTrue(subject.putBytes("k1", payload(1024)))
        assertNotNull(subject.get("k1"))
    }
}
