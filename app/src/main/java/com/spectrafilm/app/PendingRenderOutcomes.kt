/*
 * Spektrafilm for Android. GPLv3.
 */
package com.spectrafilm.app

import com.spectrafilm.engine.AppRenderOutcome
import com.spectrafilm.engine.SimResult

/** Tracks rendered results that have not yet been published or discarded. */
internal class PendingRenderOutcomes(
    private val report: (Long, AppRenderOutcome) -> Unit = { id, outcome ->
        SimResult.reportOutcome(id, outcome)
    },
) {
    private val pending = linkedSetOf<Long>()

    fun add(renderId: Long) {
        if (renderId != 0L) pending += renderId
    }

    fun resolve(renderId: Long, outcome: AppRenderOutcome) {
        if (pending.remove(renderId)) report(renderId, outcome)
    }

    fun resolveAll(outcome: AppRenderOutcome) {
        val ids = pending.toList()
        pending.clear()
        ids.forEach { report(it, outcome) }
    }
}
