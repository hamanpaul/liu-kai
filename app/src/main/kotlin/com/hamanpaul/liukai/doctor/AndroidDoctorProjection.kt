package com.hamanpaul.liukai.doctor

import android.view.inputmethod.EditorInfo
import com.hamanpaul.liukai.core.doctor.DoctorEditorState
import com.hamanpaul.liukai.core.doctor.DoctorEventProjector
import com.hamanpaul.liukai.core.doctor.DoctorInputScope

internal fun EditorInfo.doctorEditorState(): DoctorEditorState =
    DoctorEditorState(inputType, imeOptions)

internal fun DoctorEditorState?.doctorInputScope(): DoctorInputScope =
    DoctorEventProjector.classify(this)
