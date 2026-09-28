package com.hamanpaul.liukai.settings

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.provider.Settings
import android.util.TypedValue
import android.view.WindowInsets
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.TableBundle
import com.hamanpaul.liukai.data.TableStore
import com.hamanpaul.liukai.data.readAllAndClose
import java.util.concurrent.Executors

/** 設定頁：啟用引導、嘸蝦米鍵盤設定、匯入／清除字表、目前字表資訊與試打區。 */
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
        button("加字加詞") { startActivity(Intent(this, UserPhrasesActivity::class.java)) }
        button("1. 在系統設定啟用 liu-kai") { startActivity(Intent(Settings.ACTION_INPUT_METHOD_SETTINGS)) }
        button("2. 切換輸入法") { getSystemService(InputMethodManager::class.java).showInputMethodPicker() }
        button("嘸蝦米鍵盤設定") { startActivity(Intent(this, KeyboardSettingsActivity::class.java)) }
        button("3. 匯入字表（可多選：liu_ibus_final.txt + lime_liu7.txt）") { pickFiles() }
        button("清除字表") {
            TableStore.clear(this)
            report.text = "已清除匯入的字表"
            refresh()
        }
        report = TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextIsSelectable(true)
        }
        root.addView(report)
        root.addView(TextView(this).apply { text = "試打區" })
        root.addView(EditText(this).apply { minLines = 3; contentDescription = "try_area" })
        val scroll = ScrollView(this).apply { addView(root) }
        // targetSdk 35 強制 edge-to-edge：以系統列與輸入法的 insets 補 padding。padding 加在外層容器而不是 ScrollView：
        // ScrollView 判斷焦點欄位與游標是否可見時不扣自己的 padding，鍵盤較高時試打區會被蓋住也不捲動；
        // 讓 ScrollView 實際縮小，才會把焦點欄位捲到鍵盤上方。
        val frame = FrameLayout(this).apply { addView(scroll) }
        frame.setOnApplyWindowInsetsListener { v, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(frame)
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        val imm = getSystemService(InputMethodManager::class.java)
        val enabled = imm.enabledInputMethodList.any { it.packageName == packageName }
        val current = Settings.Secure.getString(contentResolver, Settings.Secure.DEFAULT_INPUT_METHOD)
        val selected = current.startsWith("$packageName/")
        io.execute {
            val loaded = runCatching { TableStore.load(this) }
            runOnUiThread {
                status.text = "輸入法：" + (if (enabled) "已啟用" else "未啟用") + "／" + (if (selected) "使用中" else "未切換") +
                    "\n字表：" + loaded.fold(
                        { if (it == null) "尚未匯入" else if (it.bundled) "內建" else "已匯入" },
                        { "損毀（${it.message}），請重新匯入" },
                    )
                loaded.getOrNull()?.let { if (report.text.isEmpty()) report.text = describe(it.bundle) }
            }
        }
    }

    private fun pickFiles() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            // 字表檔通常下載到內部儲存的 Download 資料夾，直接從那裡開始選
            putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE, "primary:Download"))
        }
        startActivityForResult(intent, 1)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        // 本頁只發出一種請求（選檔），RESULT_OK 時必有 data；多選時在 clipData，單選時在 data。
        val clip = data!!.clipData
        val uris = if (clip != null) List(clip.itemCount) { clip.getItemAt(it).uri } else listOf(data.data!!)
        report.text = "匯入中…"
        io.execute {
            val message = runCatching {
                val files = uris.map { NamedBytes(displayName(it), contentResolver.openInputStream(it)!!.readAllAndClose()) }
                describe(TableStore.import(this, files))
            }.getOrElse { "匯入失敗：${it.message}" }
            runOnUiThread {
                report.text = message
                refresh()
            }
        }
    }

    /** SAF 文件必有 DISPLAY_NAME 欄位。 */
    private fun displayName(uri: Uri): String {
        val cursor = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)!!
        cursor.moveToFirst()
        val name = cursor.getString(0)
        cursor.close()
        return name
    }

    companion object {
        private const val EXTERNAL_STORAGE = "com.android.externalstorage.documents"

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
