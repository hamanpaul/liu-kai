package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorClock
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorEventPhase
import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

class DoctorInputProducerTest {
    @Test
    fun `restricted touch does not allocate operation sequence or gesture counters`() {
        val fixture = Fixture(capacity = 2)
        fixture.producer.activate(
            epoch = 4L,
            producerId = 11L,
            sessionId = 12L,
            timezoneOffsetMinutes = 480,
        )
        fixture.producer.setInputState(DoctorInputScope.SENSITIVE, DoctorEventMode.LIU, focus = true)

        assertEquals(0L, fixture.producer.beginOperation())
        assertFalse(
            fixture.producer.recordTouch(
                operationId = 1L,
                gestureToken = 1L,
                pointerCount = 1,
                pressed = true,
                source = DoctorCallbackSource.DISPATCH,
                handled = true,
                durationUs = 1L,
            ),
        )

        assertEquals(0L, fixture.ring.depth)
        assertEquals(0L, fixture.metrics.snapshot().operationsStarted)
        assertEquals(0L, fixture.metrics.snapshot().enqueued)
    }

    @Test
    fun `one operation and gesture correlate dispatch forwarding and key action`() {
        val fixture = Fixture(capacity = 3)
        fixture.producer.activate(
            epoch = 7L,
            producerId = 21L,
            sessionId = 22L,
            timezoneOffsetMinutes = 480,
        )
        fixture.producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, focus = true)
        val operation = fixture.producer.beginOperation()

        val sources = listOf(
            DoctorCallbackSource.DISPATCH,
            DoctorCallbackSource.SLOT_FORWARD,
            DoctorCallbackSource.KEY_ACTION,
        )
        sources.forEachIndexed { index, source ->
            assertTrue(
                fixture.producer.recordTouch(
                    operationId = operation,
                    gestureToken = operation,
                    pointerCount = 2,
                    pressed = index < 2,
                    source = source,
                    handled = true,
                    durationUs = index.toLong() + 1L,
                ),
            )
        }

        val records = mutableListOf<DoctorQueueRecord>()
        while (fixture.ring.poll { records += it.copyRecord() }) Unit
        assertEquals(3, records.size)
        assertTrue(records.all { it.operationId == operation })
        assertTrue(records.all { it.gestureToken == operation })
        assertEquals(sources, records.map { it.callbackSource })
        assertTrue(records.all { it.pointerCount == 2 })
    }

    @Test
    fun `scope loss before touch finish discards the staged chain`() {
        val fixture = Fixture(capacity = 3)
        fixture.producer.activate(
            epoch = 8L,
            producerId = 23L,
            sessionId = 24L,
            timezoneOffsetMinutes = 480,
        )
        fixture.producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, focus = true)
        val operation = fixture.producer.beginOperation()
        assertTrue(fixture.producer.beginTouchChain(operation, operation, 1, true))

        fixture.producer.setInputState(DoctorInputScope.SENSITIVE, DoctorEventMode.NONE, focus = true)
        fixture.producer.finishTouchChain(handled = true, durationUs = 1L)

        assertEquals(0L, fixture.ring.depth)
        assertEquals(1L, fixture.metrics.snapshot().operationsStarted)
        assertEquals(0L, fixture.metrics.snapshot().enqueued)
        assertEquals(0L, fixture.metrics.snapshot().dropped)
    }

    @Test
    fun `queue overflow drops only ordinary records`() {
        val fixture = Fixture(capacity = 1)
        fixture.producer.activate(
            epoch = 1L,
            producerId = 31L,
            sessionId = 32L,
            timezoneOffsetMinutes = 0,
        )
        fixture.producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.ENGLISH, focus = true)
        val operation = fixture.producer.beginOperation()

        assertTrue(
            fixture.producer.recordTouch(
                operation, operation, 1, true, DoctorCallbackSource.DISPATCH, true, 1L,
            ),
        )
        assertFalse(
            fixture.producer.recordTouch(
                operation, operation, 1, false, DoctorCallbackSource.DISPATCH, true, 1L,
            ),
        )
        fixture.producer.setInputState(DoctorInputScope.UNKNOWN, DoctorEventMode.NONE, focus = false)
        assertFalse(fixture.producer.recordLifecycle(DoctorEventCode.FOCUS))

        assertEquals(1L, fixture.metrics.snapshot().dropped)
        assertEquals(1L, fixture.ring.depth)
    }

    @Test
    fun `restricted lifecycle and health are op zero fixed source and omit touch fields`() {
        val fixture = Fixture(capacity = 2)
        fixture.producer.activate(
            epoch = 3L,
            producerId = 41L,
            sessionId = 42L,
            timezoneOffsetMinutes = -300,
        )
        fixture.producer.setInputState(DoctorInputScope.UNKNOWN, DoctorEventMode.NONE, focus = false)

        assertTrue(fixture.producer.recordLifecycle(DoctorEventCode.FOCUS))
        assertTrue(fixture.producer.recordHealth())

        val records = mutableListOf<DoctorQueueRecord>()
        while (fixture.ring.poll { records += it.copyRecord() }) Unit
        assertEquals(listOf(DoctorEventCode.FOCUS, DoctorEventCode.HEALTH), records.map { it.eventCode })
        assertTrue(records.all { it.operationId == 0L })
        assertTrue(records.all { it.callbackSource == DoctorCallbackSource.NONE })
        assertEquals(DoctorEventPhase.LIFECYCLE, records[0].phase)
        assertEquals(DoctorEventPhase.HEALTH, records[1].phase)
        assertTrue(records.all { it.gestureToken == null })
        assertTrue(records.all { it.pointerCount == null })
        assertTrue(records.all { it.pressed == null })
        assertTrue(records.all { it.dropCount == null })
    }

    @Test
    fun `closing the gate invalidates queued work only through the writer discard`() {
        val fixture = Fixture(capacity = 1)
        fixture.producer.activate(1L, 51L, 52L, 0)
        fixture.producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, focus = true)
        val operation = fixture.producer.beginOperation()
        fixture.producer.recordTouch(operation, operation, 1, true, DoctorCallbackSource.DISPATCH, true, 1L)

        fixture.producer.close()

        assertFalse(fixture.producer.isOrdinaryCollecting)
        assertEquals(1L, fixture.ring.depth)
        assertNotEquals(0L, fixture.metrics.snapshot().enqueued)
    }

    private class Fixture(capacity: Int) {
        val ring = DoctorSpscRing(capacity)
        val metrics = DoctorInstrumentation()
        val producer = DoctorInputProducer(
            ring = ring,
            metrics = metrics,
            clock = MutableClock(),
            producerThreadId = Thread.currentThread().id,
            wakeWriter = {},
        )
    }

    private class MutableClock : DoctorClock {
        private var next = 1_000L
        override fun wallUtcMs(): Long = next++
        override fun monotonicNs(): Long = next * 1_000_000L
    }
}
