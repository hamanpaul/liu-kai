package com.hamanpaul.liukai.testhost

import android.app.Activity
import android.os.Bundle
import android.text.InputType
import android.os.Build
import android.view.WindowInsets
import android.widget.EditText
import android.widget.LinearLayout

/** E2E 宿主：一般輸入欄、密碼欄、多行欄，供 UiAutomator 驅動 liu-kai 輸入法。 */
class HostActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        fun field(desc: String, type: Int) = EditText(this).apply {
            contentDescription = desc
            inputType = type
            hint = desc
            root.addView(this)
        }
        field("plain", InputType.TYPE_CLASS_TEXT).requestFocus()
        field("password", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
        field("multiline", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE)
        // targetSdk 35 強制 edge-to-edge：以系統列與輸入法的 insets 補 padding，避免輸入欄被遮住。
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        setContentView(root)
    }
}
