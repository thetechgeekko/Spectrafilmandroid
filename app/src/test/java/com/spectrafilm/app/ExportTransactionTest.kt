/*
 * Spektrafilm for Android — unit tests for crash-safe, verified export publication. GPLv3.
 * Film modeling powered by spektrafilm.
 */
package com.spectrafilm.app

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.CancellationException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ExportTransactionTest {

    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun completedArtifact_rejectsEncoderByteCountMismatch() {
        val completedFile = completedFile("encoder-count.bin", byteArrayOf(1, 2, 3, 4))

        expectFailure {
            EncodedArtifact.fromCompletedFile(
                completedFile,
                encoderByteCount = completedFile.length() - 1,
            )
        }
    }

    @Test
    fun commit_copyFailureDeletesPendingRowAndRetiresJournalEntry() {
        val backend = FakePendingExportBackend().apply { failWriteAfterBytes = 2 }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        expectFailure { transaction.commit(artifact("copy-failure.bin"), destination()) }

        assertEquals(0, backend.totalPublishCalls)
        assertEquals(1, backend.totalDeleteCalls)
        assertTrue(journal.tokens().isEmpty())
        assertTrue(backend.states.values.all { it == StoredExportState.MISSING })
    }

    @Test
    fun commit_preCancelledDoesNotReserveOrPublishDestination() {
        val backend = FakePendingExportBackend()
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        val failure = expectFailure {
            transaction.commit(
                artifact("pre-cancelled.bin"),
                destination(),
                isCancelled = { true },
            )
        }

        assertTrue(failure is CancellationException)
        assertEquals(0, backend.totalReserveCalls)
        assertEquals(0, backend.totalPublishCalls)
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_cancelledDuringDestinationCopyDeletesPendingAndNeverPublishes() {
        var cancelled = false
        val backend = FakePendingExportBackend().apply {
            afterFirstWrite = { cancelled = true }
        }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        val failure = expectFailure {
            transaction.commit(
                artifact("cancel-during-copy.bin"),
                destination(),
                isCancelled = { cancelled },
            )
        }

        assertTrue(failure is CancellationException)
        assertEquals(1, backend.totalReserveCalls)
        assertEquals(0, backend.totalPublishCalls)
        assertEquals(1, backend.totalDeleteCalls)
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_interruptedCloseDeletesPendingRowAndNeverPublishes() {
        val backend = FakePendingExportBackend().apply { failWriteClose = true }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        expectFailure { transaction.commit(artifact("close-failure.bin"), destination()) }

        assertEquals(0, backend.totalPublishCalls)
        assertEquals(1, backend.totalDeleteCalls)
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_copyAndCleanupFailureRetainsJournalEntryForRecovery() {
        val backend = FakePendingExportBackend().apply {
            failWriteAfterBytes = 2
            deleteResult = 0
        }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        expectFailure { transaction.commit(artifact("copy-cleanup-failure.bin"), destination()) }

        assertEquals(0, backend.totalPublishCalls)
        assertEquals(1, backend.totalDeleteCalls)
        assertEquals(setOf("destination-1"), journal.tokens())
        assertEquals(StoredExportState.PENDING, backend.state("destination-1"))
    }

    @Test
    fun commit_readbackDigestMismatchDeletesPendingRowAndNeverPublishes() {
        val backend = FakePendingExportBackend().apply { corruptReadback = true }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        expectFailure { transaction.commit(artifact("digest-mismatch.bin"), destination()) }

        assertEquals(0, backend.totalPublishCalls)
        assertEquals(1, backend.totalDeleteCalls)
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_publishUpdateZeroIsFailureAndCleansPendingRow() {
        val backend = FakePendingExportBackend().apply { publishResult = 0 }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        expectFailure { transaction.commit(artifact("update-zero.bin"), destination()) }

        assertEquals(1, backend.totalPublishCalls)
        assertEquals(1, backend.totalDeleteCalls)
        assertTrue(journal.tokens().isEmpty())
        assertTrue(backend.states.values.all { it == StoredExportState.MISSING })
    }

    @Test
    fun commit_successPublishesExactlyOnceAndRetiresJournalEntry() {
        val bytes = byteArrayOf(10, 20, 30, 40, 50)
        val backend = FakePendingExportBackend()
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        val token = transaction.commit(artifact("success.bin", bytes), destination())
        transaction.recover()

        assertEquals(1, backend.publishCalls(token))
        assertEquals(0, backend.deleteCalls(token))
        assertEquals(StoredExportState.PUBLISHED, backend.state(token))
        assertArrayEquals(bytes, backend.bytes(token))
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_providerThrowsAfterApplyingPublish_confirmedPublishedStillSucceeds() {
        val backend = FakePendingExportBackend().apply { throwAfterApplyingPublish = true }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)

        val token = transaction.commit(artifact("applied-before-throw.bin"), destination())

        assertEquals(1, backend.publishCalls(token))
        assertEquals(StoredExportState.PUBLISHED, backend.state(token))
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_cancellationAfterApplyingPublish_confirmedPublishedStillSucceeds() {
        val backend = FakePendingExportBackend().apply {
            failureAfterApplyingPublish = CancellationException("Activity was recreated")
        }
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)
        var publishedCallbackCount = 0
        var publishedCallbackToken: String? = null

        val token = transaction.commit(
            artifact("published-before-cancellation.bin"),
            destination(),
            onPublished = { publishedToken ->
                publishedCallbackCount++
                publishedCallbackToken = publishedToken
            },
        )

        assertEquals(1, backend.publishCalls(token))
        assertEquals(StoredExportState.PUBLISHED, backend.state(token))
        assertEquals(1, publishedCallbackCount)
        assertEquals(token, publishedCallbackToken)
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun commit_duplicateDisplayNamesStillReserveDistinctDestinationTokens() {
        val backend = FakePendingExportBackend()
        val journal = FakePendingExportJournal()
        val transaction = ExportTransaction(backend, journal)
        val spec = destination(displayName = "same-name.tif")
        val encoded = artifact("duplicate-source.bin")

        val first = transaction.commit(encoded, spec)
        val second = transaction.commit(encoded, spec)

        assertFalse(first == second)
        assertEquals(spec, backend.spec(first))
        assertEquals(spec, backend.spec(second))
        assertEquals(1, backend.publishCalls(first))
        assertEquals(1, backend.publishCalls(second))
    }

    @Test
    fun recover_pendingRowDeletesItAndRetiresJournalEntry() {
        val backend = FakePendingExportBackend().apply {
            seed("pending-token", StoredExportState.PENDING)
        }
        val journal = FakePendingExportJournal("pending-token")

        ExportTransaction(backend, journal).recover()

        assertEquals(1, backend.deleteCalls("pending-token"))
        assertEquals(0, backend.publishCalls("pending-token"))
        assertEquals(StoredExportState.MISSING, backend.state("pending-token"))
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun recover_unjournaledPendingRowDiscoveredByBackend_deletesOrphan() {
        val backend = FakePendingExportBackend().apply {
            seed("insert-before-journal-kill", StoredExportState.PENDING)
            discoveredPendingTokens += "insert-before-journal-kill"
        }
        val journal = FakePendingExportJournal()

        val report = ExportTransaction(backend, journal).recover()

        assertEquals(1, report.examined)
        assertEquals(1, report.removedPending)
        assertEquals(1, backend.deleteCalls("insert-before-journal-kill"))
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun recover_pendingDiscoveryFailureFailsClosed() {
        val backend = FakePendingExportBackend().apply { failPendingDiscovery = true }
        val journal = FakePendingExportJournal()

        val failure = expectFailure { ExportTransaction(backend, journal).recover() }

        assertTrue(failure.message.orEmpty().contains("discovery"))
        assertEquals(0, backend.totalDeleteCalls)
        assertEquals(0, backend.totalPublishCalls)
    }

    @Test
    fun recoverAbandonedStages_deletesOnlyPriorProcessExportParts() {
        val cache = temporaryFolder.newFolder("cache")
        val oldStage = File(cache, "spectrafilm-export-old.png.part").apply {
            writeBytes(byteArrayOf(1))
            assertTrue(setLastModified(100L))
        }
        val currentStage = File(cache, "spectrafilm-export-current.tif.part").apply {
            writeBytes(byteArrayOf(2))
            assertTrue(setLastModified(300L))
        }
        val unrelated = File(cache, "unrelated.part").apply {
            writeBytes(byteArrayOf(3))
            assertTrue(setLastModified(100L))
        }

        val report = recoverAbandonedExportStages(cache, priorProcessCutoffMillis = 200L)

        assertEquals(1, report.examined)
        assertEquals(1, report.removed)
        assertFalse(oldStage.exists())
        assertTrue(currentStage.exists())
        assertTrue(unrelated.exists())
    }

    @Test
    fun stageRecoveryGate_providerRetryCannotAdvanceCutoffOrDeleteLiveStage() {
        val cache = temporaryFolder.newFolder("retry-cache")
        val priorProcessStage = File(cache, "spectrafilm-export-prior.png.part").apply {
            writeBytes(byteArrayOf(1))
            assertTrue(setLastModified(100L))
        }
        val gate = ProcessStageRecoveryGate(cutoffMillis = 200L)

        val firstSweep = gate.runOnce { cutoff -> recoverAbandonedExportStages(cache, cutoff) }
        assertEquals(1, firstSweep?.removed)
        assertFalse(priorProcessStage.exists())

        // Models provider/journal recovery failing after the one safe stage sweep. A live stage
        // is then created by this process before Activity recreation retries provider recovery.
        val liveStage = File(cache, "spectrafilm-export-live.png.part").apply {
            writeBytes(byteArrayOf(2))
            assertTrue(setLastModified(300L))
        }
        val retrySweep = gate.runOnce { cutoff -> recoverAbandonedExportStages(cache, cutoff) }

        assertNull(retrySweep)
        assertTrue(liveStage.exists())
        assertEquals(200L, gate.cutoffMillis)
    }

    @Test
    fun recover_alreadyPublishedRowOnlyRetiresJournalEntry() {
        val backend = FakePendingExportBackend().apply {
            seed("published-token", StoredExportState.PUBLISHED)
        }
        val journal = FakePendingExportJournal("published-token")

        ExportTransaction(backend, journal).recover()

        assertEquals(0, backend.deleteCalls("published-token"))
        assertEquals(0, backend.publishCalls("published-token"))
        assertEquals(StoredExportState.PUBLISHED, backend.state("published-token"))
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun recover_missingRowOnlyRetiresJournalEntry() {
        val backend = FakePendingExportBackend().apply {
            seed("missing-token", StoredExportState.MISSING)
        }
        val journal = FakePendingExportJournal("missing-token")

        ExportTransaction(backend, journal).recover()

        assertEquals(0, backend.deleteCalls("missing-token"))
        assertEquals(0, backend.publishCalls("missing-token"))
        assertTrue(journal.tokens().isEmpty())
    }

    @Test
    fun recover_cleanupFailureRetainsJournalEntryForNextProcessStart() {
        val backend = FakePendingExportBackend().apply {
            seed("retry-token", StoredExportState.PENDING)
            deleteResult = 0
        }
        val journal = FakePendingExportJournal("retry-token")

        ExportTransaction(backend, journal).recover()

        assertEquals(1, backend.deleteCalls("retry-token"))
        assertEquals(setOf("retry-token"), journal.tokens())
        assertEquals(StoredExportState.PENDING, backend.state("retry-token"))
    }

    private fun artifact(
        name: String,
        bytes: ByteArray = byteArrayOf(1, 3, 3, 7, 9),
    ): EncodedArtifact {
        val file = completedFile(name, bytes)
        return EncodedArtifact.fromCompletedFile(file, encoderByteCount = bytes.size.toLong())
    }

    private fun completedFile(name: String, bytes: ByteArray): File =
        temporaryFolder.newFile(name).apply { writeBytes(bytes) }

    private fun destination(displayName: String = "Spektrafilm.tif") = ExportDestinationSpec(
        displayName = displayName,
        mimeType = "image/tiff",
        relativePath = "Pictures/Spektrafilm",
    )

    private fun expectFailure(block: () -> Unit): Exception = try {
        block()
        throw AssertionError("Expected the operation to fail")
    } catch (failure: Exception) {
        failure
    }

    private class FakePendingExportJournal(vararg initialTokens: String) : PendingExportJournal {
        private val pendingTokens = linkedSetOf(*initialTokens)

        override fun add(token: String) {
            pendingTokens += token
        }

        override fun remove(token: String) {
            pendingTokens -= token
        }

        override fun tokens(): Set<String> = pendingTokens.toSet()
    }

    private class FakePendingExportBackend : PendingExportBackend {
        private data class Entry(
            val spec: ExportDestinationSpec,
            var storedState: StoredExportState,
            var bytes: ByteArray = byteArrayOf(),
        )

        private val entries = linkedMapOf<String, Entry>()
        private val publishCounts = linkedMapOf<String, Int>()
        private val deleteCounts = linkedMapOf<String, Int>()
        private var nextToken = 1

        var totalReserveCalls: Int = 0

        var failWriteAfterBytes: Int? = null
        var failWriteClose: Boolean = false
        var corruptReadback: Boolean = false
        var publishResult: Int = 1
        var deleteResult: Int = 1
        var throwAfterApplyingPublish: Boolean = false
        var failureAfterApplyingPublish: Throwable? = null
        var failPendingDiscovery: Boolean = false
        var afterFirstWrite: (() -> Unit)? = null
        val discoveredPendingTokens = linkedSetOf<String>()

        val totalPublishCalls: Int
            get() = publishCounts.values.sum()

        val totalDeleteCalls: Int
            get() = deleteCounts.values.sum()

        val states: Map<String, StoredExportState>
            get() = entries.mapValues { it.value.storedState }

        override fun reserve(spec: ExportDestinationSpec): String {
            totalReserveCalls++
            val token = "destination-${nextToken++}"
            entries[token] = Entry(spec, StoredExportState.PENDING)
            return token
        }

        override fun openWrite(token: String): OutputStream {
            val entry = requireNotNull(entries[token]) { "Unknown token: $token" }
            val sink = ByteArrayOutputStream()
            var written = 0

            return object : OutputStream() {
                override fun write(value: Int) {
                    if (failWriteAfterBytes?.let { written >= it } == true) {
                        throw IOException("No space left on device")
                    }
                    sink.write(value)
                    written++
                    entry.bytes = sink.toByteArray()
                    if (written == 1) afterFirstWrite?.invoke()
                }

                override fun write(buffer: ByteArray, offset: Int, length: Int) {
                    repeat(length) { index -> write(buffer[offset + index].toInt()) }
                }

                override fun close() {
                    entry.bytes = sink.toByteArray()
                    if (failWriteClose) throw IOException("Destination close was interrupted")
                }
            }
        }

        override fun openRead(token: String): InputStream {
            val original = requireNotNull(entries[token]) { "Unknown token: $token" }.bytes
            val bytes = if (corruptReadback) {
                if (original.isEmpty()) byteArrayOf(1) else original.copyOf().apply { this[0] = (this[0] + 1).toByte() }
            } else {
                original
            }
            return ByteArrayInputStream(bytes)
        }

        override fun publish(token: String, spec: ExportDestinationSpec): Int {
            publishCounts[token] = publishCalls(token) + 1
            if (publishResult == 1) {
                requireNotNull(entries[token]).storedState = StoredExportState.PUBLISHED
            }
            if (throwAfterApplyingPublish) throw IOException("provider reply was interrupted")
            failureAfterApplyingPublish?.let { throw it }
            return publishResult
        }

        override fun delete(token: String): Int {
            deleteCounts[token] = deleteCalls(token) + 1
            if (deleteResult == 1) {
                requireNotNull(entries[token]).storedState = StoredExportState.MISSING
            }
            return deleteResult
        }

        override fun state(token: String): StoredExportState =
            entries[token]?.storedState ?: StoredExportState.MISSING

        override fun pendingTokens(): Set<String> {
            if (failPendingDiscovery) throw IOException("pending discovery unavailable")
            return discoveredPendingTokens.toSet()
        }

        fun seed(token: String, state: StoredExportState) {
            entries[token] = Entry(destinationSpecForSeed, state)
        }

        fun publishCalls(token: String): Int = publishCounts[token] ?: 0

        fun deleteCalls(token: String): Int = deleteCounts[token] ?: 0

        fun bytes(token: String): ByteArray = requireNotNull(entries[token]).bytes

        fun spec(token: String): ExportDestinationSpec = requireNotNull(entries[token]).spec

        private companion object {
            val destinationSpecForSeed = ExportDestinationSpec(
                displayName = "recovered.tif",
                mimeType = "image/tiff",
                relativePath = "Pictures/Spektrafilm",
            )
        }
    }
}
