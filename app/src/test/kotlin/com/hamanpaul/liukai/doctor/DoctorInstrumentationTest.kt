package com.hamanpaul.liukai.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class DoctorInstrumentationTest {
    @Test
    fun `long tail histograms resolve one tenth millisecond and retain overflow`() {
        val metrics = DoctorInstrumentation()
        metrics.recordDispatch(enabled = false, durationUs = 8_001)
        metrics.recordDispatch(enabled = false, durationUs = 8_099)
        metrics.recordDispatch(enabled = false, durationUs = 8_100)
        metrics.recordHardware(enabled = true, durationUs = 32_099)
        metrics.recordHardware(enabled = true, durationUs = Long.MAX_VALUE)
        metrics.recordHardware(enabled = true, durationUs = Long.MIN_VALUE)
        val snapshot = metrics.snapshot()
        assertEquals(2_561, snapshot.dispatchOff.size)
        assertEquals(2L, snapshot.dispatchOff[80])
        assertEquals(1L, snapshot.dispatchOff[81])
        assertEquals(1L, snapshot.hardwareOn[320])
        assertEquals(1L, snapshot.hardwareOn.last())
        assertEquals(1L, snapshot.hardwareOn.first())
        assertEquals(3L, snapshot.hardwareOn.sum())
        metrics.resetForTest()
        assertEquals(0L, metrics.snapshot().dispatchOff.sum())
        assertEquals(0L, metrics.snapshot().hardwareOn.sum())
    }

    @Test
    fun `dispatch sampler uses fixed off and on histograms`() {
        val instrumentation = DoctorInstrumentation()

        instrumentation.recordDispatch(enabled = false, durationUs = 10L)
        instrumentation.recordDispatch(enabled = true, durationUs = 900L)

        val snapshot = instrumentation.snapshot()
        assertEquals(1L, snapshot.dispatchOff.sum())
        assertEquals(1L, snapshot.dispatchOn.sum())
        assertEquals(snapshot.dispatchOff.size, snapshot.dispatchOn.size)
    }

    @Test
    fun `commit booleans are measured from actual attempted and committed results`() {
        val instrumentation = DoctorInstrumentation()

        instrumentation.recordCommit(ordinary = true, committed = true)
        instrumentation.recordCommit(ordinary = true, committed = false)

        val snapshot = instrumentation.snapshot()
        assertEquals(2L, snapshot.commitAttempts)
        assertEquals(1L, snapshot.commitSuccesses)
        assertEquals(1, snapshot.lastCommitAttempted)
        assertEquals(0, snapshot.lastCommitCommitted)
    }

    @Test
    fun `private and unknown fields cannot update callback metrics`() {
        val instrumentation = DoctorInstrumentation()

        instrumentation.recordDispatch(ordinary = false, enabled = false, durationUs = 10L)
        instrumentation.recordHardware(ordinary = false, enabled = true, durationUs = 20L)
        instrumentation.recordCommit(ordinary = false, committed = true)

        val snapshot = instrumentation.snapshot()
        assertTrue(snapshot.dispatchOff.all { it == 0L })
        assertTrue(snapshot.dispatchOn.all { it == 0L })
        assertTrue(snapshot.hardwareOff.all { it == 0L })
        assertTrue(snapshot.hardwareOn.all { it == 0L })
        assertEquals(0L, snapshot.commitAttempts)
        assertEquals(0L, snapshot.commitSuccesses)
        assertEquals(0, snapshot.lastCommitAttempted)
        assertEquals(0, snapshot.lastCommitCommitted)
    }

    @Test
    fun `reset clears accumulated measurements and samples`() {
        val instrumentation = DoctorInstrumentation()
        instrumentation.recordOperationStarted()
        instrumentation.recordProducerViolation()
        instrumentation.recordEnqueued()
        instrumentation.recordWritten(bytes = 123L)
        instrumentation.recordDrop()
        instrumentation.recordDispatch(enabled = false, durationUs = 100L)
        instrumentation.recordHardware(enabled = true, durationUs = 200L)
        instrumentation.recordCommit(ordinary = true, committed = true)
        val populated = instrumentation.snapshot()
        assertEquals(1L, populated.operationsStarted)
        assertEquals(1L, populated.producerViolations)
        assertEquals(123L, populated.writtenBytes)
        assertEquals(1L, instrumentation.droppedCount)

        instrumentation.resetForTest()

        val snapshot = instrumentation.snapshot()
        assertEquals(0L, snapshot.enqueued)
        assertEquals(0L, snapshot.operationsStarted)
        assertEquals(0L, snapshot.producerViolations)
        assertEquals(0L, snapshot.written)
        assertEquals(0L, snapshot.writtenBytes)
        assertEquals(0L, snapshot.dropped)
        assertTrue(snapshot.dispatchOff.all { it == 0L })
        assertTrue(snapshot.dispatchOn.all { it == 0L })
        assertTrue(snapshot.hardwareOff.all { it == 0L })
        assertTrue(snapshot.hardwareOn.all { it == 0L })
        assertEquals(0L, snapshot.commitAttempts)
        assertEquals(0L, snapshot.commitSuccesses)
        assertEquals(0, snapshot.lastCommitAttempted)
        assertEquals(0, snapshot.lastCommitCommitted)
    }
}
