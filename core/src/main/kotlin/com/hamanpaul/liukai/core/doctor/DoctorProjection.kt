package com.hamanpaul.liukai.core.doctor

import com.hamanpaul.liukai.core.ime.EditorPolicy

data class DoctorEditorState(
    val inputType: Int,
    val imeOptions: Int,
)

enum class DoctorInputScope {
    UNKNOWN,
    ORDINARY,
    SENSITIVE,
}

enum class DoctorTouchSource(val code: Long) {
    DISPATCH(1),
    SLOT_FORWARD(2),
    KEY_ACTION(3),
}

data class DoctorTouchEvent(
    val gestureToken: Long,
    val pointerCount: Int,
    val pressed: Boolean,
    val source: DoctorTouchSource,
) {
    fun toV1Fields(): Map<String, Long> = mapOf(
        "schemaVersion" to 1L,
        "eventCode" to 1L,
        "gestureToken" to gestureToken,
        "pointerCount" to pointerCount.toLong(),
        "pressed" to if (pressed) 1L else 0L,
        "callbackSource" to source.code,
    )
}

object DoctorEventProjector {
    private const val TYPE_MASK_CLASS = 0x0f
    private const val TYPE_MASK_VARIATION = 0xff0
    private const val TYPE_CLASS_TEXT = 0x1
    private const val TYPE_CLASS_NUMBER = 0x2
    private const val IME_FLAG_NO_PERSONALIZED_LEARNING = 0x1000000
    private val TEXT_VARIATIONS = setOf(
        0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0xa0, 0xb0, 0xc0, 0xd0,
    )

    fun classify(editor: DoctorEditorState?): DoctorInputScope {
        if (editor == null) return DoctorInputScope.UNKNOWN
        if (editor.imeOptions and IME_FLAG_NO_PERSONALIZED_LEARNING != 0) {
            return DoctorInputScope.SENSITIVE
        }
        if (EditorPolicy.isPassword(editor.inputType)) return DoctorInputScope.SENSITIVE
        val inputClass = editor.inputType and TYPE_MASK_CLASS
        val variation = editor.inputType and TYPE_MASK_VARIATION
        return when {
            inputClass == TYPE_CLASS_TEXT && variation in TEXT_VARIATIONS ->
                DoctorInputScope.ORDINARY
            inputClass == TYPE_CLASS_NUMBER && variation == 0 ->
                DoctorInputScope.ORDINARY
            else -> DoctorInputScope.UNKNOWN
        }
    }

    fun touch(
        editor: DoctorEditorState?,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
        source: DoctorTouchSource,
    ): DoctorTouchEvent? {
        if (classify(editor) != DoctorInputScope.ORDINARY) return null
        return DoctorTouchEvent(gestureToken, pointerCount, pressed, source)
    }
}
