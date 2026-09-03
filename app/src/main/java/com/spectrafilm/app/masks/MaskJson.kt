/*
 * Spektrafilm for Android — local-adjustment (mask) recipe serialization. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * Round-trips a List<LocalAdjustment> to/from JSON for the recipe/preset `"masks"` block, using the
 * same android org.json the rest of Presets uses. App-internal schema for now (mirrors our Mask model);
 * a future increment can additionally emit the full crs:MaskGroupBasedCorrections XMP for Lightroom
 * interop (see docs/MASKING_SPEC.md). Old recipes with no `"masks"` key → empty list → today's exact
 * global-only behavior. Pure Kotlin, no engine touched.
 */
package com.spectrafilm.app.masks

import org.json.JSONArray
import org.json.JSONObject
import java.math.BigDecimal
import java.math.BigInteger

object MaskJson {

    const val SCHEMA_ID = "org.spektrafilm.mask-set"
    const val SCHEMA_VERSION = 1
    const val MAX_ADJUSTMENTS = 64
    const val MAX_COMPONENTS_PER_MASK = 32

    /** Stable interoperable mask-set document; see docs/MASK_JSON_SCHEMA.md. */
    fun toJson(adjustments: List<LocalAdjustment>): JSONObject {
        require(adjustments.size <= MAX_ADJUSTMENTS) {
            "mask adjustment count exceeds $MAX_ADJUSTMENTS"
        }
        val arr = JSONArray()
        for (adj in adjustments) {
            require(adj.mask.components.size <= MAX_COMPONENTS_PER_MASK) {
                "mask component count exceeds $MAX_COMPONENTS_PER_MASK"
            }
            arr.put(JSONObject().apply {
                put("delta", deltaToJson(adj.delta))
                put("mask", maskToJson(adj.mask))
            })
        }
        return JSONObject().apply {
            put("schema", SCHEMA_ID)
            put("version", SCHEMA_VERSION)
            put("adjustments", arr)
        }
    }

    /** Accepts the current object document and the shipped legacy bare-array v0 form. */
    fun fromJson(value: Any?): List<LocalAdjustment> {
        if (value == null || value === JSONObject.NULL) return emptyList()
        val arr = when (value) {
            is JSONArray -> value // legacy recipes/presets written before schema v1
            is JSONObject -> {
                require(value.optString("schema") == SCHEMA_ID) { "unsupported mask schema" }
                val rawVersion = value.opt("version")
                require(isExactSchemaVersion(rawVersion)) {
                    "unsupported mask schema version: $rawVersion"
                }
                value.optJSONArray("adjustments")
                    ?: throw IllegalArgumentException("mask document has no adjustments array")
            }
            else -> throw IllegalArgumentException("mask document must be an object or legacy array")
        }
        require(arr.length() <= MAX_ADJUSTMENTS) {
            "mask adjustment count exceeds $MAX_ADJUSTMENTS"
        }
        val out = ArrayList<LocalAdjustment>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i)
                ?: throw IllegalArgumentException("mask adjustment $i is not an object")
            out.add(LocalAdjustment(maskFromJson(o.optJSONObject("mask")), deltaFromJson(o.optJSONObject("delta"))))
        }
        return out
    }

    /** Do not use JSONObject.optInt: it coerces strings/fractions and can truncate large longs. */
    private fun isExactSchemaVersion(value: Any?): Boolean = when (value) {
        is Byte, is Short, is Int, is Long -> (value as Number).toLong() == SCHEMA_VERSION.toLong()
        is Float -> value.isFinite() && value == SCHEMA_VERSION.toFloat()
        is Double -> value.isFinite() && value == SCHEMA_VERSION.toDouble()
        is BigInteger -> value == BigInteger.valueOf(SCHEMA_VERSION.toLong())
        is BigDecimal -> value.compareTo(BigDecimal.valueOf(SCHEMA_VERSION.toLong())) == 0
        else -> false
    }

    internal fun migrateLegacy(arr: JSONArray): JSONObject = JSONObject().apply {
        put("schema", SCHEMA_ID)
        put("version", SCHEMA_VERSION)
        put("adjustments", JSONArray(arr.toString()))
    }

    private fun deltaToJson(d: TierADelta) = JSONObject().apply {
        put("exposureEv", d.exposureEv.toDouble()); put("temp", d.temp.toDouble()); put("tint", d.tint.toDouble())
        put("saturation", d.saturation.toDouble()); put("contrast", d.contrast.toDouble()); put("hue", d.hue.toDouble())
        put("whites", d.whites.toDouble()); put("blacks", d.blacks.toDouble())
        put("clarity", d.clarity.toDouble()); put("sharpness", d.sharpness.toDouble()); put("texture", d.texture.toDouble())
        put("highlights", d.highlights.toDouble()); put("shadows", d.shadows.toDouble())
    }

    private fun deltaFromJson(o: JSONObject?): TierADelta {
        if (o == null) return TierADelta()
        return TierADelta(
            f(o, "exposureEv"), f(o, "temp"), f(o, "tint"), f(o, "saturation"), f(o, "contrast"), f(o, "hue"),
            f(o, "whites"), f(o, "blacks"),
            f(o, "clarity"), f(o, "sharpness"), f(o, "texture"), f(o, "highlights"), f(o, "shadows"),
        )
    }

    private fun maskToJson(m: Mask) = JSONObject().apply {
        put("invert", m.invert); put("opacity", m.opacity.toDouble())
        put("components", JSONArray().apply { for (c in m.components) put(componentToJson(c)) })
        m.luminanceRange?.let { put("lumRange", lumRangeToJson(it)) }
        m.colorRange?.let { put("colorRange", colorRangeToJson(it)) }
    }

    private fun maskFromJson(o: JSONObject?): Mask {
        if (o == null) return Mask()
        val comps = ArrayList<Mask.Component>()
        o.optJSONArray("components")?.let { ca ->
            require(ca.length() <= MAX_COMPONENTS_PER_MASK) {
                "mask component count exceeds $MAX_COMPONENTS_PER_MASK"
            }
            for (i in 0 until ca.length()) {
                val component = ca.optJSONObject(i)
                    ?: throw IllegalArgumentException("mask component $i is not an object")
                comps.add(componentFromJson(component))
            }
        }
        val lum = o.optJSONObject("lumRange")?.let { lumRangeFromJson(it) }
        val col = o.optJSONObject("colorRange")?.let { colorRangeFromJson(it) }
        return Mask(comps, o.optBoolean("invert", false), f(o, "opacity", 1f), lum, col)
    }

    private fun lumRangeToJson(r: LuminanceRange) = JSONObject().apply {
        put("min", r.lumMin.toDouble()); put("max", r.lumMax.toDouble())
        put("feather", r.feather.toDouble()); put("invert", r.invert)
    }

    private fun lumRangeFromJson(o: JSONObject) = LuminanceRange(
        f(o, "min", 0f), f(o, "max", 1f), f(o, "feather", 0.1f), o.optBoolean("invert", false),
    )

    private fun colorRangeToJson(r: ColorRange) = JSONObject().apply {
        put("r", r.targetR.toDouble()); put("g", r.targetG.toDouble()); put("b", r.targetB.toDouble())
        put("tolerance", r.tolerance.toDouble()); put("feather", r.feather.toDouble()); put("invert", r.invert)
    }

    private fun colorRangeFromJson(o: JSONObject) = ColorRange(
        f(o, "r", 0.5f), f(o, "g", 0.5f), f(o, "b", 0.5f),
        f(o, "tolerance", 0.6f), f(o, "feather", 0.1f), o.optBoolean("invert", false),
    )

    private fun componentToJson(c: Mask.Component) = JSONObject().apply {
        put("mode", c.mode.name); put("invert", c.invert); put("value", c.value.toDouble())
        put("shape", shapeToJson(c.shape))
    }

    private fun componentFromJson(o: JSONObject) = Mask.Component(
        mode = strictEnum(o.optString("mode", BlendMode.ADD.name), BlendMode.ADD),
        shape = shapeFromJson(o.optJSONObject("shape")),
        invert = o.optBoolean("invert", false),
        value = f(o, "value", 1f),
    )

    private fun shapeToJson(s: MaskComponent): JSONObject = when (s) {
        is MaskComponent.Linear -> JSONObject().apply {
            put("type", "linear")
            put("x0", s.x0.toDouble()); put("y0", s.y0.toDouble())
            put("x1", s.x1.toDouble()); put("y1", s.y1.toDouble())
        }
        is MaskComponent.Radial -> JSONObject().apply {
            put("type", "radial")
            put("cx", s.cx.toDouble()); put("cy", s.cy.toDouble())
            put("rx", s.rx.toDouble()); put("ry", s.ry.toDouble())
            put("feather", s.feather.toDouble()); put("angleDeg", s.angleDeg.toDouble())
        }
    }

    private fun shapeFromJson(o: JSONObject?): MaskComponent {
        if (o == null) return MaskComponent.Radial(0.5f, 0.5f, 0.25f, 0.25f)
        return when (val type = o.optString("type", "radial")) {
            "linear" -> MaskComponent.Linear(f(o, "x0"), f(o, "y0"), f(o, "x1", 1f), f(o, "y1"))
            "radial" -> MaskComponent.Radial(
                f(o, "cx", 0.5f), f(o, "cy", 0.5f), f(o, "rx", 0.25f), f(o, "ry", 0.25f),
                f(o, "feather", 0.5f), f(o, "angleDeg"),
            )
            else -> throw IllegalArgumentException("unsupported mask shape: $type")
        }
    }

    private fun f(o: JSONObject, k: String, def: Float = 0f): Float {
        val value = o.optDouble(k, def.toDouble()).toFloat()
        require(value.isFinite()) { "mask number $k is not finite" }
        return value
    }

    private inline fun <reified T : Enum<T>> strictEnum(name: String, def: T): T {
        if (name.isBlank()) return def
        return runCatching { enumValueOf<T>(name) }
            .getOrElse { throw IllegalArgumentException("unsupported ${T::class.java.simpleName}: $name") }
    }
}
