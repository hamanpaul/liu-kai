package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorExportPreview
import com.hamanpaul.liukai.core.doctor.DoctorExportWindow
import com.hamanpaul.liukai.core.doctor.DoctorStoreFailure
import java.text.DateFormat
import java.util.Date

internal object DoctorSettingsPresentation {
    fun window(beforeMinutes: String, afterSeconds: String): DoctorExportWindow? {
        val before = beforeMinutes.toLongOrNull() ?: return null
        val after = afterSeconds.toLongOrNull() ?: return null
        if (before !in 0L..Long.MAX_VALUE / 60_000 || after !in 0L..Long.MAX_VALUE / 1_000) return null
        return DoctorExportWindow(before * 60_000, after * 1_000)
    }

    fun time(value: Long?): String {
        if (value == null) return "無"
        return DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.MEDIUM).format(Date(value))
    }

    fun preview(value: DoctorExportPreview): String = buildString {
        val oldest = value.oldestRetainedMs
        val newest = value.newestRetainedMs
        append("這份 ZIP 已固定內容，儲存時不會重新收集。\n")
        append("區間：${time(value.windowStartMs)} ～ ${time(value.windowEndMs)}\n")
        append("記錄：${value.recordCount} 筆\n")
        if (oldest == null || newest == null) {
            append("選定區間沒有記錄。\n")
        } else {
            append("實際保留：${time(value.oldestRetainedMs)} ～ ${time(value.newestRetainedMs)}\n")
            if (oldest > value.windowStartMs) append("區間開頭沒有記錄。\n")
            if (newest < value.windowEndMs) append("區間結尾沒有記錄。\n")
        }
        append("缺段提示：${value.sequenceGap}；略過損毀記錄：${value.ignoredLines}\n")
        append("密碼、私密與未提供欄位種類的輸入框不保留逐鍵記錄。")
    }

    fun failure(reason: DoctorStoreFailure?): String = when (reason) {
        DoctorStoreFailure.CLEAR_INCOMPLETE, DoctorStoreFailure.EPOCH_COMMIT_FAILED ->
            "清除尚未完成；舊記錄已停止讀取與匯出，請重試清除。"
        DoctorStoreFailure.EXPORT_EXPIRED, DoctorStoreFailure.EPOCH_MISMATCH,
        DoctorStoreFailure.INVALID_SNAPSHOT -> "預覽已失效，請重新建立預覽。"
        DoctorStoreFailure.PERMISSION_DENIED -> "沒有檔案存取權限，請確認儲存位置。"
        DoctorStoreFailure.DISK_FULL -> "儲存空間不足，請釋放空間後重試。"
        DoctorStoreFailure.EXPORT_LIMIT, DoctorStoreFailure.TOTAL_LIMIT,
        DoctorStoreFailure.RECORD_LIMIT -> "已達診斷容量上限，請縮小匯出區間或清除記錄。"
        DoctorStoreFailure.EXPORT_EXISTS -> "已有待確認的匯出，請先儲存或取消。"
        DoctorStoreFailure.TORN_TAIL -> "部分記錄不完整，匯出會標示缺漏。"
        else -> "診斷操作未完成，請稍後重試。"
    }
}
