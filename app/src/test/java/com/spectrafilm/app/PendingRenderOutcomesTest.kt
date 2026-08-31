package com.spectrafilm.app

import com.spectrafilm.engine.AppRenderOutcome
import org.junit.Assert.assertEquals
import org.junit.Test

class PendingRenderOutcomesTest {
    @Test
    fun publishedDraftIsNotRelabeledWhenFinalRenderFails() {
        val reported = mutableListOf<Pair<Long, AppRenderOutcome>>()
        val pending = PendingRenderOutcomes { id, outcome -> reported += id to outcome }

        pending.add(101L)
        pending.resolve(101L, AppRenderOutcome.CONSUMED)
        pending.add(102L)
        pending.resolveAll(AppRenderOutcome.FAILED)

        assertEquals(
            listOf(
                101L to AppRenderOutcome.CONSUMED,
                102L to AppRenderOutcome.FAILED,
            ),
            reported,
        )
    }

    @Test
    fun resolvedIdsCannotReceiveALaterDisposition() {
        val reported = mutableListOf<Pair<Long, AppRenderOutcome>>()
        val pending = PendingRenderOutcomes { id, outcome -> reported += id to outcome }

        pending.add(201L)
        pending.resolveAll(AppRenderOutcome.SUPERSEDED)
        pending.resolveAll(AppRenderOutcome.FAILED)

        assertEquals(
            listOf(201L to AppRenderOutcome.SUPERSEDED),
            reported,
        )
    }
}
