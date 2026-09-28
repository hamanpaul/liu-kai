package com.hamanpaul.liukai.ime

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.RelativeSizeSpan
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import com.hamanpaul.liukai.core.engine.Candidate
import com.hamanpaul.liukai.core.engine.InputMode

/** 軟鍵盤送出的按鍵。 */
sealed interface SoftKey {
    data class Text(val char: Char) : SoftKey
    data object Backspace : SoftKey
    data object Space : SoftKey
    data object Enter : SoftKey
    data object ToggleEnglish : SoftKey
    data object ToggleJapanese : SoftKey
}

interface ImeActions {
    fun onSoftKey(key: SoftKey)
    fun onCandidateTap(index: Int)
    fun onCandidateLongPress(index: Int)
}

/** 畫面需要的引擎狀態快照。 */
data class UiState(
    val mode: InputMode,
    val composing: String,
    val candidates: List<Candidate>,
    val pageStart: Int,
    val pageSize: Int,
    val homophoneOf: String?,
    val tableLoaded: Boolean,
)

/**
 * 輸入畫面：上方候選列、下方軟鍵盤。偵測到實體鍵盤時只顯示候選列。
 * 候選與按鍵都是獨立的標準 View（含 contentDescription），確保點擊判定可靠、也可被 UiAutomator 定位。
 */
@SuppressLint("ViewConstructor")
class ImeView(context: Context, private val actions: ImeActions) : LinearLayout(context) {
    private val density = resources.displayMetrics.density
    private fun dp(v: Float) = (v * density).toInt()

    private val badge = TextView(context)
    private val composingView = TextView(context)
    private val candidateRow = LinearLayout(context)
    private val candidateScroll = HorizontalScrollView(context)
    private val keyboard = LinearLayout(context)

    private var shifted = false
    private var symbols = false
    private var mode: InputMode = InputMode.CHINESE
    private val repeatHandler = Handler(Looper.getMainLooper())

    init {
        orientation = VERTICAL
        setBackgroundColor(BG)
        addView(buildCandidateBar(), LayoutParams(LayoutParams.MATCH_PARENT, dp(52f)))
        keyboard.orientation = VERTICAL
        keyboard.setPadding(dp(2f), dp(2f), dp(2f), dp(4f))
        addView(keyboard, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        rebuildKeyboard()
        // 輸入法視窗在手勢導覽下會延伸到導覽列底下：以導覽列 inset 補底部 padding，避免最下排按鍵被遮住。
        setOnApplyWindowInsetsListener { v, insets ->
            val bottom = if (Build.VERSION.SDK_INT >= 30) {
                insets.getInsets(WindowInsets.Type.navigationBars()).bottom
            } else {
                @Suppress("DEPRECATION")
                insets.systemWindowInsetBottom
            }
            v.setPadding(0, 0, 0, bottom)
            insets
        }
    }

    fun setKeyboardVisible(visible: Boolean) {
        keyboard.visibility = if (visible) VISIBLE else GONE
    }

    fun render(state: UiState) {
        if (state.mode != mode) {
            mode = state.mode
            rebuildKeyboard()
        }
        badge.text = when {
            !state.tableLoaded -> "無表"
            state.mode == InputMode.ENGLISH -> "英"
            state.mode == InputMode.JAPANESE -> "日"
            else -> "中"
        }
        composingView.text = when {
            state.homophoneOf != null -> "音:${state.homophoneOf}"
            else -> state.composing
        }
        candidateRow.removeAllViews()
        state.candidates.forEachIndexed { index, cand ->
            candidateRow.addView(candidateView(index, cand, state))
        }
        candidateScroll.post { candidateScroll.scrollTo(0, 0) }
    }

    private fun buildCandidateBar(): View {
        val bar = LinearLayout(context)
        bar.orientation = HORIZONTAL
        bar.gravity = Gravity.CENTER_VERTICAL
        bar.setBackgroundColor(BAR_BG)
        badge.apply {
            setTextColor(ACCENT)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            gravity = Gravity.CENTER
            contentDescription = "mode"
        }
        bar.addView(badge, LayoutParams(dp(40f), LayoutParams.MATCH_PARENT))
        composingView.apply {
            setTextColor(Color.LTGRAY)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(2f), 0, dp(8f), 0)
            contentDescription = "composing"
        }
        bar.addView(composingView, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
        candidateRow.orientation = HORIZONTAL
        candidateRow.gravity = Gravity.CENTER_VERTICAL
        candidateScroll.isHorizontalScrollBarEnabled = false
        candidateScroll.contentDescription = "candidates"
        candidateScroll.addView(candidateRow, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
        bar.addView(candidateScroll, LayoutParams(0, LayoutParams.MATCH_PARENT, 1f))
        return bar
    }

    private fun candidateView(index: Int, cand: Candidate, state: UiState): View {
        val tv = TextView(context)
        val text = SpannableStringBuilder()
        val pageOffset = index - state.pageStart
        if (pageOffset in 0 until state.pageSize) {
            val label = if (pageOffset == 9) "0" else "${pageOffset + 1}"
            text.append(label, ForegroundColorSpan(Color.GRAY), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text.setSpan(RelativeSizeSpan(0.55f), 0, label.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        text.append(cand.text)
        cand.annotation?.let {
            val start = text.length
            text.append(" ").append(it)
            text.setSpan(RelativeSizeSpan(0.6f), start, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text.setSpan(ForegroundColorSpan(ACCENT), start, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        tv.text = text
        tv.setTextColor(Color.WHITE)
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
        tv.gravity = Gravity.CENTER
        tv.setPadding(dp(10f), 0, dp(10f), 0)
        tv.minWidth = dp(44f)
        tv.contentDescription = "cand:$index:${cand.text}"
        tv.isClickable = true
        tv.isLongClickable = true
        tv.setOnClickListener { actions.onCandidateTap(index) }
        tv.setOnLongClickListener {
            actions.onCandidateLongPress(index)
            true
        }
        tv.background = keyBackground(pressedOnly = true)
        return tv
    }

    private fun rebuildKeyboard() {
        keyboard.removeAllViews()
        val rows = if (symbols) SYMBOL_ROWS else LETTER_ROWS
        rows.forEach { row -> keyboard.addView(buildRow(row), LayoutParams(LayoutParams.MATCH_PARENT, dp(50f))) }
    }

    private fun buildRow(keys: List<KeyDef>): View {
        val row = LinearLayout(context)
        row.orientation = HORIZONTAL
        keys.forEach { def ->
            val key = TextView(context)
            key.text = labelFor(def)
            key.setTextColor(Color.WHITE)
            key.setTextSize(TypedValue.COMPLEX_UNIT_SP, if (def.label.length > 1) 15f else 20f)
            key.gravity = Gravity.CENTER
            key.contentDescription = "key:${def.id}"
            key.background = keyBackground(special = def.special)
            key.isClickable = true
            if (def.id == "backspace") attachRepeat(key) else key.setOnClickListener { onKey(def) }
            val lp = LayoutParams(0, LayoutParams.MATCH_PARENT, def.weight)
            lp.setMargins(dp(2f), dp(2f), dp(2f), dp(2f))
            row.addView(key, lp)
        }
        return row
    }

    private fun labelFor(def: KeyDef): String = when (def.id) {
        "toggle_english" -> if (mode == InputMode.ENGLISH) "英" else "中"
        "toggle_japanese" -> if (mode == InputMode.JAPANESE) "日✓" else "日"
        "shift" -> if (shifted) "⇪" else "⇧"
        else -> if (shifted && def.label.length == 1 && def.label[0].isLetter()) def.label.uppercase() else def.label
    }

    private fun onKey(def: KeyDef) {
        when (def.id) {
            "shift" -> { shifted = !shifted; rebuildKeyboard() }
            "symbols" -> { symbols = !symbols; rebuildKeyboard() }
            "space" -> actions.onSoftKey(SoftKey.Space)
            "enter" -> actions.onSoftKey(SoftKey.Enter)
            "toggle_english" -> actions.onSoftKey(SoftKey.ToggleEnglish)
            "toggle_japanese" -> actions.onSoftKey(SoftKey.ToggleJapanese)
            else -> {
                var c = def.label[0]
                if (shifted && c.isLetter()) {
                    c = c.uppercaseChar()
                    shifted = false
                    rebuildKeyboard()
                }
                actions.onSoftKey(SoftKey.Text(c))
            }
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun attachRepeat(key: View) {
        val repeat = object : Runnable {
            override fun run() {
                actions.onSoftKey(SoftKey.Backspace)
                repeatHandler.postDelayed(this, 60)
            }
        }
        key.setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.isPressed = true
                    actions.onSoftKey(SoftKey.Backspace)
                    repeatHandler.postDelayed(repeat, 400)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    v.isPressed = false
                    repeatHandler.removeCallbacks(repeat)
                }
            }
            true
        }
    }

    private fun keyBackground(special: Boolean = false, pressedOnly: Boolean = false): android.graphics.drawable.Drawable {
        val normal = GradientDrawable().apply {
            cornerRadius = dp(6f).toFloat()
            setColor(if (pressedOnly) Color.TRANSPARENT else if (special) KEY_SPECIAL else KEY_BG)
        }
        val pressed = GradientDrawable().apply {
            cornerRadius = dp(6f).toFloat()
            setColor(KEY_PRESSED)
        }
        return android.graphics.drawable.StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed), pressed)
            addState(intArrayOf(), normal)
        }
    }

    private data class KeyDef(val id: String, val label: String, val weight: Float = 1f, val special: Boolean = false)

    companion object {
        private val BG = Color.rgb(0x1C, 0x1C, 0x1E)
        private val BAR_BG = Color.rgb(0x26, 0x26, 0x29)
        private val KEY_BG = Color.rgb(0x3A, 0x3A, 0x3D)
        private val KEY_SPECIAL = Color.rgb(0x2C, 0x2C, 0x2F)
        private val KEY_PRESSED = Color.rgb(0x5A, 0x5A, 0x60)
        private val ACCENT = Color.rgb(0x7F, 0xC8, 0xF8)

        private fun chars(s: String) = s.map { KeyDef(it.toString(), it.toString()) }

        private val LETTER_ROWS = listOf(
            chars("qwertyuiop[]"),
            chars("asdfghjkl;'"),
            listOf(KeyDef("shift", "⇧", 1.4f, true)) + chars("zxcvbnm,.") + KeyDef("backspace", "⌫", 1.4f, true),
            listOf(
                KeyDef("symbols", "符", 1.2f, true),
                KeyDef("toggle_english", "中", 1.2f, true),
                KeyDef("toggle_japanese", "日", 1.2f, true),
                KeyDef("?", "?"),
                KeyDef("*", "*"),
                KeyDef("space", "空白", 4f),
                KeyDef("enter", "↵", 1.6f, true),
            ),
        )

        private val SYMBOL_ROWS = listOf(
            chars("1234567890"),
            chars("@#$%&-+()/"),
            chars("!\":_=`~<>") + KeyDef("backspace", "⌫", 1.4f, true),
            listOf(
                KeyDef("symbols", "ABC", 1.2f, true),
                KeyDef("toggle_english", "中", 1.2f, true),
                KeyDef(",", ","),
                KeyDef(".", "."),
                KeyDef("space", "空白", 4f),
                KeyDef("enter", "↵", 1.6f, true),
            ),
        )
    }
}
