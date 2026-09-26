package com.hamanpaul.liukai.testhost

import android.app.Activity
import android.os.Bundle
import android.text.InputType
import android.view.WindowInsets
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

/**
 * 端對端測試宿主：一般、密碼、多行、搜尋（imeOptions=actionSearch）四個輸入欄與一個結果標籤。
 * 欄位不設 hint（uiautomator 會把空欄位的 hint 當成文字回報）；欄位設固定 id，旋轉重建 Activity 時由框架保存並還原文字與焦點。
 */
class HostActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        fun field(desc: String, type: Int) = EditText(this).apply {
            id = root.childCount + 1
            contentDescription = desc
            inputType = type
            root.addView(this)
        }
        field("plain", InputType.TYPE_CLASS_TEXT).requestFocus()
        field("password", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
        field("multiline", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE)
        val result = TextView(this).apply { contentDescription = "result" }
        field("search", InputType.TYPE_CLASS_TEXT).apply {
            imeOptions = EditorInfo.IME_ACTION_SEARCH
            setOnEditorActionListener { v, actionId, _ ->
                result.text = "action=$actionId text=${v.text}"
                true
            }
        }
        root.addView(result)
        // targetSdk 35 強制 edge-to-edge：以系統列與輸入法的 insets 補 padding，避免輸入欄被遮住。
        root.setOnApplyWindowInsetsListener { v, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(root)
    }
}
