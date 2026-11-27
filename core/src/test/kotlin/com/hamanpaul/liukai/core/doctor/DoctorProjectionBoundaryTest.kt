package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull

class DoctorProjectionBoundaryTest {
    @Test
    fun `不支援的文字及數字變體不產生觸控事件`() {
        val unsupported = listOf(
            DoctorEditorState(inputType = 0x1 or 0xf0, imeOptions = 0),
            DoctorEditorState(inputType = 0x2 or 0x20, imeOptions = 0),
        )
        unsupported.forEach { editor ->
            assertEquals(DoctorInputScope.UNKNOWN, DoctorEventProjector.classify(editor))
            assertNull(
                DoctorEventProjector.touch(
                    editor,
                    gestureToken = 7,
                    pointerCount = 1,
                    pressed = false,
                    source = DoctorTouchSource.DISPATCH,
                ),
            )
        }
    }

    @Test
    fun `普通欄位的按下狀態清除會序列化為零`() {
        val event = assertNotNull(
            DoctorEventProjector.touch(
                DoctorEditorState(inputType = 0x1, imeOptions = 0),
                gestureToken = 7,
                pointerCount = 1,
                pressed = false,
                source = DoctorTouchSource.KEY_ACTION,
            ),
        )
        assertEquals(0L, event.toV1Fields()["pressed"])
        assertEquals(1L, event.toV1Fields()["pointerCount"])
        assertEquals(7L, event.toV1Fields()["gestureToken"])
        assertEquals(3L, event.toV1Fields()["callbackSource"])
    }
}
