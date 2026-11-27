package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorEventPhase
import com.hamanpaul.liukai.core.doctor.DoctorInputScope

internal data class DoctorQueueRecord(
    val epoch: Long,
    val operationId: Long,
    val sequence: Long,
    val eventCode: DoctorEventCode,
    val callbackSource: DoctorCallbackSource,
    val scope: DoctorInputScope,
    val mode: DoctorEventMode,
    val phase: DoctorEventPhase,
    val focus: Boolean,
    val handled: Boolean,
    val gestureToken: Long?,
    val pointerCount: Int?,
    val pressed: Boolean?,
    val dropCount: Long?,
    val gapFromSequence: Long?,
)

internal fun DoctorQueueSlot.copyRecord(): DoctorQueueRecord =
    DoctorQueueRecord(
        epoch = epoch,
        operationId = operationId,
        sequence = sequence,
        eventCode = eventCode,
        callbackSource = callbackSource,
        scope = scope,
        mode = mode,
        phase = phase,
        focus = focus,
        handled = handled,
        gestureToken = gestureToken.takeIf { hasGestureToken },
        pointerCount = pointerCount.takeIf { hasPointerCount },
        pressed = pressed.takeIf { hasPressed },
        dropCount = dropCount.takeIf { hasDropCount },
        gapFromSequence = gapFromSequence.takeIf { hasGapFromSequence },
    )
