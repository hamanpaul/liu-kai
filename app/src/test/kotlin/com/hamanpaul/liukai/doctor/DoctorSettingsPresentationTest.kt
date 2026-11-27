package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorExportPreview
import com.hamanpaul.liukai.core.doctor.DoctorStoreFailure
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class DoctorSettingsPresentationTest {
    @Test fun `custom window rejects malformed negative and overflowing durations`() {
        assertEquals(300_000L, DoctorSettingsPresentation.window("5", "30")?.beforeMs)
        assertEquals(30_000L, DoctorSettingsPresentation.window("5", "30")?.afterMs)
        assertEquals(0L, DoctorSettingsPresentation.window("0", "0")?.beforeMs)
        for ((before, after) in listOf("" to "30", "-1" to "30", "5" to "-1", "5" to "x",
            Long.MAX_VALUE.toString() to "0", "0" to Long.MAX_VALUE.toString())) {
            assertNull(DoctorSettingsPresentation.window(before, after))
        }
    }

    @Test fun `preview explains each missing end and a gap from the actual snapshot`() {
        val preview = DoctorExportPreview(7, 12, 1_000, 9_000, 2_000, 8_000, 3, 4)
        val text = DoctorSettingsPresentation.preview(preview)
        assertTrue(text.contains("12"))
        assertTrue(text.contains("開頭"))
        assertTrue(text.contains("結尾"))
        assertTrue(text.contains("4"))
        assertTrue(text.contains("3"))
        assertTrue(DoctorSettingsPresentation.preview(preview.copy(recordCount = 0,
            oldestRetainedMs = null, newestRetainedMs = null)).contains("沒有記錄"))
    }

    @Test fun `failure wording explains incomplete clear and expired preview without raw exception data`() {
        assertTrue(DoctorSettingsPresentation.failure(DoctorStoreFailure.CLEAR_INCOMPLETE).contains("清除"))
        assertTrue(DoctorSettingsPresentation.failure(DoctorStoreFailure.EXPORT_EXPIRED).contains("重新"))
        assertTrue(DoctorSettingsPresentation.failure(DoctorStoreFailure.PERMISSION_DENIED).contains("權限"))
        for (failure in DoctorStoreFailure.entries) {
            assertTrue(DoctorSettingsPresentation.failure(failure).isNotBlank())
        }
    }
}
