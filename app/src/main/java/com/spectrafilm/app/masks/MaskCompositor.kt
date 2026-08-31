/*
 * Spektrafilm for Android — local-adjustment compositor. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * The seam that makes masks DO something: it blends a [LocalAdjustment]'s Tier-A delta into the engine
 * OUTPUT buffer where the mask is opaque. Applied in place on the same leased `SimResult` buffer the global
 * grade uses (so preview + every export inherit it identically), it is a parity-safe Tier-2 op — no
 * engine/spektra-core/cpp is touched, and an empty/no-op stack is a strict no-op.
 *
 * Per pixel where the mask is opaque: decode the output CCTF → apply the Tier-A ops in linear/encoded
 * domain → blend `(1−α)·in + α·out`. All 13 [TierADelta] ops are wired. Class-P (pointwise,
 * parity-safe): **Exposure** (2^EV linear, dodge & burn), **Temp/Tint** (Bradford chromatic
 * adaptation 3x3 in linear output RGB), **Saturation** (Oklab chroma scale, hue-neutral-gray),
 * **Hue** (Oklab rotation), **Contrast** (the hue-neutral master S-curve, per channel on the encoded
 * value — same curve as the global control), **Whites/Blacks** (encoded levels remap). Class-S
 * (spatial, luma-only): **Clarity/Texture/Sharpness** (blurred-luma local contrast at three radii)
 * and **Highlights/Shadows** (regional gain gated by the blurred luma).
 */
package com.spectrafilm.app.masks

import com.spectrafilm.app.ContrastCurve
import com.spectrafilm.app.LocalWhiteBalance
import com.spectrafilm.app.Oklab
import com.spectrafilm.app.OutputCctf
import com.spectrafilm.engine.ColorSpace
import java.nio.ByteBuffer
import java.nio.ByteOrder

object MaskCompositor {

    /**
     * Apply every active [adjustments] entry in order, in place, to the output [data] (interleaved
     * float32 RGB, encoded in [cs] per [cctfEncoded]). Stacked masks compose (each blends onto the
     * previous result). No-op when nothing is active.
     */
    fun applyInPlace(
        data: ByteBuffer,
        w: Int,
        h: Int,
        cs: ColorSpace,
        cctfEncoded: Boolean,
        adjustments: List<LocalAdjustment>,
    ) {
        val active = adjustments.filter { hasOp(it.delta) && MaskRaster.coverage(it.mask) > 1e-4f }
        if (active.isEmpty()) return
        val f = data.order(ByteOrder.nativeOrder()).asFloatBuffer()
        val rgb = FloatArray(3)  // reused per pixel for the linear-domain ops (no per-pixel alloc)
        for (adj in active) {
            val d = adj.delta
            val gain = exposureGain(d.exposureEv)
            // temp/tint = a colorimetric white balance: a Bradford chromatic adaptation in the output
            // space, precomputed once as a linear-RGB 3x3. null when neutral (no per-pixel cost).
            val wbM = if (d.temp != 0f || d.tint != 0f) LocalWhiteBalance.matrix(cs, d.temp, d.tint) else null
            val sat = d.saturation / 100f
            val hue = d.hue
            val contrast = d.contrast
            // whites/blacks = a linear levels remap of the encoded value: move the display white/black
            // points, then rescale. (0,0) → bp=0, wp=1 → identity. ±100 moves an endpoint by 0.25.
            val levels = d.whites != 0f || d.blacks != 0f
            val bp = -(d.blacks / 100f) * 0.25f               // blacks + → bp<0 (lift); − → bp>0 (crush)
            val wp = 1f - (d.whites / 100f) * 0.25f            // whites + → wp<1 (brighten); − → wp>1 (recover)
            val invSpan = if (levels) 1f / (wp - bp) else 1f   // wp−bp ∈ [0.5,1.5] over the slider ranges
            val lumRange = adj.mask.luminanceRange?.takeIf { it.isActive }
            val colorRange = adj.mask.colorRange?.takeIf { it.isActive }
            val alpha = MaskRaster.rasterize(adj.mask, w, h)
            val n = w * h

            // Class-S spatial pass: precompute the output luma + the blurred buffer(s) each active op
            // needs (radii scale with the long edge so draft and export match). Luma-only — colour is
            // preserved by scaling RGB by the luma ratio. Skipped entirely when no spatial op is set.
            val spatial = d.hasSpatial
            val maxWH = maxOf(w, h).toFloat()
            val luma = if (spatial) FloatArray(n).also {
                var q = 0
                while (q < n) { val kk = q * 3; it[q] = 0.2126f * f.get(kk) + 0.7152f * f.get(kk + 1) + 0.0722f * f.get(kk + 2); q++ }
            } else null
            val blurClarity = if (luma != null && d.clarity != 0f) MaskSpatial.blur(luma, w, h, MaskSpatial.RADIUS_FRAC_CLARITY * maxWH) else null
            val blurTexture = if (luma != null && d.texture != 0f) MaskSpatial.blur(luma, w, h, MaskSpatial.RADIUS_FRAC_TEXTURE * maxWH) else null
            val blurSharp = if (luma != null && d.sharpness != 0f) MaskSpatial.blur(luma, w, h, MaskSpatial.RADIUS_FRAC_SHARP * maxWH) else null
            val blurRegion = if (luma != null && (d.highlights != 0f || d.shadows != 0f)) MaskSpatial.blur(luma, w, h, MaskSpatial.RADIUS_FRAC_REGION * maxWH) else null
            val clarityK = d.clarity / 100f * 1.2f
            val textureK = d.texture / 100f * 1.0f
            val sharpK = d.sharpness / 100f * 1.5f
            val highK = d.highlights / 100f * 0.35f
            val shadK = d.shadows / 100f * 0.35f

            var p = 0
            while (p < n) {
                var a = alpha[p]
                if (a > 0f) {
                    val k = p * 3
                    val or = f.get(k); val og = f.get(k + 1); val ob = f.get(k + 2)
                    // luminance-range refinement: gate coverage by the output luma (Rec-709 on display).
                    if (lumRange != null) a *= lumRange.gate(0.2126f * or + 0.7152f * og + 0.0722f * ob)
                    // color-range refinement: gate coverage by chroma-plane distance to the target color.
                    if (a > 0f && colorRange != null) a *= colorRange.gate(or, og, ob)
                    if (a > 0f) {
                        // decode → linear, exposure gain
                        rgb[0] = OutputCctf.decode(cs, or, cctfEncoded) * gain
                        rgb[1] = OutputCctf.decode(cs, og, cctfEncoded) * gain
                        rgb[2] = OutputCctf.decode(cs, ob, cctfEncoded) * gain
                        // white balance (temp/tint): chromatic-adaptation 3x3 in linear output RGB
                        if (wbM != null) {
                            val r = rgb[0]; val g = rgb[1]; val b = rgb[2]
                            rgb[0] = wbM[0] * r + wbM[1] * g + wbM[2] * b
                            rgb[1] = wbM[3] * r + wbM[4] * g + wbM[5] * b
                            rgb[2] = wbM[6] * r + wbM[7] * g + wbM[8] * b
                        }
                        // saturation (linear Oklab; hue + lightness preserved)
                        if (sat != 0f) Oklab.scaleChromaLinear(rgb, sat, 0f)
                        // hue (linear Oklab; rotate chroma, lightness preserved)
                        if (hue != 0f) Oklab.rotateHueLinear(rgb, hue)
                        // encode → display
                        var er = OutputCctf.encode(cs, rgb[0], cctfEncoded)
                        var eg = OutputCctf.encode(cs, rgb[1], cctfEncoded)
                        var eb = OutputCctf.encode(cs, rgb[2], cctfEncoded)
                        // contrast (encoded, hue-neutral per-channel S-curve)
                        if (contrast != 0f) {
                            er = ContrastCurve.curveAt(er, contrast)
                            eg = ContrastCurve.curveAt(eg, contrast)
                            eb = ContrastCurve.curveAt(eb, contrast)
                        }
                        // whites/blacks (encoded levels remap; anchors the opposite end for a single slider)
                        if (levels) {
                            er = ((er - bp) * invSpan).coerceIn(0f, 1f)
                            eg = ((eg - bp) * invSpan).coerceIn(0f, 1f)
                            eb = ((eb - bp) * invSpan).coerceIn(0f, 1f)
                        }
                        // Class-S spatial: add a luma-only local-contrast / regional delta, preserving
                        // colour by scaling the (pointwise-adjusted) RGB by the luma ratio.
                        if (spatial && luma != null) {
                            val lp = luma[p]
                            var dL = 0f
                            if (blurClarity != null) dL += clarityK * (lp - blurClarity[p]) * MaskSpatial.midtoneWeight(lp)
                            if (blurTexture != null) dL += textureK * (lp - blurTexture[p])
                            if (blurSharp != null) dL += sharpK * (lp - blurSharp[p])
                            if (blurRegion != null) {
                                val br = blurRegion[p]
                                if (highK != 0f) dL += highK * smoothstep((br - 0.5f) / 0.5f)
                                if (shadK != 0f) dL += shadK * smoothstep((0.5f - br) / 0.5f)
                            }
                            if (dL != 0f && lp > 1e-4f) {
                                val ratio = (lp + dL).coerceIn(0f, 1f) / lp
                                er = (er * ratio).coerceIn(0f, 1f)
                                eg = (eg * ratio).coerceIn(0f, 1f)
                                eb = (eb * ratio).coerceIn(0f, 1f)
                            }
                        }
                        // blend by alpha: (1−a)·in + a·out
                        f.put(k, or + a * (er - or))
                        f.put(k + 1, og + a * (eg - og))
                        f.put(k + 2, ob + a * (eb - ob))
                    }
                }
                p++
            }
        }
    }

    /** True when [d] has any Tier-A op the compositor wires (pointwise Class-P or spatial Class-S). */
    private fun hasOp(d: TierADelta): Boolean = !d.isNoOp

    /** Exposure as a linear-light gain (2^EV); 0 EV → 1.0 (no-op). */
    private fun exposureGain(ev: Float): Float =
        if (ev == 0f) 1f else Math.pow(2.0, ev.toDouble()).toFloat()
}
