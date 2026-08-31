/*
 * Spektrafilm for Android — monotonic render publication gate. GPLv3.
 */
package com.spectrafilm.app

import java.util.concurrent.atomic.AtomicLong

internal enum class RenderPublicationPriority(internal val rank: Int) {
    DRAFT(1),
    SETTLE(2),
    EXPORT(3),
}

internal class RenderPublicationTicket internal constructor(
    internal val revision: Long,
    internal val priority: RenderPublicationPriority,
    internal val serial: Long,
)

/** Allows exactly one publication from the newest, highest-quality live request. */
internal class RenderPublicationGate {
    private val revisions = AtomicLong(0L)
    private var serial = 0L
    private var current: RenderPublicationTicket? = null
    private var claimed = false

    fun nextRevision(): Long = revisions.incrementAndGet()

    @Synchronized
    fun begin(
        revision: Long,
        priority: RenderPublicationPriority,
    ): RenderPublicationTicket {
        require(revision > 0L) { "render publication revision must be positive" }
        val ticket = RenderPublicationTicket(revision, priority, ++serial)
        val active = current
        if (
            active == null ||
            revision > active.revision ||
            (revision == active.revision && priority.rank >= active.priority.rank)
        ) {
            current = ticket
            claimed = false
        }
        return ticket
    }

    @Synchronized
    fun tryClaim(ticket: RenderPublicationTicket): Boolean {
        if (claimed || current != ticket) return false
        claimed = true
        return true
    }
}
