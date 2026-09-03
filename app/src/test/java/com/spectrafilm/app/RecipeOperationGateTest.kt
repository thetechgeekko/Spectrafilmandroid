/*
 * Spektrafilm for Android — recipe lifecycle serialization tests. GPLv3.
 */
package com.spectrafilm.app

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RecipeOperationGateTest {

    @Test
    fun processFifo_oldSaveSurvivesCanceledWaiterAndPrecedesRecreatedRestore() = runBlocking {
        val runtime = RecipeOperationRuntime(
            CoroutineScope(SupervisorJob() + Dispatchers.Default),
        )
        val saveEntered = CountDownLatch(1)
        val releaseSave = CountDownLatch(1)
        val restoreCompleted = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())

        val oldSave = runtime.submit {
            saveEntered.countDown()
            assertTrue(releaseSave.await(5, TimeUnit.SECONDS))
            events += "B committed"
        }
        assertTrue(saveEntered.await(5, TimeUnit.SECONDS))
        val oldActivityWaiter = launch { oldSave.await() }
        oldActivityWaiter.cancelAndJoin()

        val recreatedRestore = runtime.submit {
            events += "B read"
            restoreCompleted.countDown()
        }
        assertFalse("restore overtook queued save", restoreCompleted.await(250, TimeUnit.MILLISECONDS))
        releaseSave.countDown()

        withTimeout(5_000) { oldSave.await() }
        withTimeout(5_000) { recreatedRestore.await() }
        assertEquals(listOf("B committed", "B read"), events)
    }

    @Test
    fun resetEpoch_preventsStaleDebounceFromSubmittingBehindBlockedDelete() = runBlocking {
        val runtime = RecipeOperationRuntime(
            CoroutineScope(SupervisorJob() + Dispatchers.Default),
        )
        val epoch = RecipeEditEpoch()
        val staleDebounceEpoch = epoch.snapshot()
        val blockerEntered = CountDownLatch(1)
        val releaseBlocker = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        val blocker = runtime.submit {
            blockerEntered.countDown()
            assertTrue(releaseBlocker.await(5, TimeUnit.SECONDS))
        }
        assertTrue(blockerEntered.await(5, TimeUnit.SECONDS))

        epoch.invalidate() // reset intent, synchronous on Main
        val delete = runtime.submit { events += "delete" }
        if (epoch.isCurrent(staleDebounceEpoch)) {
            runtime.submit { events += "stale save" }
        }
        releaseBlocker.countDown()

        withTimeout(5_000) { blocker.await() }
        withTimeout(5_000) { delete.await() }
        assertEquals(listOf("delete"), events)
    }

    @Test
    fun recreatedRead_waitsForExecutingOldActivitySave() {
        val key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        val saveEntered = CountDownLatch(1)
        val releaseSave = CountDownLatch(1)
        val readCompleted = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())

        val oldActivitySave = Thread {
            Recipes.withOperationGate(key) {
                saveEntered.countDown()
                assertTrue(releaseSave.await(5, TimeUnit.SECONDS))
                events += "new recipe committed"
            }
        }
        val recreatedActivityRead = Thread {
            assertTrue(saveEntered.await(5, TimeUnit.SECONDS))
            Recipes.withOperationGate(key) {
                events += "new recipe read"
                readCompleted.countDown()
            }
        }
        oldActivitySave.start()
        recreatedActivityRead.start()
        assertTrue(saveEntered.await(5, TimeUnit.SECONDS))
        assertFalse("read overtook executing save", readCompleted.await(250, TimeUnit.MILLISECONDS))

        releaseSave.countDown()
        oldActivitySave.join(5_000)
        recreatedActivityRead.join(5_000)

        assertFalse(oldActivitySave.isAlive)
        assertFalse(recreatedActivityRead.isAlive)
        assertEquals(listOf("new recipe committed", "new recipe read"), events)
    }
}
