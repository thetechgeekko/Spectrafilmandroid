/*
 * Spektrafilm for Android — retained-result grade cache. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Grade-only edits (Saturation / Vibrance / Gamut compression / local-adjustment
 * masks) are pure post-engine Kotlin passes on the engine's OUTPUT buffer — yet
 * without this cache every slider tick re-runs the full native simulate. This
 * single-slot cache retains a PRISTINE (ungraded) copy of the last settle
 * render's SimResult buffer, keyed by everything that feeds the ENGINE (the
 * SpektraParams snapshot + the decode key + the preview edge). On a grade-only
 * edit the key is unchanged, so the UI re-grades a scratch copy of the pristine
 * buffer with ZERO native work.
 *
 * ⚠ The pristine copy MUST be taken before simResultToBitmapGraded runs —
 * ColorGrade/MaskCompositor mutate the leased result buffer in place. The master buffer here
 * is never mutated after store(); every reader grades inside a fresh [Pristine.withScratch].
 *
 * Threading: lookup atomically pins the retained master before it leaves the cache lock. Clear or
 * replacement can therefore retire the owner without invalidating an already-returned hit. Store
 * publication is generation-checked so source retirement also rejects a copy that was in flight.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.NativeBufferOwner
import com.spectrafilm.engine.SpektraParams
import com.spectrafilm.libraw.WhiteBalance
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

internal fun interface GradeBufferAllocator {
    fun allocate(sizeBytes: Int): GradeBufferOwner
}

internal interface GradeBufferOwner : AutoCloseable {
    fun acquire(): GradeBufferLease
}

internal class GradeBufferLease(
    val data: ByteBuffer,
    private val release: () -> Unit,
) : AutoCloseable {
    private val closed = AtomicBoolean(false)

    override fun close() {
        if (closed.compareAndSet(false, true)) release()
    }
}

/**
 * Transfer [owner] into a newly-constructed wrapper, closing it if wrapper construction fails
 * (including OOME). Inline is important: creating the builder lambda itself cannot open a gap
 * after ownership has already been acquired.
 */
internal inline fun <T : AutoCloseable, R> handoffOrClose(
    owner: T,
    build: (T) -> R,
): R = try {
    build(owner)
} catch (failure: Throwable) {
    try {
        owner.close()
    } catch (closeFailure: Throwable) {
        runCatching { failure.addSuppressed(closeFailure) }
    }
    throw failure
}

internal enum class GradeCacheStoreOutcome { PUBLISHED, SUPERSEDED, CAPACITY_DENIED }

/** Cache retention is optional: admission/OOM failure must not discard a valid current render. */
internal inline fun storeGradeCacheBestEffort(store: () -> Boolean): GradeCacheStoreOutcome = try {
    if (store()) GradeCacheStoreOutcome.PUBLISHED else GradeCacheStoreOutcome.SUPERSEDED
} catch (_: OutOfMemoryError) {
    GradeCacheStoreOutcome.CAPACITY_DENIED
}

private val nativeGradeBufferAllocator = GradeBufferAllocator { sizeBytes ->
    require(sizeBytes > 0) { "grade buffer size must be positive" }
    handoffOrClose(NativeBufferOwner.allocate(sizeBytes.toLong())) { nativeOwner ->
        object : GradeBufferOwner {
            override fun acquire(): GradeBufferLease = handoffOrClose(
                nativeOwner.acquireDataLease(),
            ) { lease ->
                GradeBufferLease(lease.data, lease::close)
            }

            override fun close() = nativeOwner.close()
        }
    }
}

internal class GradeCache(
    private val allocator: GradeBufferAllocator = nativeGradeBufferAllocator,
) : AutoCloseable {

    /** Decode-affecting inputs — mirrors [DecodedSourceCache]'s key exactly. */
    data class DecodeKey(
        val uri: String?,
        val kind: String,
        val whiteBalance: WhiteBalance,
        val temperature: Float,
        val tint: Float,
        val creativeTemp: Float,
        val creativeTint: Float,
        val filmBalance: String,
        val rotationDegrees: Int,
        val maxEdge: Int,
    )

    /**
     * Everything that feeds the NATIVE render: the engine-param snapshot (a pure
     * data-class tree — structural equality, no array fields) + the decode key.
     * Grade inputs (saturation/vibrance/gamutCompress/masks/cctf) are deliberately
     * NOT here — they are applied at re-grade time.
     */
    data class Key(
        val engineParams: SpektraParams,
        val decode: DecodeKey,
    )

    /** Authority issued before a render starts; only its exact key/generation may publish. */
    class StoreTicket internal constructor(
        internal val generation: Long,
        internal val key: Key,
    )

    /**
     * A pinned lookup hit. Closing the cache may retire its master owner, but the active lease held
     * here keeps the bytes alive until this hit is closed. Callers must use this with `use { ... }`.
     */
    class Pristine(
        private val masterLease: GradeBufferLease,
        private val allocator: GradeBufferAllocator,
        val width: Int,
        val height: Int,
        val colorSpace: ColorSpace,
    ) : AutoCloseable {
        /** Run one grade pass on a fresh explicitly-owned copy; the master remains immutable. */
        @Synchronized
        fun <T> withScratch(block: (ByteBuffer) -> T): T {
            check(!closed.get()) { "grade cache hit is closed" }
            val src = masterLease.data.duplicate().order(ByteOrder.nativeOrder())
            src.clear()
            val scratchOwner = allocator.allocate(src.remaining())
            val scratchLease = try {
                scratchOwner.acquire()
            } catch (failure: Throwable) {
                scratchOwner.close()
                throw failure
            }
            // Transfer effective ownership to the active lease so close is deterministic even if
            // the grade block throws. NativeBufferOwner defers its free until this lease exits.
            scratchOwner.close()
            return scratchLease.use { lease ->
                val copy = lease.data.duplicate().order(ByteOrder.nativeOrder())
                copy.clear()
                copy.put(src)
                copy.clear()
                block(copy)
            }
        }

        private val closed = AtomicBoolean(false)

        @Synchronized
        override fun close() {
            if (closed.compareAndSet(false, true)) masterLease.close()
        }
    }

    private class Master(
        private val owner: GradeBufferOwner,
        private val allocator: GradeBufferAllocator,
        val width: Int,
        val height: Int,
        val colorSpace: ColorSpace,
    ) : AutoCloseable {
        fun pin(): Pristine = handoffOrClose(owner.acquire()) { masterLease ->
            Pristine(
                masterLease = masterLease,
                allocator = allocator,
                width = width,
                height = height,
                colorSpace = colorSpace,
            )
        }

        override fun close() = owner.close()
    }

    private class Entry(val key: Key, val master: Master)

    @Volatile private var slot: Entry? = null
    private var publicationGeneration = 0L
    private var closed = false

    /**
     * Supersede every older render/store and issue publication authority for [key]. This must be
     * called before decode/native work starts, never from inside [store].
     */
    @Synchronized
    fun beginStore(key: Key): StoreTicket {
        check(!closed) { "grade cache is closed" }
        return StoreTicket(++publicationGeneration, key)
    }

    /**
     * A pinned pristine result for [key], or null (engine render required). The master lease is
     * acquired while holding the same monitor used by clear/replacement, closing the lookup/use
     * race. The returned hit must be closed.
     */
    @Synchronized
    fun lookup(key: Key): Pristine? {
        if (closed) return null
        return slot
            ?.takeIf { it.key == key }
            ?.master
            ?.pin()
    }

    /**
     * Retain a pristine copy of [data] (w*h*3 float32) for [key], replacing any
     * previous entry. Call BEFORE the grade mutates the buffer. Copies eagerly —
     * the caller's buffer may be freed/mutated right after.
     */
    fun store(
        ticket: StoreTicket,
        key: Key,
        data: ByteBuffer,
        width: Int,
        height: Int,
        colorSpace: ColorSpace,
    ): Boolean {
        require(width > 0 && height > 0) { "invalid grade cache dimensions ${width}x$height" }
        val requiredBytes = try {
            Math.multiplyExact(Math.multiplyExact(width, height), 3 * Float.SIZE_BYTES)
        } catch (failure: ArithmeticException) {
            throw IllegalArgumentException("grade cache dimensions overflow: ${width}x$height", failure)
        }
        val src = data.duplicate().order(ByteOrder.nativeOrder())
        require(src.isDirect) { "grade cache requires a direct source buffer" }
        src.clear()
        require(src.remaining() >= requiredBytes) {
            "grade cache source is truncated: ${src.remaining()} < $requiredBytes bytes"
        }
        src.limit(requiredBytes)
        require(ticket.key == key) { "grade cache store ticket/key mismatch" }
        val authorizedBeforeAllocation = synchronized(this) {
            !closed && publicationGeneration == ticket.generation
        }
        if (!authorizedBeforeAllocation) return false
        val owner = allocator.allocate(requiredBytes)
        try {
            owner.acquire().use { lease ->
                val copy = lease.data.duplicate().order(ByteOrder.nativeOrder())
                copy.clear()
                require(copy.remaining() >= requiredBytes) {
                    "grade cache allocation is truncated: ${copy.remaining()} < $requiredBytes bytes"
                }
                copy.limit(requiredBytes)
                copy.put(src)
            }
        } catch (failure: Throwable) {
            owner.close()
            throw failure
        }

        val master = handoffOrClose(owner) { retainedOwner ->
            Master(retainedOwner, allocator, width, height, colorSpace)
        }
        val replacement = handoffOrClose(master) { retainedMaster ->
            Entry(key, retainedMaster)
        }
        var previous: Entry? = null
        val published = synchronized(this) {
            if (closed || publicationGeneration != ticket.generation) {
                false
            } else {
                previous = slot
                slot = replacement
                true
            }
        }
        if (published) {
            previous?.master?.close()
        } else {
            replacement.master.close()
        }
        return published
    }

    /** Drop the retained buffer (source switch / teardown). */
    fun clear() {
        val previous = synchronized(this) {
            if (closed) return
            publicationGeneration++
            slot.also { slot = null }
        }
        previous?.master?.close()
    }

    /** Permanently reject lookup, ticket issuance, and late publication after final teardown. */
    override fun close() {
        val previous = synchronized(this) {
            if (closed) return
            closed = true
            publicationGeneration++
            slot.also { slot = null }
        }
        previous?.master?.close()
    }
}
