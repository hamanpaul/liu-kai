package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.*
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.assertFailsWith
import java.nio.file.Files

class DoctorInputProducerAdditionalTest {
    private class Fixture(capacity: Int) {
        val ring = DoctorSpscRing(capacity)
        val metrics = DoctorInstrumentation()
        val producer = DoctorInputProducer(ring, metrics, object : DoctorClock {
            override fun wallUtcMs() = 1000L
            override fun monotonicNs() = 2000L
        }, Thread.currentThread().id, {})
        init { producer.activate(0, 1, 2, 0); producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, true) }
        fun records(): List<DoctorQueueRecord> = buildList { while (ring.poll { add(it.copyRecord()) }) Unit }
    }
    @Test fun stagedChainPublishesCorrelatedActionsInOrder() {
        val f=Fixture(4); val op=f.producer.beginOperation()
        assertTrue(f.producer.beginTouchChain(op,op,2,true))
        assertTrue(f.producer.recordStage(op,DoctorEventCode.EDIT_APPLIED,DoctorEventPhase.COMPOSITION,true,DoctorResultCode.OK,12))
        assertEquals(0L,f.ring.depth)
        f.producer.finishTouchChain(true,25)
        val rows=f.records(); assertEquals(2,rows.size)
        assertEquals(listOf(DoctorEventCode.TOUCH,DoctorEventCode.EDIT_APPLIED),rows.map { it.eventCode })
        assertTrue(rows.all { it.operationId==op });assertTrue(rows[0].handled)
    }
    @Test fun failedTouchChainStartDoesNotPoisonFutureStaging() {
        listOf(-1L to 1, 1L to -1).forEach { (gestureToken, pointerCount) ->
            val f = Fixture(4)
            val operation = f.producer.beginOperation()

            assertFalse(
                f.producer.beginTouchChain(
                    operation,
                    gestureToken,
                    pointerCount,
                    true,
                ),
            )
            assertEquals(0L, f.ring.depth)
            assertTrue(f.producer.beginTouchChain(operation, operation, 1, true))
            assertTrue(f.producer.recordTouch(
                operation,
                operation,
                1,
                false,
                DoctorCallbackSource.KEY_ACTION,
                true,
                1L,
            ))
            f.producer.finishTouchChain(true, 2L)

            val rows = f.records()
            assertEquals(2, rows.size)
            assertTrue(rows.all { it.operationId == operation })
            assertEquals(
                listOf(DoctorCallbackSource.DISPATCH, DoctorCallbackSource.KEY_ACTION),
                rows.map { it.callbackSource },
            )
        }
    }
    @Test fun overflowAndDiscardAnnotateOnlyNextOrdinaryEvent() {
        val f=Fixture(1); val op=f.producer.beginOperation()
        fun touch()=f.producer.recordTouch(op,op,1,true,DoctorCallbackSource.DISPATCH,true,1)
        assertTrue(touch());assertFalse(touch());assertEquals(1,f.records().size)
        f.producer.recordDiscarded(2,true,false);assertTrue(touch())
        val next=f.records().single();assertEquals(1L,next.dropCount);assertEquals(0L,next.gapFromSequence)
        assertEquals(3L,f.metrics.snapshot().dropped)
        assertTrue(touch());val clean=f.records().single();assertEquals(null,clean.dropCount);assertEquals(null,clean.gapFromSequence)
    }
    @Test fun nestedOperationContextRestoresAfterException() {
        val f=Fixture(2)
        f.producer.withOperation(3,4,2,true) {
            assertEquals(3L,f.producer.activeOperationId);assertEquals(4L,f.producer.activeGesture)
            assertFailsWith<IllegalStateException> { f.producer.withOperation(5,6,1,false) { throw IllegalStateException("test") } }
            assertEquals(3L,f.producer.activeOperationId);assertEquals(2,f.producer.activePointers);assertTrue(f.producer.activeIsPressed)
        }
        assertEquals(0L,f.producer.activeOperationId);assertEquals(0L,f.producer.activeGesture);assertFalse(f.producer.activeIsPressed)
    }
    @Test fun lifecycleHealthAndRestrictedScopeStayContentFree() {
        val f=Fixture(4)
        f.producer.setInputState(DoctorInputScope.UNKNOWN,DoctorEventMode.NONE,false)
        assertEquals(DoctorInputScope.UNKNOWN,f.producer.inputScope);assertEquals(DoctorEventMode.NONE,f.producer.inputMode);assertFalse(f.producer.inputFocus)
        assertEquals(0L,f.producer.beginOperation());assertTrue(f.producer.recordLifecycle(DoctorEventCode.FOCUS));assertTrue(f.producer.recordHealth())
        val rows=f.records();assertEquals(listOf(DoctorEventCode.FOCUS,DoctorEventCode.HEALTH),rows.map { it.eventCode });assertTrue(rows.all { it.operationId==0L && it.dropCount==null && it.gestureToken==null })
    }
    @Test fun invalidInputsAndEpochChangesFailClosed() {
        val f=Fixture(3);f.producer.activate(-1,1,2,0);assertFalse(f.producer.isCollecting);assertEquals(0L,f.producer.beginOperation())
        f.producer.activate(1,1,2,0);val op=f.producer.beginOperation();assertTrue(op>0)
        assertFalse(f.producer.recordTouch(0,0,1,true,DoctorCallbackSource.DISPATCH,true,0))
        assertFalse(f.producer.recordTouch(op,-1,1,true,DoctorCallbackSource.DISPATCH,true,0))
        assertFalse(f.producer.recordTouch(op,0,-1,true,DoctorCallbackSource.DISPATCH,true,0))
        assertTrue(f.producer.beginTouchChain(op,op,1,true));f.producer.activate(2,1,2,0);f.producer.finishTouchChain(true,0)
        assertEquals(0L,f.ring.depth)
        assertTrue(f.producer.recordHealth());assertEquals(0L,f.records().single().sequence)
    }
    @Test fun stagedOverflowIsAccountedAndNestedChainRejected() {
        val f=Fixture(40);val op=f.producer.beginOperation();assertTrue(f.producer.beginTouchChain(op,op,1,true))
        assertFalse(f.producer.beginTouchChain(op,op,1,true))
        repeat(31) { assertTrue(f.producer.recordTouch(op,op,1,true,DoctorCallbackSource.KEY_ACTION,true,0)) }
        assertFalse(f.producer.recordTouch(op,op,1,true,DoctorCallbackSource.KEY_ACTION,true,0))
        f.producer.finishTouchChain(false,-1);val rows=f.records();assertEquals(32,rows.size);assertFalse(rows[0].handled)
        assertEquals(1L,f.metrics.snapshot().dropped);assertEquals(1L,f.metrics.snapshot().producerViolations)
    }
    @Test fun stagedChainPartialPublishCountsDropOnNextOrdinaryRecord() {
        val f=Fixture(1);val op=f.producer.beginOperation();assertTrue(f.producer.beginTouchChain(op,op,1,true))
        assertTrue(f.producer.recordTouch(op,op,1,false,DoctorCallbackSource.KEY_ACTION,true,4))
        f.producer.finishTouchChain(true,6);assertEquals(1,f.records().size);assertEquals(1L,f.metrics.snapshot().dropped)
        assertTrue(f.producer.recordTouch(op,op,1,false,DoctorCallbackSource.DISPATCH,true,0))
        assertEquals(1L,f.records().single().dropCount)
    }
    @Test fun resetEpochDropsStaleGapAnnotations() {
        val f=Fixture(1);val op=f.producer.beginOperation()
        assertTrue(f.producer.recordTouch(op,op,1,true,DoctorCallbackSource.DISPATCH,true,0))
        assertFalse(f.producer.recordTouch(op,op,1,true,DoctorCallbackSource.DISPATCH,true,0));f.records()
        f.producer.recordDiscarded(1,true,true);assertTrue(f.producer.recordHealth())
        val clean=f.records().single();assertEquals(null,clean.dropCount);assertEquals(null,clean.gapFromSequence)
    }
    @Test fun restrictedFailedHealthDoesNotCountAsOrdinaryDrop() {
        val f=Fixture(1);assertTrue(f.producer.recordHealth());f.producer.setInputState(DoctorInputScope.SENSITIVE,DoctorEventMode.NONE,false)
        assertFalse(f.producer.recordHealth());assertEquals(0L,f.metrics.snapshot().dropped);f.records()
        f.producer.setInputState(DoctorInputScope.ORDINARY,DoctorEventMode.LIU,true);assertTrue(f.producer.recordHealth())
        assertEquals(null,f.records().single().dropCount)
    }
    @Test fun stagedOrdinaryTouchIsDiscardedWhenScopeBecomesSensitive() {
        val f=Fixture(4)
        val op=f.producer.beginOperation()
        assertTrue(f.producer.beginTouchChain(op,op,1,true))
        f.producer.setInputState(DoctorInputScope.SENSITIVE,DoctorEventMode.NONE,true)
        assertEquals(0L,f.producer.beginOperation())
        assertFalse(f.producer.recordTouch(op,op,1,true,DoctorCallbackSource.DISPATCH,true,0))
        f.producer.finishTouchChain(true,4)
        assertEquals(0L,f.ring.depth)
        f.producer.setInputState(DoctorInputScope.ORDINARY,DoctorEventMode.LIU,true)
        val next=f.producer.beginOperation()
        assertEquals(op+1,next)
        assertTrue(f.producer.recordTouch(next,next,1,true,DoctorCallbackSource.DISPATCH,true,0))
        val record=f.records().single()
        assertEquals(0L,record.sequence)
        assertEquals(next,record.operationId)
        assertEquals(null,record.dropCount)
    }
    @Test fun spscRingPreservesOrderAcrossRealThreads() {
        val ring=DoctorSpscRing(8);val failures=java.util.concurrent.atomic.AtomicReference<String?>(null)
        val start=java.util.concurrent.CountDownLatch(1);val deadline=System.nanoTime()+java.util.concurrent.TimeUnit.SECONDS.toNanos(10)
        val writer=Thread {
            start.await()
            for (i in 1..1000) {
                while (!ring.tryWrite { it.operationId=i.toLong() }) {
                    if (System.nanoTime()>deadline) { failures.set("writer timeout");return@Thread };Thread.yield()
                }
            }
        }
        val reader=Thread {
            start.await()
            for (i in 1..1000) {
                var value=0L
                while (!ring.poll { value=it.operationId }) {
                    if (System.nanoTime()>deadline) { failures.set("reader timeout");return@Thread };Thread.yield()
                }
                if (value!=i.toLong()) { failures.set("expected $i observed $value");return@Thread }
            }
        }
        writer.start();reader.start();start.countDown();writer.join(12000);reader.join(12000)
        assertFalse(writer.isAlive);assertFalse(reader.isAlive);assertEquals(null,failures.get());assertEquals(0L,ring.depth)
    }
    @Test fun wrongThreadCannotAllocateOrWriteEvents() {
        val f=Fixture(2);var op=-1L
        val t=Thread { op=f.producer.beginOperation() };t.start();t.join()
        assertEquals(0L,op);assertEquals(0L,f.ring.depth);assertEquals(1L,f.metrics.snapshot().producerViolations)
    }

    @Test fun wrongThreadCannotUseAnyProducerCallbackOrAllocateSharedState() {
        val ring = DoctorSpscRing(8)
        val metrics = DoctorInstrumentation()
        val clockReads = java.util.concurrent.atomic.AtomicInteger()
        val wakeCalls = java.util.concurrent.atomic.AtomicInteger()
        val producer = DoctorInputProducer(
            ring,
            metrics,
            object : DoctorClock {
                override fun wallUtcMs(): Long {
                    clockReads.incrementAndGet()
                    return 1_000L
                }

                override fun monotonicNs(): Long {
                    clockReads.incrementAndGet()
                    return 2_000L
                }
            },
            Thread.currentThread().id,
            { wakeCalls.incrementAndGet() },
        )
        producer.activate(4L, 1L, 2L, 0)
        producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, true)

        val results = java.util.concurrent.LinkedBlockingQueue<Any>()
        val worker = Thread {
            results.add(producer.beginOperation())
            results.add(producer.recordTouch(
                1L, 1L, 1, true, DoctorCallbackSource.DISPATCH, true, 1L,
            ))
            results.add(producer.beginTouchChain(1L, 1L, 1, true))
            results.add(producer.recordStage(
                1L,
                DoctorEventCode.EDIT_APPLIED,
                DoctorEventPhase.COMMIT,
                true,
                DoctorResultCode.OK,
                1L,
            ))
            results.add(producer.recordLifecycle(DoctorEventCode.FOCUS))
            results.add(producer.recordHealth())
        }
        worker.start()
        worker.join(5_000)

        assertFalse(worker.isAlive)
        assertEquals(0L, results.poll())
        repeat(5) { assertEquals(false, results.poll()) }
        assertEquals(0, clockReads.get())
        assertEquals(0, wakeCalls.get())
        assertEquals(0L, ring.depth)
        assertEquals(0L, metrics.snapshot().operationsStarted)
        assertEquals(0L, metrics.snapshot().enqueued)
        assertEquals(0L, metrics.snapshot().dropped)
        assertEquals(6L, metrics.snapshot().producerViolations)

        val ownerOperation = producer.beginOperation()
        assertEquals(1L, ownerOperation)
        assertTrue(producer.recordHealth())
        val record = buildList {
            while (ring.poll { add(it.copyRecord()) }) Unit
        }.single()
        assertEquals(0L, record.sequence)
        assertEquals(DoctorEventCode.HEALTH, record.eventCode)
    }

    @Test fun restrictedInputGuardAvoidsClockAndCallbackMetrics() {
        var clockReads = 0
        val ring = DoctorSpscRing(2)
        val metrics = DoctorInstrumentation()
        val producer = DoctorInputProducer(
            ring,
            metrics,
            object : DoctorClock {
                override fun wallUtcMs(): Long {
                    clockReads += 1
                    return 1_000L
                }

                override fun monotonicNs(): Long {
                    clockReads += 1
                    return 2_000L
                }
            },
            Thread.currentThread().id,
            {},
        )
        producer.activate(0L, 1L, 2L, 0)
        producer.setInputState(DoctorInputScope.SENSITIVE, DoctorEventMode.NONE, true)
        val before = metrics.snapshot()

        assertEquals(0L, producer.beginOperation())
        assertFalse(producer.beginTouchChain(1L, 1L, 1, true))
        assertFalse(producer.recordTouch(
            1L,
            1L,
            1,
            true,
            DoctorCallbackSource.DISPATCH,
            true,
            1L,
        ))
        assertFalse(producer.recordStage(
            1L,
            DoctorEventCode.EDIT_APPLIED,
            DoctorEventPhase.COMMIT,
            true,
            DoctorResultCode.OK,
            1L,
        ))

        assertEquals(0, clockReads)
        assertEquals(before, metrics.snapshot())
        assertEquals(0L, ring.depth)
    }

    @Test fun closeDiscardsAnUnfinishedStagedChain() {
        val f = Fixture(4)
        val operation = f.producer.beginOperation()
        assertTrue(f.producer.beginTouchChain(operation, operation, 1, true))
        assertTrue(f.producer.recordTouch(
            operation,
            operation,
            1,
            false,
            DoctorCallbackSource.KEY_ACTION,
            true,
            1L,
        ))

        f.producer.close()
        f.producer.finishTouchChain(true, 2L)

        assertEquals(0L, f.ring.depth)
        assertEquals(0L, f.metrics.snapshot().enqueued)
        assertFalse(f.producer.isCollecting)
    }

    @Test fun sameEpochReactivationContinuesLifecycleSequenceAfterClose() {
        val f = Fixture(4)

        assertTrue(f.producer.recordLifecycle(DoctorEventCode.FOCUS))
        val before = f.records().single()

        f.producer.close()
        assertFalse(f.producer.recordLifecycle(DoctorEventCode.DEACTIVATE))
        assertEquals(0L, f.ring.depth)

        f.producer.activate(before.epoch, 1L, 2L, 0)
        assertTrue(f.producer.recordLifecycle(DoctorEventCode.DEACTIVATE))

        val after = f.records().single()
        assertEquals(before.epoch, after.epoch)
        assertEquals(before.sequence + 1L, after.sequence)
        assertEquals(DoctorEventCode.FOCUS, before.eventCode)
        assertEquals(DoctorEventCode.DEACTIVATE, after.eventCode)
        assertEquals(0L, f.metrics.snapshot().dropped)
    }

    @Test fun writerCloseThenSameEpochReactivationCannotResurrectStagedChain() {
        val root = Files.createTempDirectory("doctor-producer-close")
        try {
            val failAppend = java.util.concurrent.atomic.AtomicBoolean()
            val clock = object : DoctorClock {
                override fun wallUtcMs() = 1_000L
                override fun monotonicNs() = 2_000L
            }
            val build = DoctorBuildMetadata("0.0.0-test", "1234567")
            val store = DoctorStore.open(
                root = root,
                build = build,
                clock = clock,
                ioFaults = object : DoctorIoFaults {
                    override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                        if (
                            operation == DoctorIoOperation.APPEND_RECORD &&
                            failAppend.compareAndSet(true, false)
                        ) {
                            DoctorStoreFailure.IO_ERROR
                        } else {
                            null
                        }
                },
            )
            val ring = DoctorSpscRing(8)
            val metrics = DoctorInstrumentation()
            val producer = DoctorInputProducer(
                ring,
                metrics,
                clock,
                Thread.currentThread().id,
                {},
            )
            producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, true)
            val writer = DoctorWriterCore(
                store = store,
                ring = ring,
                producer = producer,
                metrics = metrics,
                producerId = 101L,
                sessionId = 102L,
                timezoneOffsetMinutes = 0,
                build = build,
            )
            writer.enable().also { assertTrue(it is DoctorStoreResult.Ok) }
            assertTrue(producer.recordHealth())
            val staleOperation = producer.beginOperation()
            assertTrue(producer.beginTouchChain(staleOperation, staleOperation, 1, true))
            assertTrue(producer.recordTouch(
                staleOperation,
                staleOperation,
                1,
                false,
                DoctorCallbackSource.KEY_ACTION,
                true,
                1L,
            ))

            failAppend.set(true)
            val pumpStarted = java.util.concurrent.CountDownLatch(1)
            val closed = java.util.concurrent.CountDownLatch(1)
            val reactivate = java.util.concurrent.CountDownLatch(1)
            val reactivated = java.util.concurrent.CountDownLatch(1)
            val writerFailure =
                java.util.concurrent.atomic.AtomicReference<Throwable?>()
            var reactivateResult: DoctorStoreResult<Unit>? = null
            val writerThread = Thread({
                try {
                    pumpStarted.await()
                    writer.pumpOneBatch()
                    closed.countDown()
                    reactivate.await()
                    reactivateResult = writer.enable()
                    reactivated.countDown()
                } catch (failure: Throwable) {
                    writerFailure.set(failure)
                    closed.countDown()
                    reactivated.countDown()
                }
            }, "liu-kai-doctor-writer-test").apply { isDaemon = true }
            writerThread.start()
            pumpStarted.countDown()
            assertTrue(
                closed.await(5_000, java.util.concurrent.TimeUnit.MILLISECONDS),
            )
            assertEquals(null, writerFailure.get())
            assertFalse(producer.isCollecting)

            reactivate.countDown()
            assertTrue(
                reactivated.await(5_000, java.util.concurrent.TimeUnit.MILLISECONDS),
            )
            assertEquals(null, writerFailure.get())
            assertTrue(reactivateResult is DoctorStoreResult.Ok)
            assertTrue(producer.isCollecting)
            producer.finishTouchChain(true, 2L)
            assertEquals(0L, ring.depth)

            val freshOperation = producer.beginOperation()
            assertEquals(staleOperation + 1L, freshOperation)
            assertTrue(producer.beginTouchChain(freshOperation, freshOperation, 1, true))
            assertTrue(producer.recordTouch(
                freshOperation,
                freshOperation,
                1,
                false,
                DoctorCallbackSource.KEY_ACTION,
                true,
                3L,
            ))
            producer.finishTouchChain(true, 4L)
            val rows = buildList {
                while (ring.poll { add(it.copyRecord()) }) Unit
            }
            assertEquals(2, rows.size)
            assertTrue(rows.all { it.operationId == freshOperation })
            assertEquals(listOf(1L, 2L), rows.map { it.sequence })
            assertEquals(0L, metrics.snapshot().producerViolations)
            writerThread.join(5_000)
            assertFalse(writerThread.isAlive)
        } finally {
            root.toFile().deleteRecursively()
        }
    }

    @Test fun invalidProducerOrSessionCannotOpenTheCollectionGate() {
        val f = Fixture(1)
        f.producer.close()

        f.producer.activate(1L, 0L, 2L, 0)
        assertFalse(f.producer.isCollecting)
        f.producer.activate(1L, 1L, 0L, 0)
        assertFalse(f.producer.isCollecting)
        assertEquals(0L, f.producer.beginOperation())
    }

}
