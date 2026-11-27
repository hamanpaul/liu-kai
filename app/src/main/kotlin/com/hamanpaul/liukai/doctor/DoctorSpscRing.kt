package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorEventPhase
import com.hamanpaul.liukai.core.doctor.DoctorEventRecord
import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import com.hamanpaul.liukai.core.doctor.DoctorResultCode
import java.util.concurrent.atomic.AtomicLong

internal class DoctorQueueSlot {
    var epoch = 0L
    var operationId = 0L
    var sequence = 0L
    var monotonicNs = 0L
    var wallUtcMs = 0L
    var timezoneOffsetMinutes = 0
    var eventCode = DoctorEventCode.HEALTH
    var callbackSource = DoctorCallbackSource.NONE
    var scope = DoctorInputScope.UNKNOWN
    var mode = DoctorEventMode.NONE
    var phase = DoctorEventPhase.HEALTH
    var focus = false
    var handled = false
    var resultCode = DoctorResultCode.NONE
    var durationUs = 0L
    var gestureToken = 0L
    var hasGestureToken = false
    var pointerCount = 0
    var hasPointerCount = false
    var pressed = false
    var hasPressed = false
    var dropCount = 0L
    var hasDropCount = false
    var gapFromSequence = 0L
    var hasGapFromSequence = false

    fun toEventRecord(producerId: Long, sessionId: Long): DoctorEventRecord =
        DoctorEventRecord(
            producerId = producerId,
            sessionId = sessionId,
            epoch = epoch,
            operationId = operationId,
            sequence = sequence,
            monotonicNs = monotonicNs,
            wallUtcMs = wallUtcMs,
            timezoneOffsetMinutes = timezoneOffsetMinutes,
            eventCode = eventCode,
            callbackSource = callbackSource,
            scope = scope,
            mode = mode,
            phase = phase,
            focus = focus,
            handled = handled,
            resultCode = resultCode,
            durationUs = durationUs,
            gestureToken = gestureToken.takeIf { hasGestureToken },
            pointerCount = pointerCount.takeIf { hasPointerCount },
            pressed = pressed.takeIf { hasPressed },
            dropCount = dropCount.takeIf { hasDropCount },
            gapFromSequence = gapFromSequence.takeIf { hasGapFromSequence },
        )

    fun copyFrom(other: DoctorQueueSlot) {
        epoch = other.epoch
        operationId = other.operationId
        sequence = other.sequence
        monotonicNs = other.monotonicNs
        wallUtcMs = other.wallUtcMs
        timezoneOffsetMinutes = other.timezoneOffsetMinutes
        eventCode = other.eventCode
        callbackSource = other.callbackSource
        scope = other.scope
        mode = other.mode
        phase = other.phase
        focus = other.focus
        handled = other.handled
        resultCode = other.resultCode
        durationUs = other.durationUs
        gestureToken = other.gestureToken
        hasGestureToken = other.hasGestureToken
        pointerCount = other.pointerCount
        hasPointerCount = other.hasPointerCount
        pressed = other.pressed
        hasPressed = other.hasPressed
        dropCount = other.dropCount
        hasDropCount = other.hasDropCount
        gapFromSequence = other.gapFromSequence
        hasGapFromSequence = other.hasGapFromSequence
    }
}

internal class DoctorSpscRing(capacity: Int) {
    private val slots = Array(capacity.coerceAtLeast(1)) {
        DoctorQueueSlot()
    }
    private val published = AtomicLong()
    private val consumed = AtomicLong()

    val depth: Long
        get() = (published.get() - consumed.get()).coerceAtLeast(0L)

    inline fun tryWrite(write: (DoctorQueueSlot) -> Unit): Boolean {
        val index = published.get()
        if (index - consumed.get() >= slots.size) return false
        val slot = slots[(index % slots.size).toInt()]
        write(slot)
        published.lazySet(index + 1L)
        return true
    }

    inline fun poll(read: (DoctorQueueSlot) -> Unit): Boolean {
        val index = consumed.get()
        if (index >= published.get()) return false
        read(slots[(index % slots.size).toInt()])
        consumed.lazySet(index + 1L)
        return true
    }

    inline fun discard(read: (DoctorQueueSlot) -> Unit) {
        while (poll(read)) Unit
    }
}
