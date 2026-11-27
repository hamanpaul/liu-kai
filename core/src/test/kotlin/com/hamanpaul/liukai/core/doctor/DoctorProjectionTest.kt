package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class DoctorProjectionTest {
    private val classText = 0x1
    private val classNumber = 0x2
    private val classUnknown = 0x7
    private val typeNull = 0
    private val textPassword = 0x80
    private val textVisiblePassword = 0x90
    private val textWebPassword = 0xe0
    private val numberPassword = 0x10
    private val imeFlagNoPersonalizedLearning = 0x1000000

    @Test
    fun `密碼變體與停用個人化都投影為敏感欄位`() {
        listOf(
            classText or textPassword,
            classText or textVisiblePassword,
            classText or textWebPassword,
            classNumber or numberPassword,
        ).forEach {
            assertEquals(
                DoctorInputScope.SENSITIVE,
                DoctorEventProjector.classify(DoctorEditorState(it, imeOptions = 0)),
            )
        }
        assertEquals(
            DoctorInputScope.SENSITIVE,
            DoctorEventProjector.classify(
                DoctorEditorState(classText, imeFlagNoPersonalizedLearning),
            ),
        )
    }

    @Test
    fun `一般文字與數字可實測為普通欄位`() {
        assertEquals(
            DoctorInputScope.ORDINARY,
            DoctorEventProjector.classify(DoctorEditorState(classText, imeOptions = 0)),
        )
        assertEquals(
            DoctorInputScope.ORDINARY,
            DoctorEventProjector.classify(DoctorEditorState(classNumber, imeOptions = 0)),
        )
    }

    @Test
    fun `TYPE_NULL 缺少 EditorInfo 與不認識型別不可推定為普通欄位`() {
        assertEquals(
            DoctorInputScope.UNKNOWN,
            DoctorEventProjector.classify(DoctorEditorState(typeNull, imeOptions = 0)),
        )
        assertEquals(DoctorInputScope.UNKNOWN, DoctorEventProjector.classify(null))
        assertEquals(
            DoctorInputScope.UNKNOWN,
            DoctorEventProjector.classify(DoctorEditorState(classUnknown, imeOptions = 0)),
        )
        assertEquals(
            DoctorInputScope.UNKNOWN,
            DoctorEventProjector.classify(DoctorEditorState(classText or 0xf0, imeOptions = 0)),
        )
        assertEquals(
            DoctorInputScope.UNKNOWN,
            DoctorEventProjector.classify(DoctorEditorState(classNumber or 0x20, imeOptions = 0)),
        )
    }

    @Test
    fun `匿名 touch 投影只含固定欄位且敏感與未知欄位不逐鍵`() {
        val ordinary = DoctorEditorState(classText, imeOptions = 0)
        val event = DoctorEventProjector.touch(
            ordinary,
            gestureToken = 0x01020304,
            pointerCount = 2,
            pressed = true,
            source = DoctorTouchSource.SLOT_FORWARD,
        )
        assertEquals(
            mapOf(
                "schemaVersion" to 1L,
                "eventCode" to 1L,
                "gestureToken" to 0x01020304L,
                "pointerCount" to 2L,
                "pressed" to 1L,
                "callbackSource" to 2L,
            ),
            event?.toV1Fields(),
        )
        assertEquals(
            0L,
            DoctorEventProjector.touch(
                ordinary,
                gestureToken = 0,
                pointerCount = 1,
                pressed = false,
                source = DoctorTouchSource.DISPATCH,
            )?.toV1Fields()?.getValue("pressed"),
        )

        assertNull(
            DoctorEventProjector.touch(
                DoctorEditorState(classText or textPassword, imeOptions = 0),
                gestureToken = 1,
                pointerCount = 1,
                pressed = true,
                source = DoctorTouchSource.DISPATCH,
            ),
        )
        assertNull(
            DoctorEventProjector.touch(
                DoctorEditorState(classText, imeFlagNoPersonalizedLearning),
                gestureToken = 1,
                pointerCount = 1,
                pressed = true,
                source = DoctorTouchSource.KEY_ACTION,
            ),
        )
        assertNull(
            DoctorEventProjector.touch(
                DoctorEditorState(typeNull, imeOptions = 0),
                gestureToken = 1,
                pointerCount = 1,
                pressed = false,
                source = DoctorTouchSource.KEY_ACTION,
            ),
        )
        assertNull(
            DoctorEventProjector.touch(
                null,
                gestureToken = 1,
                pointerCount = 1,
                pressed = true,
                source = DoctorTouchSource.DISPATCH,
            ),
        )
    }
}
