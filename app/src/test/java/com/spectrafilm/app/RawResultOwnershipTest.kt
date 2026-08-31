/*
 * Spektrafilm for Android — decoded RAW ownership regressions. GPLv3.
 */
package com.spectrafilm.app

import com.spectrafilm.libraw.LinearResult
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RawResultOwnershipTest {
    @Test
    fun previewCopyClosesNativeResultImmediately() {
        val result = decodedResult(width = 2, height = 1)

        val image = rawResultToLinearImage(result, maxEdge = 2048)

        assertTrue(result.isClosed)
        image.close()
    }

    @Test
    fun exportTransferClosesResultButKeepsNativeLeaseAliveUntilImageCloses() {
        val result = decodedResult(width = 2, height = 1)

        val image = rawResultToLinearImage(result, maxEdge = 4097)

        // Ownership has moved: no new LinearResult readers are accepted, while the
        // LinearImage's transferred lease still pins the native allocation.
        assertTrue(result.isClosed)
        image.acquireDataLease().use { lease ->
            val data = lease.data
            assertTrue(data.asFloatBuffer().get(5).isFinite())
        }
        image.close()
        assertTrue(result.isClosed)
        image.close()
        assertTrue(result.isClosed)
    }

    @Test
    fun malformedNativeResultIsClosedOnValidationFailure() {
        val result = LinearResult(
            ByteBuffer.allocateDirect(3 * Float.SIZE_BYTES).order(ByteOrder.nativeOrder()),
            width = 2,
            height = 2,
        )

        assertThrows(IllegalArgumentException::class.java) {
            rawResultToLinearImage(result, maxEdge = 2048)
        }
        assertTrue(result.isClosed)
    }

    private fun decodedResult(width: Int, height: Int): LinearResult {
        val data = ByteBuffer.allocateDirect(width * height * 3 * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
        val floats = data.asFloatBuffer()
        repeat(width * height * 3) { index -> floats.put(index, index.toFloat()) }
        return LinearResult(data, width, height)
    }
}
