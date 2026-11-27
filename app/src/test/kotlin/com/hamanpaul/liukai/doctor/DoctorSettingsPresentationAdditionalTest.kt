package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorExportPreview
import com.hamanpaul.liukai.core.doctor.DoctorStoreFailure
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

class DoctorSettingsPresentationAdditionalTest {
    @Test fun durationLimitsAreInclusiveAndOverflowIsRejected() {
        val minutes=Long.MAX_VALUE/60_000
        val seconds=Long.MAX_VALUE/1_000
        val max=assertNotNull(DoctorSettingsPresentation.window(minutes.toString(),seconds.toString()))
        assertEquals(minutes*60_000,max.beforeMs);assertEquals(seconds*1_000,max.afterMs)
        assertNull(DoctorSettingsPresentation.window((minutes+1).toString(),"0"))
        assertNull(DoctorSettingsPresentation.window("0",(seconds+1).toString()))
    }
    @Test fun previewReportsOnlyTheMissingEdges() {
        val base=DoctorExportPreview(1,1,1_000,9_000,1_000,9_000,0,0)
        val complete=DoctorSettingsPresentation.preview(base)
        assertFalse(complete.contains("開頭沒有"));assertFalse(complete.contains("結尾沒有"))
        val beginning=DoctorSettingsPresentation.preview(base.copy(oldestRetainedMs=2_000))
        assertTrue(beginning.contains("開頭沒有"));assertFalse(beginning.contains("結尾沒有"))
        val end=DoctorSettingsPresentation.preview(base.copy(newestRetainedMs=8_000))
        assertFalse(end.contains("開頭沒有"));assertTrue(end.contains("結尾沒有"))
        assertTrue(DoctorSettingsPresentation.preview(base.copy(oldestRetainedMs=null)).contains("沒有記錄"))
        assertTrue(DoctorSettingsPresentation.preview(base.copy(newestRetainedMs=null)).contains("沒有記錄"))
    }
    @Test fun unknownFailureDoesNotLeakDetails() {
        assertTrue(DoctorSettingsPresentation.failure(null).contains("稍後重試"))
        assertEquals("無",DoctorSettingsPresentation.time(null))
    }

    @Test fun fixedFailureMapCoversEveryPublicStoreReasonWithoutExceptionText() {
        val groups = mapOf(
            DoctorStoreFailure.CLEAR_INCOMPLETE to "清除尚未完成",
            DoctorStoreFailure.EPOCH_COMMIT_FAILED to "清除尚未完成",
            DoctorStoreFailure.EXPORT_EXPIRED to "預覽已失效",
            DoctorStoreFailure.EPOCH_MISMATCH to "預覽已失效",
            DoctorStoreFailure.INVALID_SNAPSHOT to "預覽已失效",
            DoctorStoreFailure.PERMISSION_DENIED to "沒有檔案存取權限",
            DoctorStoreFailure.DISK_FULL to "儲存空間不足",
            DoctorStoreFailure.EXPORT_LIMIT to "診斷容量上限",
            DoctorStoreFailure.TOTAL_LIMIT to "診斷容量上限",
            DoctorStoreFailure.RECORD_LIMIT to "診斷容量上限",
            DoctorStoreFailure.EXPORT_EXISTS to "已有待確認的匯出",
            DoctorStoreFailure.TORN_TAIL to "部分記錄不完整",
        )
        val fallback = buildList<DoctorStoreFailure?> {
            addAll(DoctorStoreFailure.entries.filterNot(groups::containsKey))
            add(null)
        }

        groups.forEach { (reason, message) ->
            assertTrue(
                DoctorSettingsPresentation.failure(reason).contains(message),
                reason.name,
            )
        }
        fallback.forEach { reason ->
            assertTrue(
                DoctorSettingsPresentation.failure(reason).contains("稍後重試"),
                reason?.name ?: "null",
            )
        }
        buildList<DoctorStoreFailure?> {
            addAll(DoctorStoreFailure.entries)
            add(null)
        }.forEach { reason ->
            val message = DoctorSettingsPresentation.failure(reason)
            assertFalse(message.contains("Exception"))
            assertFalse(message.contains(".kt"))
        }
    }
}
