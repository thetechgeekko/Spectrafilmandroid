/*
 * Spektrafilm for Android - shipping tc_lut cache JNI/instrumentation checks.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
package com.spectrafilm.engine

import android.content.Context
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.json.JSONObject

internal object TcLutCacheInstrumentationChecks {
    private const val JSON_CAPACITY_BYTES = 2_048

    fun run(context: Context) {
        val pixels = ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        pixels.asFloatBuffer().apply {
            put(0, 0.18f)
            put(1, 0.18f)
            put(2, 0.18f)
        }
        val image = LinearImage(pixels, 1, 1)
        val params = SpektraParams(
            filmProfile = "kodak_portra_400",
            printProfile = "kodak_portra_endura",
        )
        val engine = SpektraEngine.fromAssets(context.applicationContext.assets)
        try {
            val before = checkedSnapshot(engine.tcLutCacheStatsJson())
            check(before.getLong("cache_held_bytes") == 0L)
            check(before.getLong("pinned_entries") == 0L)
            check(before.getLong("dynamic_entries") == 0L)

            engine.simulate(image, params).use { result ->
                check(result.width == 1 && result.height == 1)
                result.acquireDataLease().use { lease ->
                    check(lease.data.remaining() >= 3 * Float.SIZE_BYTES)
                }
            }
            val afterMiss = checkedSnapshot(engine.tcLutCacheStatsJson())
            check(afterMiss.getLong("misses") > before.getLong("misses"))
            check(afterMiss.getLong("pinned_entries") == 1L)
            check(afterMiss.getLong("cache_held_bytes") > 0L)

            engine.simulate(image, params).use { result ->
                check(result.width == 1 && result.height == 1)
            }
            val afterHit = checkedSnapshot(engine.tcLutCacheStatsJson())
            check(afterHit.getLong("hits") > afterMiss.getLong("hits"))
            check(afterHit.getLong("misses") == afterMiss.getLong("misses"))
            check(afterHit.getLong("cache_held_bytes") ==
                afterMiss.getLong("cache_held_bytes"))
        } finally {
            engine.close()
            image.close()
        }

        // The public readout must use EngineHandleLease instead of sending a
        // stale native pointer through JNI after close.
        val closedFailure = runCatching { engine.tcLutCacheStatsJson() }.exceptionOrNull()
        check(closedFailure is IllegalStateException) {
            "cache diagnostics bypassed the closed-engine lifecycle lease: $closedFailure"
        }
    }

    private fun checkedSnapshot(raw: String): JSONObject {
        check(raw.toByteArray(Charsets.UTF_8).size < JSON_CAPACITY_BYTES)
        val json = JSONObject(raw)
        check(json.getString("schema") == "spk.tc_lut_cache.v1")
        check(json.getString("memory_boundary") == "post-build-cache-residency")

        val pinnedBytes = json.nonNegativeLong("pinned_bytes")
        val dynamicBytes = json.nonNegativeLong("dynamic_bytes")
        val heldBytes = json.nonNegativeLong("cache_held_bytes")
        val pinnedEntries = json.nonNegativeLong("pinned_entries")
        val dynamicEntries = json.nonNegativeLong("dynamic_entries")
        val pinnedBudget = json.nonNegativeLong("pinned_byte_budget")
        val dynamicBudget = json.nonNegativeLong("dynamic_byte_budget")
        val maxPinned = json.nonNegativeLong("max_pinned_entries")
        val maxDynamic = json.nonNegativeLong("max_dynamic_entries")

        check(heldBytes == pinnedBytes + dynamicBytes)
        check(pinnedBytes <= pinnedBudget)
        check(dynamicBytes <= dynamicBudget)
        check(pinnedEntries <= maxPinned)
        check(dynamicEntries <= maxDynamic)
        check(maxPinned == 28L)
        check(maxDynamic == 64L)
        for (counter in listOf(
            "hits", "misses", "race_hits", "evictions",
            "oversize_bypasses", "entry_limit_bypasses", "budget_bypasses",
            "global_budget_denials", "build_failures", "accounting_overflows",
            "classification_conflicts",
        )) {
            json.nonNegativeLong(counter)
        }
        return json
    }

    private fun JSONObject.nonNegativeLong(name: String): Long =
        getLong(name).also { check(it >= 0L) { "$name was negative: $it" } }
}
