/*
 * Spektrafilm for Android — cross-dispatcher publication coordination. GPLv3.
 */
package com.spectrafilm.app

import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * Records the irreversible destination-publication linearization point from the worker thread.
 *
 * `withContext` has prompt cancellation on its return dispatch. Consequently, code after
 * `withContext` is not guaranteed to run even when the worker already published the destination.
 * The publishing adapter calls [markPublished] at its actual commit point (confirmed MediaStore
 * `PUBLISHED`, or the successful legacy atomic rename), before control crosses any cancellable
 * dispatcher return. The outer failure classifier can then make publication win over late cancel.
 */
internal class ExportCommitLinearization {
    private val published = AtomicBoolean(false)

    val isPublished: Boolean
        get() = published.get()

    fun markPublished() {
        published.set(true)
    }
}

internal data class RoiRenderPublicationTicket(
    val renderKey: Int,
    val requestId: Long,
)

/** Last-request-wins gate which also rejects every result from an older edit/render key. */
internal class RoiRenderPublicationGate {
    private val latestRequestId = AtomicLong(0L)

    fun begin(renderKey: Int): RoiRenderPublicationTicket = RoiRenderPublicationTicket(
        renderKey = renderKey,
        requestId = latestRequestId.incrementAndGet(),
    )

    fun invalidate() {
        latestRequestId.incrementAndGet()
    }

    fun canPublish(
        ticket: RoiRenderPublicationTicket,
        currentRenderKey: Int,
    ): Boolean = ticket.renderKey == currentRenderKey &&
        ticket.requestId == latestRequestId.get()
}
