/*
 * Spektrafilm for Android — non-destructive recipe / sidecar layer. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * M5 headline feature. A "recipe" is simply a preset bound to a specific source
 * image: the full SpektraParams (+ film/print profile ids, raw WB, output color
 * space) serialized with the EXACT same JSON schema as a saved preset (see
 * Presets.encode/decode — no parallel params model), wrapped in a small envelope
 * that records the stable source key plus light metadata.
 *
 * The original image is NEVER written. Edits are persisted automatically (debounced)
 * to app-private storage at filesDir/recipes/<key>.json. When a source is re-opened,
 * if a recipe exists for its key it is loaded and the editing state is restored, so
 * the edit is non-destructive and reproducible. If none exists, defaults are used.
 *
 * Source key: a SHA-256 (hex) of the source's stable identity string. For a content
 * Uri that is the Uri string itself; this is stable across re-opens of the SAME Uri.
 * Limitation: photo-picker Uris are ephemeral grants whose string can differ between
 * sessions for the same underlying file, so the recipe may not re-bind after the
 * grant is revoked — RAW/DNG opened via OpenDocument (persistable) and re-picked
 * identical Uris re-bind reliably. The DEMO source has no stable identity and is not
 * persisted.
 */
package com.spectrafilm.app

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.IOException
import java.nio.charset.CharacterCodingException
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap

internal sealed interface RecipeLoadResult {
    data object Missing : RecipeLoadResult
    data class Loaded(val rotation: SourceRotation) : RecipeLoadResult
    data class CorruptQuarantined(val quarantinePath: String?, val reason: String) : RecipeLoadResult
    data class Unavailable(val reason: String) : RecipeLoadResult
}

internal sealed interface RecipeReadResult {
    data object Missing : RecipeReadResult
    data class Loaded(val document: RecipeDocument) : RecipeReadResult
    data class CorruptQuarantined(val quarantinePath: String?, val reason: String) : RecipeReadResult
    data class Unsupported(val version: String) : RecipeReadResult
    data class CorruptQuarantineFailed(val reason: String) : RecipeReadResult
    data class IoFailure(val reason: String) : RecipeReadResult
}

object Recipes {

    private data class RecipeGate(var generation: Long = 0L)

    private val gates = ConcurrentHashMap<String, RecipeGate>()

    private fun gate(key: String): RecipeGate {
        RecipeDocumentCodec.validateKey(key)
        return gates.computeIfAbsent(key) { RecipeGate() }
    }

    private fun <T> withGate(key: String, block: (RecipeGate) -> T): T {
        val operation = gate(key)
        return synchronized(operation) { block(operation) }
    }

    internal fun <T> withOperationGate(key: String, block: () -> T): T =
        withGate(key) { block() }

    internal fun generation(key: String): Long = withGate(key) { it.generation }

    private fun dir(ctx: Context): File =
        File(ctx.filesDir, "recipes").apply { mkdirs() }

    /**
     * Stable key for a source. Returns null for sources that have no stable identity
     * (e.g. the synthetic demo image), which should not be persisted as recipes.
     */
    fun keyFor(uri: Uri?): String? {
        val id = uri?.toString()?.takeIf { it.isNotBlank() } ?: return null
        return sha256Hex(id)
    }

    private fun file(ctx: Context, key: String): File {
        RecipeDocumentCodec.validateKey(key)
        return File(dir(ctx), "$key.json")
    }

    fun exists(ctx: Context, key: String?): Boolean =
        key != null && runCatching { file(ctx, key).isFile }.getOrDefault(false)

    /**
     * Save/update the recipe for [key] from a PRE-SERIALIZED params payload
     * ([Presets.toJsonString]). Serializing reads live Compose [ParamsState] field-by-field
     * and must happen on the main thread; this takes the finished string so only
     * the envelope build + file write cross to IO — avoiding a torn off-thread snapshot.
     * The original image is untouched — only this app-private sidecar JSON is written;
     * [sourceName] is stored purely as a human-readable hint for any future recipe browser.
     */
    fun saveJson(
        ctx: Context,
        key: String,
        paramsJson: String,
        sourceName: String,
        rotationDegrees: Int = 0,
        expectedGeneration: Long? = null,
    ): Boolean {
        return withGate(key) { operation ->
            if (expectedGeneration != null && expectedGeneration != operation.generation) {
                return@withGate false
            }
            val envelope = RecipeDocumentCodec.encode(
                sourceKey = key,
                paramsJson = paramsJson,
                sourceName = sourceName.take(RecipeDocumentCodec.MAX_SOURCE_NAME_CHARS),
                manualRotationDeg = rotationDegrees,
                updatedAtMillis = System.currentTimeMillis(),
            )
            AtomicJsonStore.writeUtf8(file(ctx, key), envelope, AtomicJsonStore.MAX_RECIPE_BYTES)
            true
        }
    }

    /**
     * Load the recipe for [key] into [into]. Returns true if a recipe existed and was
     * applied, false otherwise (leaving [into] untouched so defaults stand).
     */
    fun load(ctx: Context, key: String?, into: ParamsState): Boolean {
        return loadResult(ctx, key, into) is RecipeLoadResult.Loaded
    }

    /** Typed restore lets UI distinguish missing state from quarantined corruption. */
    internal fun loadResult(ctx: Context, key: String?, into: ParamsState): RecipeLoadResult {
        return when (val read = readResult(ctx, key)) {
            RecipeReadResult.Missing -> RecipeLoadResult.Missing
            is RecipeReadResult.CorruptQuarantined -> RecipeLoadResult.CorruptQuarantined(
                read.quarantinePath,
                read.reason,
            )
            is RecipeReadResult.Unsupported -> RecipeLoadResult.Unavailable(
                "unsupported recipe version ${read.version}",
            )
            is RecipeReadResult.CorruptQuarantineFailed -> RecipeLoadResult.Unavailable(read.reason)
            is RecipeReadResult.IoFailure -> RecipeLoadResult.Unavailable(read.reason)
            is RecipeReadResult.Loaded -> {
                Presets.decode(read.document.params, into)
                RecipeLoadResult.Loaded(SourceRotation.fromDegrees(read.document.manualRotationDeg))
            }
        }
    }

    internal fun readResult(ctx: Context, key: String?): RecipeReadResult {
        if (key == null) return RecipeReadResult.Missing
        return try {
            withOperationGate(key) {
                val target = file(ctx, key)
                AtomicJsonStore.withPathLock(target) {
                    if (!target.isFile) return@withPathLock RecipeReadResult.Missing
                    val text = try {
                        AtomicJsonStore.readUtf8(target, AtomicJsonStore.MAX_RECIPE_BYTES)
                    } catch (failure: DocumentLimitException) {
                        return@withPathLock quarantineResult(target, failure)
                    } catch (failure: CharacterCodingException) {
                        return@withPathLock quarantineResult(target, failure)
                    } catch (failure: IOException) {
                        return@withPathLock RecipeReadResult.IoFailure(
                            failure.message ?: "recipe read failed",
                        )
                    }
                    try {
                        val document = RecipeDocumentCodec.decode(text, key)
                        RecipeReadResult.Loaded(document)
                    } catch (failure: UnsupportedRecipeVersionException) {
                        RecipeReadResult.Unsupported(failure.version)
                    } catch (failure: Exception) {
                        quarantineResult(target, failure)
                    }
                }
            }
        } catch (failure: IllegalArgumentException) {
            RecipeReadResult.CorruptQuarantineFailed(failure.message ?: "invalid recipe key")
        }
    }

    private fun quarantineResult(target: File, failure: Throwable): RecipeReadResult {
        return try {
            val quarantined = AtomicJsonStore.quarantine(target)
            RecipeReadResult.CorruptQuarantined(
                quarantinePath = quarantined.absolutePath,
                reason = failure.message ?: failure.javaClass.simpleName,
            )
        } catch (quarantineFailure: Exception) {
            failure.addSuppressed(quarantineFailure)
            RecipeReadResult.CorruptQuarantineFailed(
                failure.message ?: "recipe corruption could not be quarantined",
            )
        }
    }

    /**
     * The persisted MANUAL rotation (clockwise) for [key], or [SourceRotation.NONE] if
     * there is no recipe / no stored rotation. The EXIF baseline is NOT stored here.
     */
    fun loadRotation(ctx: Context, key: String?): SourceRotation {
        if (key == null) return SourceRotation.NONE
        return runCatching {
            withOperationGate(key) {
                val f = file(ctx, key)
                if (!f.isFile) return@withOperationGate SourceRotation.NONE
                val text = AtomicJsonStore.readUtf8(f, AtomicJsonStore.MAX_RECIPE_BYTES)
                SourceRotation.fromDegrees(RecipeDocumentCodec.decode(text, key).manualRotationDeg)
            }
        }.getOrDefault(SourceRotation.NONE)
    }

    /** Delete the recipe for [key] (clears the saved edit). No-op if none exists. */
    fun delete(ctx: Context, key: String?) {
        if (key == null) return
        withGate(key) { operation ->
            operation.generation++
            AtomicJsonStore.delete(file(ctx, key))
        }
    }

    /**
     * Reset the live editing [state] back to defaults, reusing the shared preset schema
     * so every field is restored exactly as a fresh launch would seed it. A pristine
     * [ParamsState] is round-tripped through the same encode/decode path (no parallel
     * reset logic to drift from serialization), then the user's saved app defaults are
     * re-applied just like on first launch. The original image is never touched.
     */
    fun resetToDefaults(state: ParamsState, settings: AppSettings, availableProfiles: List<String>) {
        Presets.decode(Presets.encode(ParamsState()), state)
        settings.applyDefaultsTo(state, availableProfiles)
    }

    private fun sha256Hex(s: String): String {
        val bytes = MessageDigest.getInstance("SHA-256").digest(s.toByteArray())
        return bytes.joinToString("") { "%02x".format(it) }
    }
}
