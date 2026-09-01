package com.spectrafilm.engine

import org.junit.Assert.assertEquals
import org.junit.Test

class MemoryBudgetStageTest {
    @Test
    fun stableNativeCodesRemainOrderedAndAppendOnly() {
        assertEquals(
            listOf(
                "UNKNOWN" to 0,
                "JNI_SIM_RESULT" to 1,
                "JNI_DIRECT_BUFFER" to 2,
                "DECODE" to 3,
                "FILMING" to 4,
                "SCANNING" to 5,
                "PRINTING" to 6,
                "SPATIAL" to 7,
                "GRAIN" to 8,
                "LUT" to 9,
                "WRITER" to 10,
                "GPU" to 11,
            ),
            MemoryBudgetStage.entries.map { it.name to it.nativeCode },
        )
    }
}
