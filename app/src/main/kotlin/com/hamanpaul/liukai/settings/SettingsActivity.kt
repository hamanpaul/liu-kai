package com.hamanpaul.liukai.settings

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.provider.Settings
import android.util.TypedValue
import android.os.Build
import android.view.WindowInsets
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.TableBundle
import com.hamanpaul.liukai.data.TableStore
import java.io.FileNotFoundException
import java.util.Optional
import java.util.concurrent.Executors

/** 設定頁：啟用引導、匯入／清除字表、目前字表資訊與試打區。 */
class SettingsActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var report: TextView
    private val io = Executors.newSingleThreadExecutor()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        fun button(label: String, onClick: () -> Unit) = Button(this).apply {
            text = label
            setOnClickListener { onClick() }
            root.addView(this)
        }
        status = TextView(this).apply { setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f) }
        root.addView(status)
        button("1. 在系統設定啟用 liu-kai") { startActivity(Intent(Settings.ACTION_INPUT_METHOD_SETTINGS)) }
        button("2. 切換輸入法") { getSystemService(InputMethodManager::class.java).showInputMethodPicker() }
        button("3. 匯入字表（可多選：liu_ibus_final.txt + lime_liu7.txt）") { pickFiles() }
        button("清除字表") {
            TableStore.clear(this)
            report.text = "已清除字表"
            refresh()
        }
        report = TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextIsSelectable(true)
        }
        root.addView(report)
        root.addView(TextView(this).apply { text = "試打區" })
        root.addView(EditText(this).apply { hint = "在這裡測試輸入"; minLines = 3 })
        val scroll = ScrollView(this).apply { addView(root) }
        // targetSdk 35 強制 edge-to-edge：以系統列與輸入法的 insets 補 padding。
        scroll.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        setContentView(scroll)
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    /** 結束背景執行緒，避免 Activity 重建（例如旋轉）時累積。 */
    override fun onDestroy() {
        io.shutdown()
        super.onDestroy()
    }

    private fun refresh() {
        val imm = getSystemService(InputMethodManager::class.java)
        val enabled = imm.enabledInputMethodList.any { it.packageName == packageName }
        val selected = if (Build.VERSION.SDK_INT >= 34) {
            imm.currentInputMethodInfo?.packageName == packageName
        } else {
            Settings.Secure.getString(contentResolver, Settings.Secure.DEFAULT_INPUT_METHOD).orEmpty().startsWith("$packageName/")
        }
        io.execute {
            val loaded = runCatching { TableStore.load(this) }.getOrNull()
            runOnUiThread {
                status.text = buildString {
                    append("輸入法：").append(if (enabled) "已啟用" else "未啟用")
                    append("／").append(if (selected) "使用中" else "未切換").append('\n')
                    append("字表：").append(if (loaded != null) "已匯入" else "尚未匯入")
                }
                if (loaded != null && report.text.isNullOrEmpty()) report.text = describe(loaded.bundle)
            }
        }
    }

    private fun pickFiles() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(intent, REQUEST_PICK)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PICK || resultCode != RESULT_OK || data == null) return
        val uris = buildList {
            data.clipData?.let { clip -> for (i in 0 until clip.itemCount) add(clip.getItemAt(i).uri) }
            if (isEmpty()) data.data?.let { add(it) }
        }
        report.text = "匯入中…"
        io.execute {
            val message = runCatching {
                val files = uris.map {
                    val name = displayName(it)
                    // 文件提供者暫時無法開啟時回傳 null：以明確訊息失敗（顯示「匯入失敗：無法開啟檔案：…」）
                    val stream = Optional.ofNullable(contentResolver.openInputStream(it)).orElseThrow { FileNotFoundException("無法開啟檔案：$name") }
                    NamedBytes(name, stream.use { s -> s.readBytes() })
                }
                describe(TableStore.import(this, files))
            }.getOrElse { "匯入失敗：${it.message}" }
            runOnUiThread {
                report.text = message
                refresh()
            }
        }
    }

    private fun displayName(uri: Uri): String =
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        } ?: uri.lastPathSegment.orEmpty()

    companion object {
        private const val REQUEST_PICK = 1

        fun describe(result: ImportResult): String = buildString {
            append("匯入成功\n")
            append(describe(result.bundle))
            append("\n全部區段：\n")
            result.allSections.forEachIndexed { i, s ->
                append("  [$i] ${s.kind} ${s.name ?: ""} 原始 ${s.rawRows} 列／去重 ${s.uniquePairs} 組\n")
            }
        }

        fun describe(bundle: TableBundle): String = buildString {
            append("來源：\n")
            bundle.sources.forEach { append("  ${it.name}\n  sha256 ${it.sha256.take(16)}…\n") }
            append("載入區段：\n")
            bundle.stats.forEach {
                append("  ${it.kind}：${it.uniquePairs} 組、${it.uniqueCodes} 碼、${it.uniqueTexts} 字（原始 ${it.rawRows} 列）\n")
            }
        }
    }
}
