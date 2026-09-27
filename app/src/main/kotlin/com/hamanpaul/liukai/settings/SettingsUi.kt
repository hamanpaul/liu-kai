package com.hamanpaul.liukai.settings

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.widget.ArrayAdapter
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ListPopupWindow
import android.widget.ScrollView
import android.widget.TextView
import com.hamanpaul.liukai.data.ImePrefs

/**
 * 設定頁的共用元件（照官方「嘸蝦米鍵盤設定」的樣式：分區標題、標題＋說明＋右側勾選框、下拉選單）。
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

    /** 單選對話框項目（官方「顯示退出鍵盤鍵」）：說明列顯示目前的選項。 */
    fun choiceDialog(title: String, key: String, labels: List<String>, values: List<String>, current: String) {
        var selected = values.indexOf(current)
        val row = row(title, labels[selected], null)
        val summary = (row.getChildAt(0) as LinearLayout).getChildAt(1) as TextView
        row.setOnClickListener {
            AlertDialog.Builder(activity)
                .setTitle(title)
                .setSingleChoiceItems(labels.toTypedArray(), selected) { dialog, which ->
                    selected = which
                    ImePrefs.set(activity, key, values[which])
                    summary.text = labels[which]
                    dialog.dismiss()
                }
                .setNegativeButton("取消", null)
                .show()
        }
    }

    /**
     * 下拉選單列（官方「設定主題及配置」）：標籤與下拉值並排，一列可放兩組；點下拉值展開選項清單。
     * 以 TextView＋ListPopupWindow 呈現（外觀同下拉選單，只有選取一種事件）。
     */
    fun spinners(vararg items: SpinnerItem) {
        val line = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16f), dp(4f), dp(8f), dp(4f))
        }
        for (item in items) {
            line.addView(TextView(activity).apply { text = item.title; setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f) },
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            val value = TextView(activity).apply {
                text = item.labels[item.values.indexOf(item.current)] + " ▾"
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
                setPadding(dp(8f), dp(12f), dp(8f), dp(12f))
                contentDescription = "dropdown:${item.key}"
            }
            value.setOnClickListener {
                val popup = ListPopupWindow(activity)
                // modal：下拉清單成為作用中的視窗（與系統下拉選單相同），點清單外只收起
                popup.isModal = true
                popup.anchorView = value
                popup.setAdapter(ArrayAdapter(activity, android.R.layout.simple_list_item_1, item.labels))
                popup.setOnItemClickListener { _, _, position, _ ->
                    ImePrefs.set(activity, item.key, item.values[position])
                    value.text = item.labels[position] + " ▾"
                    popup.dismiss()
                }
                popup.show()
            }
            line.addView(value, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }
        root.addView(line)
    }

    class SpinnerItem(val title: String, val key: String, val labels: List<String>, val values: List<String>, val current: String)

    companion object {
        private val SECTION = Color.rgb(0x00, 0x89, 0x7B)
    }
}
