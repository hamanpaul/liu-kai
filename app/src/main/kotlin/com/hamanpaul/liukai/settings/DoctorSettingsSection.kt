package com.hamanpaul.liukai.settings

import android.app.Activity
import android.app.AlertDialog
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import com.hamanpaul.liukai.core.doctor.DoctorExportPreview
import com.hamanpaul.liukai.doctor.AndroidDoctorController
import com.hamanpaul.liukai.doctor.DoctorExportHandle
import com.hamanpaul.liukai.doctor.DoctorPreparedExport
import com.hamanpaul.liukai.doctor.DoctorRuntimeListener
import com.hamanpaul.liukai.doctor.DoctorRuntimeStatus
import com.hamanpaul.liukai.doctor.DoctorSafExportTarget
import com.hamanpaul.liukai.doctor.DoctorSettingsPresentation
import com.hamanpaul.liukai.doctor.DoctorUiOperation
import com.hamanpaul.liukai.doctor.DoctorUiResult
import java.util.Calendar

/** Existing settings UI, backed by the same process writer as the IME. */
internal class DoctorSettingsSection(
    private val activity: Activity,
    root: LinearLayout,
    savedState: Bundle?,
) : DoctorRuntimeListener {
    private val controller = AndroidDoctorController.get(activity)
    private val summary = TextView(activity)
    private val message = TextView(activity).apply { setTextIsSelectable(true) }
    private val actions = mutableListOf<Button>()
    private var enabled = false
    private var busy = false
    private var prepared: DoctorPreparedExport? = restore(savedState?.getBundle(STATE))
    private var choosingDestination = savedState?.getBundle(STATE)?.getBoolean("choosing") == true
    private var dialog: AlertDialog? = null
    private val toggle: Button
    private val subscription = controller.subscribe(this)

    init {
        root.addView(TextView(activity).apply { text = "Doctor 診斷"; textSize = 18f })
        root.addView(summary)
        root.addView(TextView(activity).apply {
            text = "只在本機保留狀態與耗時，不記錄輸入文字、按鍵內容或座標。密碼、私密與未知欄位不保留逐鍵事件。記錄最多 32 MiB、最長 7 天；沒有自動上傳。"
        })
        toggle = button(root, "開啟 Doctor 診斷") {
            if (enabled) request { controller.setEnabled(false, subscription) }
            else AlertDialog.Builder(activity).setTitle("開啟本機診斷？")
                .setMessage("啟用後會記住選擇，輸入法重新開啟後續記。你可以隨時停用、預覽匯出或清除記錄。")
                .setNegativeButton("取消", null)
                .setPositiveButton("開啟") { _, _ -> request { controller.setEnabled(true, subscription) } }
                .show()
        }
        button(root, "標記剛才的問題", ::mark)
        button(root, "預覽與匯出") { request { controller.prepareExport(subscription) } }
        button(root, "清除診斷記錄") {
            AlertDialog.Builder(activity).setTitle("清除診斷記錄？")
                .setMessage("刪除本機原始記錄及待確認的匯出；你已另存的 ZIP 由你自行管理。")
                .setNegativeButton("取消", null)
                .setPositiveButton("清除") { _, _ -> request { controller.clear(subscription) } }.show()
        }
        root.addView(message)
        if (prepared != null && !choosingDestination) root.post { preview() }
    }

    private fun button(root: LinearLayout, label: String, action: () -> Unit): Button =
        Button(activity).apply {
            text = label
            setOnClickListener { action() }
            actions += this
            root.addView(this)
        }

    private fun request(action: () -> Unit) {
        if (busy) return
        busy = true
        actions.forEach { it.isEnabled = false }
        message.text = "處理中…"
        action()
    }

    override fun onDoctorStatus(status: DoctorRuntimeStatus) {
        enabled = status.enabled
        toggle.text = if (enabled) "關閉 Doctor 診斷" else "開啟 Doctor 診斷"
        val condition = when {
            status.pendingClear -> "清除未完成"
            status.interrupted -> "收集已中斷"
            status.collecting -> "收集中"
            status.enabled -> "已開啟，等待收集"
            else -> "已關閉，保留既有記錄"
        }
        summary.text = "$condition\n保留：${DoctorSettingsPresentation.time(status.oldestRetainedMs)} ～ " +
            "${DoctorSettingsPresentation.time(status.newestRetainedMs)}\n" +
            "原始記錄 ${status.rawBytes / 1024} KiB；暫存 ${status.tempBytes / 1024} KiB\n" +
            "缺段提示 ${status.sequenceGap}；背景丟筆 ${status.dropped}；略過損毀記錄 ${status.ignoredLines}" +
            (status.failure?.let { "\n${DoctorSettingsPresentation.failure(it)}" } ?: "")
    }

    override fun onDoctorResult(result: DoctorUiResult) {
        busy = false
        actions.forEach { it.isEnabled = true }
        if (!result.success) {
            message.text = DoctorSettingsPresentation.failure(result.failure)
            if (result.operation == DoctorUiOperation.SAVE) cancelPrepared()
            return
        }
        message.text = when (result.operation) {
            DoctorUiOperation.ENABLE -> "已開啟本機診斷。"
            DoctorUiOperation.DISABLE -> "已停止新記錄，既有記錄仍保留。"
            DoctorUiOperation.MARK -> "已標記問題時間與區間，可以預覽匯出。"
            DoctorUiOperation.CLEAR -> { prepared = null; "診斷記錄已清除。" }
            DoctorUiOperation.SAVE -> { prepared = null; "診斷 ZIP 已儲存。" }
            DoctorUiOperation.CANCEL_EXPORT -> "已取消匯出。"
            DoctorUiOperation.PREPARE -> {
                prepared = result.export
                preview()
                "預覽已建立。"
            }
        }
    }

    private fun mark() {
        var selected = System.currentTimeMillis()
        val form = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        val issue = TextView(activity).apply { text = "問題時間：${DoctorSettingsPresentation.time(selected)}" }
        form.addView(issue)
        Button(activity).apply {
            text = "改選問題時間"
            setOnClickListener {
                val date = Calendar.getInstance().apply { timeInMillis = selected }
                DatePickerDialog(activity, { _, year, month, day ->
                    date.set(year, month, day)
                    TimePickerDialog(activity, { _, hour, minute ->
                        date.set(Calendar.HOUR_OF_DAY, hour)
                        date.set(Calendar.MINUTE, minute)
                        date.set(Calendar.SECOND, 0)
                        date.set(Calendar.MILLISECOND, 0)
                        selected = date.timeInMillis
                        issue.text = "問題時間：${DoctorSettingsPresentation.time(selected)}"
                    }, date.get(Calendar.HOUR_OF_DAY), date.get(Calendar.MINUTE), true).show()
                }, date.get(Calendar.YEAR), date.get(Calendar.MONTH), date.get(Calendar.DAY_OF_MONTH)).show()
            }
            form.addView(this)
        }
        fun duration(label: String, initial: String): EditText {
            form.addView(TextView(activity).apply { text = label })
            return EditText(activity).apply {
                inputType = InputType.TYPE_CLASS_NUMBER
                setText(initial)
                contentDescription = label
                form.addView(this)
            }
        }
        val before = duration("問題前幾分鐘", "5")
        val after = duration("問題後幾秒", "30")
        val error = TextView(activity).also(form::addView)
        val markDialog = AlertDialog.Builder(activity).setTitle("標記問題")
            .setView(form).setNegativeButton("取消", null).setPositiveButton("標記", null).create()
        markDialog.setOnShowListener {
            markDialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val window = DoctorSettingsPresentation.window(before.text.toString(), after.text.toString())
                if (window == null || selected < 0) error.text = "請輸入有效的非負時間。"
                else {
                    request { controller.markIssue(selected, subscription, window) }
                    markDialog.dismiss()
                }
            }
        }
        dialog = markDialog
        markDialog.show()
    }

    private fun preview() {
        val snapshot = prepared ?: return
        dialog = AlertDialog.Builder(activity).setTitle("診斷 ZIP 預覽")
            .setMessage(DoctorSettingsPresentation.preview(snapshot.preview))
            .setNegativeButton("取消") { _, _ -> cancelPrepared(reportResult = true) }
            .setPositiveButton("選擇儲存位置") { _, _ ->
                choosingDestination = true
                activity.startActivityForResult(Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "application/zip"
                    putExtra(Intent.EXTRA_TITLE, "liu-kai-doctor.zip")
                }, EXPORT_REQUEST)
            }.setOnCancelListener { cancelPrepared(reportResult = true) }.show()
    }

    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (requestCode != EXPORT_REQUEST) return false
        choosingDestination = false
        val snapshot = prepared
        val uri = data?.data
        if (resultCode != Activity.RESULT_OK || uri == null) {
            if (snapshot == null) message.text = "已取消選擇儲存位置。"
            else cancelPrepared(reportResult = true)
        } else if (snapshot == null) {
            message.text = "預覽已失效，請重新建立預覽。"
        } else {
            request { controller.saveExport(snapshot.handle, DoctorSafExportTarget(activity, uri), subscription) }
        }
        return true
    }

    private fun cancelPrepared(reportResult: Boolean = false) {
        val snapshot = prepared
        prepared = null
        if (snapshot != null) {
            if (reportResult) request { controller.cancelExport(snapshot.handle, subscription) }
            else controller.cancelExport(snapshot.handle, null)
        }
    }

    fun saveState(out: Bundle) {
        val snapshot = prepared ?: return
        val p = snapshot.preview
        out.putBundle(STATE, Bundle().apply {
            putLong("token", snapshot.handle.token); putLong("epoch", snapshot.handle.epoch)
            putLong("expires", snapshot.handle.expiresAtMs); putBoolean("choosing", choosingDestination)
            putInt("count", p.recordCount); putLong("start", p.windowStartMs); putLong("end", p.windowEndMs)
            putLong("oldest", p.oldestRetainedMs ?: -1); putLong("newest", p.newestRetainedMs ?: -1)
            putLong("ignored", p.ignoredLines); putLong("gap", p.sequenceGap)
        })
    }

    fun detach(changingConfiguration: Boolean) {
        subscription.detach()
        dialog?.dismiss()
        if (!changingConfiguration && !choosingDestination) cancelPrepared()
    }

    private fun restore(bundle: Bundle?): DoctorPreparedExport? {
        if (bundle == null) return null
        val epoch = bundle.getLong("epoch")
        return DoctorPreparedExport(DoctorExportHandle(bundle.getLong("token"), epoch, bundle.getLong("expires")),
            DoctorExportPreview(epoch, bundle.getInt("count"), bundle.getLong("start"), bundle.getLong("end"),
                bundle.getLong("oldest").takeIf { it >= 0 }, bundle.getLong("newest").takeIf { it >= 0 },
                bundle.getLong("ignored"), bundle.getLong("gap")))
    }

    companion object {
        private const val EXPORT_REQUEST = 2
        private const val STATE = "doctor-export"
    }
}
