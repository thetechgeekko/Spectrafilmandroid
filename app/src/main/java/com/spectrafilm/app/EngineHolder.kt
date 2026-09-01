/*
 * Spektrafilm for Android.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Film modeling powered by spektrafilm (GPLv3).
 */
package com.spectrafilm.app

import android.content.Context
import com.spectrafilm.engine.SpektraEngine

/**
 * Process-scoped owner of the single native [SpektraEngine].
 *
 * The engine is immutable after construction — it only holds read-only bundled
 * profiles + LUTs, and its `simulate` path has no shared mutable state — so one
 * instance is reused for the whole process across Activity recreations
 * (configuration changes, returning from background, etc.).
 *
 * This fixes a native-memory leak: the editor used to create a fresh engine in a
 * `LaunchedEffect(Unit)` on every recomposition-from-scratch (e.g. every rotation)
 * and never destroyed the previous one, orphaning its ~17 MB-class native state
 * each time.
 *
 * We deliberately never [SpektraEngine.close] the shared engine during the app's
 * life. The only safe moment to release the native handle is process death, which
 * the OS handles for us — closing on composition dispose would race a render
 * coroutine still executing inside `nativeSimulate` and free the handle out from
 * under it (use-after-free). A single immutable engine per process avoids both the
 * leak and that race.
 */
object EngineHolder {
    @Volatile private var instance: SpektraEngine? = null

    /**
     * Return the shared engine, creating it on first use. Heavy on the first call
     * (asset wiring); call off the main thread.
     */
    fun get(ctx: Context): SpektraEngine {
        instance?.let { return it }
        return synchronized(this) {
            instance ?: create(ctx).also { instance = it }
        }
    }

    /**
     * Prefer reading bundled assets straight from the APK (no ~17 MB extraction to
     * filesDir); fall back to the extract-then-create path if the AAssetManager
     * engine can't be created.
     */
    private fun create(ctx: Context): SpektraEngine {
        val app = ctx.applicationContext
        // Establish one shared native/JVM admission ceiling before the engine, caches, or first
        // decoded frame can allocate. The policy is physical-RAM based and intentionally ignores
        // largeHeap so native/GPU/writer allocations cannot escape an ART-only allowance.
        AppMemoryBudget.configure(app)
        // Apply the stored core-affinity choice before the first render. The engine
        // gates this on an env var, which a running JVM cannot set for itself, so it
        // has to be pushed in from here. Cheap and side-effect-free when off.
        runCatching {
            val settings = AppSettings.from(app)
            settings.clearLegacyBigCores()
            val on = settings.bigCores
            SpektraEngine.setBigCores(if (on) 1 else 0)
            if (on) Diag.i("big cores on (experiment) detected=${SpektraEngine.bigCoreCount()}")
        }.onFailure { Diag.w("big cores apply failed: ${it.message}") }
        return runCatching { SpektraEngine.fromAssets(app.assets) }
            .onSuccess { Diag.i("engine create ok via=fromAssets") }
            .getOrElse { fromAssetsErr ->
                Diag.w("engine create via=fromAssets failed (${fromAssetsErr.message}); falling back to extract")
                runCatching { SpektraEngine(extractAssets(app).absolutePath) }
                    .onSuccess { Diag.i("engine create ok via=extract") }
                    .onFailure { Diag.e("engine create via=extract failed", it) }
                    .getOrThrow()
            }
    }
}
