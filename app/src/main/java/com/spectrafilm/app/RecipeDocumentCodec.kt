/*
 * Spektrafilm for Android — versioned recipe envelope codec. GPLv3.
 */
package com.spectrafilm.app

import org.json.JSONObject
import java.math.BigDecimal
import java.math.BigInteger

internal data class RecipeDocument(
    val sourceKey: String,
    val sourceName: String,
    val updatedAtMillis: Long,
    val manualRotationDeg: Int,
    val params: JSONObject,
)

internal class UnsupportedRecipeVersionException(val version: String) :
    IllegalArgumentException("unsupported recipe version: $version")

internal object RecipeDocumentCodec {
    const val SCHEMA_ID = "org.spektrafilm.recipe"
    const val SCHEMA_VERSION = 2
    const val MAX_SOURCE_NAME_CHARS = 512

    fun encode(
        sourceKey: String,
        paramsJson: String,
        sourceName: String,
        manualRotationDeg: Int,
        updatedAtMillis: Long,
    ): String {
        validateKey(sourceKey)
        validateMetadata(sourceName, manualRotationDeg, updatedAtMillis)
        val params = Presets.parseImportJson(paramsJson)
        val envelope = JSONObject().apply {
            put("schema", SCHEMA_ID)
            put("recipeVersion", SCHEMA_VERSION)
            put("sourceKey", sourceKey)
            put("sourceName", sourceName)
            put("updatedAt", updatedAtMillis)
            put("manualRotationDeg", manualRotationDeg)
            put("params", params)
        }
        AtomicJsonStore.validate(envelope)
        return envelope.toString(2)
    }

    fun decode(text: String, expectedSourceKey: String): RecipeDocument {
        validateKey(expectedSourceKey)
        val root = AtomicJsonStore.parseObject(text)

        val hasRecipeEnvelopeMarker = root.has("params") ||
            root.has("recipeVersion") ||
            root.has("sourceKey") ||
            root.has("sourceName") ||
            root.has("updatedAt") ||
            root.has("manualRotationDeg") ||
            root.optString("schema") == SCHEMA_ID

        // Very early builds defensively accepted a bare preset as a recipe. Only documents
        // with no recipe-envelope markers take that migration route; a partial/future recipe
        // must never be reinterpreted as a preset and silently downgraded.
        if (!hasRecipeEnvelopeMarker) {
            return RecipeDocument(
                sourceKey = expectedSourceKey,
                sourceName = "",
                updatedAtMillis = 0L,
                manualRotationDeg = 0,
                params = Presets.parseImportJson(root.toString()),
            )
        }

        val version = exactInteger(root.opt("recipeVersion"), "recipeVersion", BigInteger.ONE)
        val currentVersion = BigInteger.valueOf(SCHEMA_VERSION.toLong())
        if (version > currentVersion) {
            throw UnsupportedRecipeVersionException(version.toString())
        }
        require(version >= BigInteger.ONE) { "invalid recipe version: $version" }
        val declaredSchema = when (val raw = root.opt("schema")) {
            null, JSONObject.NULL -> null
            is String -> raw
            else -> throw IllegalArgumentException("recipe schema must be a string")
        }
        if (declaredSchema != null) {
            require(declaredSchema == SCHEMA_ID) { "unsupported recipe schema" }
        }
        if (version == currentVersion) {
            require(declaredSchema == SCHEMA_ID) { "recipe schema is required" }
        }
        val sourceKey = root.opt("sourceKey") as? String
            ?: throw IllegalArgumentException("recipe sourceKey must be a string")
        validateKey(sourceKey)
        require(sourceKey == expectedSourceKey) { "recipe source key does not match its filename" }
        val sourceName = when (val raw = root.opt("sourceName")) {
            null, JSONObject.NULL -> ""
            is String -> raw
            else -> throw IllegalArgumentException("recipe sourceName must be a string")
        }
        val updatedAt = exactInteger(root.opt("updatedAt"), "updatedAt", BigInteger.ZERO)
            .longValueExactOrThrow("updatedAt")
        val rotation = exactInteger(root.opt("manualRotationDeg"), "manualRotationDeg", BigInteger.ZERO)
            .intValueExactOrThrow("manualRotationDeg")
        validateMetadata(sourceName, rotation, updatedAt)
        val params = root.optJSONObject("params")
            ?: throw IllegalArgumentException("recipe params must be an object")
        return RecipeDocument(
            sourceKey = sourceKey,
            sourceName = sourceName,
            updatedAtMillis = updatedAt,
            manualRotationDeg = rotation,
            params = Presets.parseImportJson(params.toString()),
        )
    }

    fun validateKey(key: String) {
        require(KEY.matches(key)) { "recipe key must be 64 lowercase hex characters" }
    }

    private fun validateMetadata(sourceName: String, rotation: Int, updatedAt: Long) {
        require(sourceName.length <= MAX_SOURCE_NAME_CHARS) { "recipe source name is too long" }
        require(rotation in VALID_ROTATIONS) { "invalid manual rotation: $rotation" }
        require(updatedAt >= 0L) { "invalid recipe timestamp" }
    }

    /** Strict JSON integer parsing; JSONObject optInt/optLong silently coerce and truncate. */
    private fun exactInteger(value: Any?, field: String, default: BigInteger): BigInteger = try {
        when (value) {
            null, JSONObject.NULL -> default
            is Byte, is Short, is Int, is Long -> BigInteger.valueOf((value as Number).toLong())
            is BigInteger -> value
            is BigDecimal -> value.toBigIntegerExact()
            is Float -> {
                require(value.isFinite()) { "$field must be a finite integer" }
                BigDecimal.valueOf(value.toDouble()).toBigIntegerExact()
            }
            is Double -> {
                require(value.isFinite()) { "$field must be a finite integer" }
                BigDecimal.valueOf(value).toBigIntegerExact()
            }
            else -> throw IllegalArgumentException("$field must be a JSON integer")
        }
    } catch (failure: ArithmeticException) {
        throw IllegalArgumentException("$field must be an exact integer", failure)
    }

    private fun BigInteger.longValueExactOrThrow(field: String): Long {
        require(this >= LONG_MIN_VALUE && this <= LONG_MAX_VALUE) {
            "$field is outside the 64-bit integer range"
        }
        return toLong()
    }

    private fun BigInteger.intValueExactOrThrow(field: String): Int {
        require(this >= INT_MIN_VALUE && this <= INT_MAX_VALUE) {
            "$field is outside the 32-bit integer range"
        }
        return toInt()
    }

    private val KEY = Regex("[0-9a-f]{64}")
    private val VALID_ROTATIONS = setOf(0, 90, 180, 270)
    private val LONG_MIN_VALUE = BigInteger.valueOf(Long.MIN_VALUE)
    private val LONG_MAX_VALUE = BigInteger.valueOf(Long.MAX_VALUE)
    private val INT_MIN_VALUE = BigInteger.valueOf(Int.MIN_VALUE.toLong())
    private val INT_MAX_VALUE = BigInteger.valueOf(Int.MAX_VALUE.toLong())
}
