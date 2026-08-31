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
 * is never mutated after store(); every reader grades a fresh [Pristine.scratchCopy].
 *
 * Threading: the settle coroutine is the sole writer (store/replace); readers on
 * any thread see a consistent immutable Entry via the single @Volatile slot.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.ColorSpace
import com.spectrafilm.engine.SpektraParams
import com.spectrafilm.libraw.WhiteBalance
import java.nio.ByteBuffer
import java.nio.ByteOrder

class GradeCache {

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

    /** An immutable ungraded engine result; grade a [scratchCopy], never the master. */
    class Pristine(
        private val master: ByteBuffer,
        val width: Int,
        val height: Int,
        val colorSpace: ColorSpace,
    ) {
        /** A fresh mutable copy for one grade pass (thread-safe: master is read-only). */
        fun scratchCopy(): ByteBuffer {
            val src = master.duplicate().order(ByteOrder.nativeOrder())
            src.clear()
            val copy = ByteBuffer.allocateDirect(src.capacity()).order(ByteOrder.nativeOrder())
            copy.put(src)
            copy.clear()
            return copy
        }
    }

    private class Entry(val key: Key, val pristine: Pristine)

    @Volatile private var slot: Entry? = null

    /** The pristine result for [key], or null (engine render required). */
    fun lookup(key: Key): Pristine? {
        val e = slot ?: return null
        return if (e.key == key) e.pristine else null
    }

    /**
     * Retain a pristine copy of [data] (w*h*3 float32) for [key], replacing any
     * previous entry. Call BEFORE the grade mutates the buffer. Copies eagerly —
     * the caller's buffer may be freed/mutated right after.
     */
    fun store(key: Key, data: ByteBuffer, width: Int, height: Int, colorSpace: ColorSpace) {
        val src = data.duplicate().order(ByteOrder.nativeOrder())
        src.clear()
        val copy = ByteBuffer.allocateDirect(src.capacity()).order(ByteOrder.nativeOrder())
        copy.put(src)
        copy.clear()
        slot = Entry(key, Pristine(copy, width, height, colorSpace))
    }

    /** Drop the retained buffer (source switch / teardown). */
    fun clear() {
        slot = null
    }
}
