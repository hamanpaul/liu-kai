package com.hamanpaul.liukai.settings

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.provider.DocumentsContract
import android.text.InputType
import android.util.TypedValue
import android.view.WindowInsets
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.hamanpaul.liukai.core.table.UserPhrase
import com.hamanpaul.liukai.core.table.UserPhrases
import com.hamanpaul.liukai.data.UserPhraseStore
import com.hamanpaul.liukai.data.readAllAndClose

/**
 * 加字加詞（照官方）：自訂拆碼與字詞，工具列為 儲存、新增、編輯、刪除、上移、下移、匯入、匯出。
 * 編輯後按「儲存」才寫入；輸入法在下一次開始輸入時套用。匯入的字詞加在清單最後，匯出為 TSV（拆碼、字詞）。
 */
class UserPhrasesActivity : Activity() {
    private var phrases: List<UserPhrase> = emptyList()
    private var selected = -1
    private lateinit var list: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        phrases = UserPhraseStore.load(this)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val bar = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        // 八個工具鈕等寬排成一列（與官方相同，不需捲動）
        fun tool(label: String, onClick: () -> Unit) = bar.addView(
            Button(this).apply {
                text = label
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                minWidth = 0
                minimumWidth = 0
                setPadding(0, 0, 0, 0)
                setOnClickListener { onClick() }
            },
            LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f),
        )
        tool("儲存") {
            UserPhraseStore.save(this, phrases)
            Toast.makeText(this, "已儲存（重新開始輸入後套用）", Toast.LENGTH_SHORT).show()
        }
        tool("新增") { editDialog(null) }
        tool("編輯") { withSelection { editDialog(it) } }
        tool("刪除") { withSelection { update(phrases.filterIndexed { i, _ -> i != it }, -1) } }
        tool("上移") { withSelection { update(UserPhrases.moveUp(phrases, it), maxOf(0, it - 1)) } }
        tool("下移") { withSelection { update(UserPhrases.moveDown(phrases, it), minOf(phrases.size - 1, it + 1)) } }
        // 匯入、匯出都從內部儲存的「下載」資料夾開始
        tool("匯入") {
            startActivityForResult(
                Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .putExtra(DocumentsContract.EXTRA_INITIAL_URI, DOWNLOAD),
                IMPORT,
            )
        }
        tool("匯出") {
            startActivityForResult(
                Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("text/tab-separated-values")
                    .putExtra(Intent.EXTRA_TITLE, "liu-kai-phrases.tsv")
                    .putExtra(DocumentsContract.EXTRA_INITIAL_URI, DOWNLOAD),
                EXPORT,
            )
        }
        root.addView(bar)
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(list) })
        root.setOnApplyWindowInsetsListener { v, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(root)
        refresh()
    }

    private fun withSelection(action: (Int) -> Unit) {
        // selected 為 -1（沒有選取）或清單內的索引
        if (selected >= 0) action(selected) else Toast.makeText(this, "請先選取字詞", Toast.LENGTH_SHORT).show()
    }

    private fun update(newList: List<UserPhrase>, newSelected: Int) {
        phrases = newList
        selected = newSelected
        refresh()
    }

    private fun refresh() {
        list.removeAllViews()
        if (phrases.isEmpty()) {
            list.addView(TextView(this).apply { text = "沒有字詞資料"; setPadding(48, 48, 48, 48) })
        }
        phrases.forEachIndexed { i, p ->
            list.addView(TextView(this).apply {
                text = "${p.code}\t${p.text}"
                contentDescription = "phrase:$i"
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
                setPadding(48, 24, 48, 24)
                setBackgroundColor(if (i == selected) SELECTED else Color.TRANSPARENT)
                setOnClickListener { selected = i; refresh() }
            })
        }
    }

    /** 新增（index 為 null）或編輯字詞；拆碼不合法或沒有字詞時提示且不變更。 */
    private fun editDialog(index: Int?) {
        val current = index?.let { phrases[it] }
        val form = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(48, 16, 48, 0) }
        form.addView(TextView(this).apply { text = "拆碼" })
        // 拆碼只有英數與 , .：以「可見密碼」類型讓輸入法直接輸入英數（liu-kai 不在此欄組字）
        val codeField = EditText(this).apply {
            contentDescription = "phrase_code"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            setText(current?.code.orEmpty())
        }
        form.addView(codeField)
        form.addView(TextView(this).apply { text = "字詞" })
        val textField = EditText(this).apply { contentDescription = "phrase_text"; setText(current?.text.orEmpty()) }
        form.addView(textField)
        form.addView(TextView(this).apply { text = HELP })
        AlertDialog.Builder(this)
            .setTitle(if (index == null) "新增字詞" else "編輯字詞")
            .setView(form)
            .setNegativeButton("取消", null)
            .setPositiveButton("確定") { _, _ ->
                val c = codeField.text.toString()
                val t = textField.text.toString()
                when {
                    !UserPhrases.validCode(c) -> Toast.makeText(this, "拆碼不合法", Toast.LENGTH_SHORT).show()
                    t.isEmpty() -> Toast.makeText(this, "請輸入字詞", Toast.LENGTH_SHORT).show()
                    index == null -> update(phrases + UserPhrase(c.lowercase(), t), phrases.size)
                    else -> update(phrases.toMutableList().apply { set(index, UserPhrase(c.lowercase(), t)) }, index)
                }
            }
            .show()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        // 只發出匯入與匯出兩種請求，RESULT_OK 時必有 data
        val uri = data!!.data!!
        if (requestCode == IMPORT) {
            val message = runCatching {
                val imported = UserPhrases.parse(contentResolver.openInputStream(uri)!!.readAllAndClose().toString(Charsets.UTF_8))
                update(phrases + imported, selected)
                "已匯入 ${imported.size} 筆（按儲存後生效）"
            }.getOrElse { "匯入失敗：${it.message}" }
            Toast.makeText(this, message, Toast.LENGTH_LONG).show()
        } else {
            contentResolver.openOutputStream(uri)!!.use { it.write(UserPhrases.serialize(phrases).toByteArray()) }
            Toast.makeText(this, "已匯出 ${phrases.size} 筆", Toast.LENGTH_SHORT).show()
        }
    }

    companion object {
        private val DOWNLOAD = DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download")
        private const val IMPORT = 1
        private const val EXPORT = 2
        private val SELECTED = Color.rgb(0x80, 0xCB, 0xC4)
        private const val HELP = "拆碼可接受英文字母、數字、及「，」「.」二個符號，但第一碼只能接受使用英文字母或「，」「.」 這二個符號。"
    }
}
