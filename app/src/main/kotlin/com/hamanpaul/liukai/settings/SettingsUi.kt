package com.hamanpaul.liukai.settings

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.hamanpaul.liukai.data.ImePrefs

/**
 * 設定頁的共用元件（照官方「嘸蝦米鍵盤設定」的樣式：分區標題、標題＋說明＋右側勾選框、單選對話框）。
 * 值一律寫入 [ImePrefs]；輸入法在下一次顯示鍵盤時讀取。
 */
class SettingsUi(private val activity: Activity) {
    private val density = activity.resources.displayMetrics.density
    private fun dp(v: Float) = (v * density).toInt()
    val root = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

    /** 把 root 放進可捲動、依系統列 inset 補 padding 的容器，設為 Activity 內容。 */
    fun show() {
        val scroll = ScrollView(activity).apply { addView(root) }
        scroll.setOnApplyWindowInsetsListener { v, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        activity.setContentView(scroll)
    }

    fun section(title: String) {
        root.addView(TextView(activity).apply {
            text = title
            setTextColor(SECTION)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            setPadding(dp(16f), dp(18f), dp(16f), dp(6f))
        })
    }

    private fun row(title: String, summary: String?, trailing: View?): LinearLayout {
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16f), dp(12f), dp(12f), dp(12f))
            isClickable = true
            minimumHeight = dp(64f)
        }
        val texts = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(TextView(activity).apply { text = title; setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f) })
        if (summary != null) {
            texts.addView(TextView(activity).apply { text = summary; setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f); alpha = 0.7f })
        }
        row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        if (trailing != null) row.addView(trailing)
        root.addView(row)
        return row
    }

    /** 點選即開啟另一頁的項目。 */
    fun link(title: String, summary: String?, onClick: () -> Unit) {
        row(title, summary, null).setOnClickListener { onClick() }
    }

    /** 勾選項目：點整列切換並寫入偏好。 */
    fun check(title: String, summary: String?, key: String, value: Boolean) {
        val box = CheckBox(activity).apply { isChecked = value; isClickable = false }
        row(title, summary, box).setOnClickListener {
            box.isChecked = !box.isChecked
            ImePrefs.set(activity, key, box.isChecked.toString())
        }
    }

    /** 不可點的資訊項目（官方「關於」）。 */
    fun info(title: String, summary: String?) {
        row(title, summary, null).isClickable = false
    }

    /**
     * 單選對話框項目（官方「顯示退出鍵盤鍵」「直式鍵盤高度」等）：說明列顯示目前的選項；
     * summaries 為各選項對應的說明文字（官方「語音輸入」），省略時顯示選項本身。
     */
    fun choiceDialog(
        title: String, key: String, labels: List<String>, values: List<String>, current: String,
        summaries: List<String> = labels,
    ) {
        var selected = values.indexOf(current)
        val row = row(title, summaries[selected], null)
        val summary = (row.getChildAt(0) as LinearLayout).getChildAt(1) as TextView
        row.setOnClickListener {
            AlertDialog.Builder(activity)
                .setTitle(title)
                .setSingleChoiceItems(labels.toTypedArray(), selected) { dialog, which ->
                    selected = which
                    ImePrefs.set(activity, key, values[which])
                    summary.text = summaries[which]
                    dialog.dismiss()
                }
                .setNegativeButton("取消", null)
                .show()
        }
    }

    companion object {
        private val SECTION = Color.rgb(0x00, 0x89, 0x7B)
    }
}
