package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorClock
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorEventPhase
import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import com.hamanpaul.liukai.core.doctor.DoctorResultCode
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

internal class DoctorInputProducer(
    internal val ring: DoctorSpscRing,
    private val metrics: DoctorInstrumentation,
    private val clock: DoctorClock,
    private val producerThreadId: Long,
    private val wakeWriter: () -> Unit,
) {
    private val collecting = AtomicBoolean(false)
    private val epoch = AtomicLong(0L)
    private val producerId = AtomicLong(0L)
    private val sessionId = AtomicLong(0L)
    private val timezoneOffsetMinutes = AtomicInteger(0)
    private val scope = AtomicInteger(DoctorInputScope.UNKNOWN.ordinal)
    private val mode = AtomicInteger(DoctorEventMode.NONE.ordinal)
    private val focus = AtomicBoolean(false)
    private val pendingDrops = AtomicLong()
    private val pendingGap = AtomicLong()
    private val sequence = AtomicLong()
    private val closeGeneration = AtomicLong()
    private val staged = Array(STAGED_CHAIN_CAPACITY) { DoctorQueueSlot() }
    private var staging = false
    private var stagedCount = 0
    private var stagedEpoch = 0L
    private var stagedCloseGeneration = 0L
    private var operation = 0L
    private var activeOperation = 0L
    private var activeGestureToken = 0L
    private var activePointerCount = 0
    private var activePressed = false

    val isCollecting: Boolean
        get() = collecting.get()

    val isOrdinaryCollecting: Boolean
        get() = collecting.get() &&
            producerId.get() > 0L &&
            sessionId.get() > 0L &&
            scope.get() == DoctorInputScope.ORDINARY.ordinal

    val activeOperationId: Long
        get() = activeOperation

    val activeGesture: Long
        get() = activeGestureToken

    val activePointers: Int
        get() = activePointerCount

    val activeIsPressed: Boolean
        get() = activePressed

    val inputScope: DoctorInputScope
        get() = DoctorInputScope.entries[
            scope.get().coerceIn(0, DoctorInputScope.entries.lastIndex)
        ]

    val inputMode: DoctorEventMode
        get() = DoctorEventMode.entries[
            mode.get().coerceIn(0, DoctorEventMode.entries.lastIndex)
        ]

    val inputFocus: Boolean
        get() = focus.get()

    fun activate(
        epoch: Long,
        producerId: Long,
        sessionId: Long,
        timezoneOffsetMinutes: Int,
    ) {
        if (epoch < 0L || producerId <= 0L || sessionId <= 0L) {
            close()
            return
        }
        if (this.epoch.get() != epoch) {
            sequence.set(0L)
            pendingDrops.set(0L)
            pendingGap.set(0L)
        }
        this.epoch.set(epoch)
        this.producerId.set(producerId)
        this.sessionId.set(sessionId)
        this.timezoneOffsetMinutes.set(timezoneOffsetMinutes)
        collecting.set(true)
    }

    fun close() {
        // The writer only invalidates generation; staging remains producer-thread-owned.
        collecting.set(false)
        closeGeneration.incrementAndGet()
    }

    fun setInputState(inputScope: DoctorInputScope, inputMode: DoctorEventMode, focus: Boolean) {
        scope.set(inputScope.ordinal)
        mode.set(inputMode.ordinal)
        this.focus.set(focus)
    }

    fun beginOperation(): Long {
        if (!isOrdinaryCollecting) return 0L
        if (!isProducerThread()) return 0L
        operation = if (operation == Long.MAX_VALUE) 1L else operation + 1L
        metrics.recordOperationStarted()
        return operation
    }

    fun recordTouch(
        operationId: Long,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
        source: DoctorCallbackSource,
        handled: Boolean,
        durationUs: Long,
    ): Boolean {
        if (operationId <= 0L || gestureToken < 0L || pointerCount < 0) return false
        if (!isOrdinaryCollecting || !isProducerThread()) return false
        return enqueue(ordinaryDrop = true) { slot ->
            slot.eventCode = DoctorEventCode.TOUCH
            slot.callbackSource = source
            slot.phase = DoctorEventPhase.TOUCH
            slot.operationId = operationId
            slot.gestureToken = gestureToken
            slot.hasGestureToken = true
            slot.pointerCount = pointerCount
            slot.hasPointerCount = true
            slot.pressed = pressed
            slot.hasPressed = true
            slot.focus = focus.get()
            slot.handled = handled
            slot.resultCode = if (handled) DoctorResultCode.OK else DoctorResultCode.REJECTED
            slot.durationUs = durationUs.coerceAtLeast(0L)
        }
    }

    fun beginTouchChain(
        operationId: Long,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
    ): Boolean {
        if (operationId <= 0L || !isOrdinaryCollecting || !isProducerThread()) return false
        if (staging) {
            if (stagedCloseGeneration == closeGeneration.get()) {
                metrics.recordProducerViolation()
                return false
            }
            clearStaging()
        }
        staging = true
        stagedCount = 0
        stagedEpoch = epoch.get()
        stagedCloseGeneration = closeGeneration.get()
        val started = recordTouch(
            operationId = operationId,
            gestureToken = gestureToken,
            pointerCount = pointerCount,
            pressed = pressed,
            source = DoctorCallbackSource.DISPATCH,
            handled = false,
            durationUs = 0L,
        )
        if (!started) clearStaging()
        return started
    }

    fun finishTouchChain(handled: Boolean, durationUs: Long) {
        if (!staging) return
        val active = collecting.get() &&
            stagedEpoch == epoch.get() &&
            stagedCloseGeneration == closeGeneration.get() &&
            scope.get() == DoctorInputScope.ORDINARY.ordinal
        if (stagedCount > 0) {
            val dispatch = staged[0]
            dispatch.handled = handled
            dispatch.resultCode =
                if (handled) DoctorResultCode.OK else DoctorResultCode.REJECTED
            dispatch.durationUs = durationUs.coerceAtLeast(0L)
        }
        staging = false
        var published = 0
        if (active) {
            while (published < stagedCount && publishPrepared(staged[published])) {
                published += 1
            }
            while (published < stagedCount) {
                if (staged[published].scope == DoctorInputScope.ORDINARY) {
                    pendingDrops.incrementAndGet()
                    metrics.recordDrop()
                }
                published += 1
            }
            if (published > 0) wakeWriter()
        }
        stagedCount = 0
    }

    fun recordStage(
        operationId: Long,
        eventCode: DoctorEventCode,
        phase: DoctorEventPhase,
        handled: Boolean,
        resultCode: DoctorResultCode,
        durationUs: Long,
    ): Boolean {
        if (operationId <= 0L || !isOrdinaryCollecting || !isProducerThread()) return false
        return enqueue(ordinaryDrop = true) { slot ->
            slot.eventCode = eventCode
            slot.callbackSource = DoctorCallbackSource.KEY_ACTION
            slot.phase = phase
            slot.operationId = operationId
            slot.focus = focus.get()
            slot.handled = handled
            slot.resultCode = resultCode
            slot.durationUs = durationUs.coerceAtLeast(0L)
        }
    }

    fun recordLifecycle(eventCode: DoctorEventCode): Boolean {
        if (!isCollecting || !isProducerThread()) return false
        return enqueue(ordinaryDrop = scope.get() == DoctorInputScope.ORDINARY.ordinal) {
            it.eventCode = eventCode
            it.callbackSource = DoctorCallbackSource.NONE
            it.phase = DoctorEventPhase.LIFECYCLE
            it.operationId = 0L
            it.focus = focus.get()
            it.handled = true
            it.resultCode = DoctorResultCode.OK
            it.durationUs = 0L
        }
    }

    fun recordHealth(): Boolean {
        if (!isCollecting || !isProducerThread()) return false
        return enqueue(ordinaryDrop = scope.get() == DoctorInputScope.ORDINARY.ordinal) {
            it.eventCode = DoctorEventCode.HEALTH
            it.callbackSource = DoctorCallbackSource.NONE
            it.phase = DoctorEventPhase.HEALTH
            it.operationId = 0L
            it.focus = focus.get()
            it.handled = true
            it.resultCode = DoctorResultCode.OK
            it.durationUs = 0L
        }
    }

    inline fun <T> withOperation(
        operationId: Long,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
        block: () -> T,
    ): T {
        val previousOperation = activeOperation
        val previousGesture = activeGestureToken
        val previousPointerCount = activePointerCount
        val previousPressed = activePressed
        activeOperation = operationId
        activeGestureToken = gestureToken
        activePointerCount = pointerCount
        activePressed = pressed
        return try {
            block()
        } finally {
            activeOperation = previousOperation
            activeGestureToken = previousGesture
            activePointerCount = previousPointerCount
            activePressed = previousPressed
        }
    }

    fun recordDiscarded(ordinary: Long, hasSequenceGap: Boolean, resetEpoch: Boolean) {
        repeatLong(ordinary) {
            metrics.recordDrop()
        }
        if (resetEpoch) {
            pendingGap.set(0L)
            pendingDrops.set(0L)
        } else if (hasSequenceGap) {
            pendingGap.incrementAndGet()
        }
    }

    private inline fun enqueue(
        ordinaryDrop: Boolean,
        write: (DoctorQueueSlot) -> Unit,
    ): Boolean {
        if (!collecting.get() || producerId.get() <= 0L || sessionId.get() <= 0L) return false
        if (staging && stagedCloseGeneration != closeGeneration.get()) {
            clearStaging()
        }
        if (staging) {
            if (stagedCount >= staged.size) {
                if (ordinaryDrop) {
                    pendingDrops.incrementAndGet()
                    metrics.recordDrop()
                }
                return false
            }
            val slot = staged[stagedCount]
            prepare(slot)
            write(slot)
            stagedCount += 1
            return true
        }
        val accepted = ring.tryWrite { slot ->
            prepare(slot)
            write(slot)
            annotateAndPublish(slot)
        }
        if (!accepted && ordinaryDrop) {
            pendingDrops.incrementAndGet()
            metrics.recordDrop()
        }
        if (accepted) wakeWriter()
        return accepted
    }

    private fun publishPrepared(source: DoctorQueueSlot): Boolean =
        ring.tryWrite { slot ->
            slot.copyFrom(source)
            annotateAndPublish(slot)
        }

    private fun annotateAndPublish(slot: DoctorQueueSlot) {
        val sequenceValue = sequence.getAndIncrement()
        slot.sequence = sequenceValue
        if (slot.scope == DoctorInputScope.ORDINARY) {
            val drops = pendingDrops.getAndSet(0L)
            if (drops > 0L) {
                slot.dropCount = drops
                slot.hasDropCount = true
            } else {
                slot.hasDropCount = false
            }
            val gap = pendingGap.getAndSet(0L)
            if (gap > 0L) {
                slot.gapFromSequence = sequenceValue - 1L
                slot.hasGapFromSequence = true
            } else {
                slot.hasGapFromSequence = false
            }
        }
        metrics.recordEnqueued()
    }

    private fun prepare(slot: DoctorQueueSlot) {
        slot.epoch = epoch.get()
        slot.operationId = 0L
        slot.sequence = 0L
        slot.monotonicNs = clock.monotonicNs().coerceAtLeast(0L)
        slot.wallUtcMs = clock.wallUtcMs().coerceAtLeast(0L)
        slot.timezoneOffsetMinutes = timezoneOffsetMinutes.get()
        slot.eventCode = DoctorEventCode.HEALTH
        slot.callbackSource = DoctorCallbackSource.NONE
        slot.scope = DoctorInputScope.entries[scope.get().coerceIn(0, DoctorInputScope.entries.lastIndex)]
        slot.mode = DoctorEventMode.entries[mode.get().coerceIn(0, DoctorEventMode.entries.lastIndex)]
        slot.phase = DoctorEventPhase.HEALTH
        slot.focus = false
        slot.handled = false
        slot.resultCode = DoctorResultCode.NONE
        slot.durationUs = 0L
        slot.gestureToken = 0L
        slot.hasGestureToken = false
        slot.pointerCount = 0
        slot.hasPointerCount = false
        slot.pressed = false
        slot.hasPressed = false
        slot.dropCount = 0L
        slot.hasDropCount = false
        slot.gapFromSequence = 0L
        slot.hasGapFromSequence = false
    }

    private fun isProducerThread(): Boolean {
        if (Thread.currentThread().id == producerThreadId) return true
        metrics.recordProducerViolation()
        return false
    }

    private fun repeatLong(count: Long, action: () -> Unit) {
        var remaining = count
        while (remaining > 0L) {
            action()
            remaining -= 1L
        }
    }

    private fun clearStaging() {
        staging = false
        stagedCount = 0
    }

    companion object {
        private const val STAGED_CHAIN_CAPACITY = 32
    }
}
