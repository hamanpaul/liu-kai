package com.hamanpaul.liukai.ime

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.InsetDrawable
import android.graphics.drawable.StateListDrawable
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
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import com.hamanpaul.liukai.core.engine.Candidate
import com.hamanpaul.liukai.core.engine.InputMode
import com.hamanpaul.liukai.core.ime.EnterAction
import com.hamanpaul.liukai.core.ime.SoftKey
import org.json.JSONArray
import org.json.JSONObject

interface ImeActions {
    fun onSoftKey(key: SoftKey)
    fun onCandidateTap(index: Int)
    fun onOpenSettings()
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
    /** 組字失敗：整個輸入畫面加紅框，直到下一次按鍵。 */
    val failed: Boolean,
)

/**
 * 輸入畫面：上方候選列、下方螢幕鍵盤。偵測到實體鍵盤時只顯示候選列。
 *
 * 螢幕鍵盤照官方嘸蝦米 PRO 的配置：字母層（q–p 附數字提示、長按輸入數字；a–l；⇧ z–m ⌫；
 * 中 ?123 , 空白 . Enter）、?123 層與 ALT 層；長按「.」彈出標點、長按「,」彈出設定。
 * 字根 ' [ ]、萬用字元 *、同音鍵 ` 與官方相同放在 ?123／ALT 層，切換鍵盤層不影響組字。
 * 候選與按鍵都是獨立的標準 View（含 contentDescription），確保點擊判定可靠、也可被 UiAutomator 定位。
 */
@SuppressLint("ViewConstructor")
class ImeView(context: Context, private val actions: ImeActions) : LinearLayout(context) {
    private val density = resources.displayMetrics.density
    private fun dp(v: Float) = (v * density).toInt()

    private enum class Layer { LETTERS, SYMBOLS, ALT }

    private val composingView = TextView(context)
    private val candidateRow = LinearLayout(context)
    private val candidateScroll = HorizontalScrollView(context)
    private val keyboard = FrameLayout(context)
    private val rowsView = LinearLayout(context)
    /** 彈出鍵盤顯示時蓋住整個鍵盤區：點彈出區以外只收起彈出鍵盤。 */
    private val popupCatcher = View(context)
    private val popupPanel = LinearLayout(context)

    private var layer = Layer.LETTERS
    private var shifted = false
    private var mode: InputMode = InputMode.CHINESE
    private var tableLoaded = true
    private var enterLabel = "↵"
    private var shownCandidates: List<Candidate> = emptyList()
    private val keyViews = LinkedHashMap<String, View>()
    private val popupViews = LinkedHashMap<String, View>()
    private val repeatHandler = Handler(Looper.getMainLooper())
    private val failureFrame = GradientDrawable().apply {
        setColor(Color.TRANSPARENT)
        setStroke(dp(3f), FAILURE)
    }

    init {
        orientation = VERTICAL
        setBackgroundColor(BG)
        addView(buildCandidateBar(), LayoutParams(LayoutParams.MATCH_PARENT, dp(48f)))
        rowsView.orientation = VERTICAL
        rowsView.setPadding(dp(2f), dp(4f), dp(2f), dp(4f))
        keyboard.addView(rowsView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        popupCatcher.visibility = GONE
        popupCatcher.contentDescription = "popup_catcher"
        popupCatcher.setOnClickListener { dismissPopup() }
        keyboard.addView(popupCatcher, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 0))
        popupPanel.orientation = VERTICAL
        popupPanel.visibility = GONE
        popupPanel.elevation = dp(6f).toFloat()
        popupPanel.background = GradientDrawable().apply {
            cornerRadius = dp(4f).toFloat()
            setColor(Color.WHITE)
        }
        keyboard.addView(popupPanel, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        addView(keyboard, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        rebuildKeyboard()
        // 輸入法視窗在手勢導覽下會延伸到導覽列底下：以導覽列高度補底部 padding，避免最下排按鍵被遮住。
        // 視窗剛附加時第一次 inset 通知的導覽列高度是 0（隨後才是實際值），若照單全收，視窗會以沒有
        // padding 的高度排版，直到下一次重新排版才長高、鍵盤跳動；因此取視窗尺寸推算的導覽列高度為下限。
        setPadding(0, 0, 0, navigationBarHeight())
        setOnApplyWindowInsetsListener { v, insets ->
            v.setPadding(0, 0, 0, maxOf(insets.getInsets(WindowInsets.Type.navigationBars()).bottom, navigationBarHeight()))
            insets
        }
    }

    private fun navigationBarHeight(): Int =
        context.getSystemService(WindowManager::class.java).currentWindowMetrics.windowInsets
            .getInsets(WindowInsets.Type.navigationBars()).bottom

    /** 回到字母層、放開 Shift、收起彈出鍵盤（換到新的輸入欄時呼叫）。 */
    fun resetLayout() {
        layer = Layer.LETTERS
        shifted = false
        dismissPopup()
        rebuildKeyboard()
    }

    /** Enter 鍵標籤依輸入欄的動作（Next、Search…）；會換行的欄位顯示 ↵。 */
    fun setEnterAction(action: EnterAction) {
        enterLabel = when (action) {
            is EnterAction.EditorAction -> ACTION_LABELS.getOrDefault(action.actionId, "↵")
            EnterAction.NewLine -> "↵"
        }
        rebuildKeyboard()
    }

    fun setKeyboardVisible(visible: Boolean) {
        keyboard.visibility = if (visible) VISIBLE else GONE
    }

    fun render(state: UiState) {
        foreground = if (state.failed) failureFrame else null
        if (state.mode != mode || state.tableLoaded != tableLoaded) {
            mode = state.mode
            tableLoaded = state.tableLoaded
            rebuildKeyboard()
        }
        composingView.text = when {
            state.homophoneOf != null -> "音:${state.homophoneOf}"
            else -> state.composing
        }
        candidateRow.removeAllViews()
        shownCandidates = state.candidates
        state.candidates.forEachIndexed { index, cand ->
            candidateRow.addView(candidateView(index, cand, state))
        }
        candidateScroll.post { candidateScroll.scrollTo(0, 0) }
    }

    /**
     * 診斷輸出：候選列、各候選與各按鍵的螢幕座標、目前鍵盤層與各排按鍵、Enter 標籤、彈出鍵盤（供 dump() 與端對端測試使用）。
     * 輸入法視窗貼齊螢幕底部，以「螢幕底 − 視窗高 + 視窗內座標」換算；不用 getLocationOnScreen，
     * 因為視窗進場動畫由系統移動、不觸發重新排版，快取的視窗位置在下次排版前都是舊值。
     */
    fun describe(json: JSONObject) {
        windowTop = context.getSystemService(WindowManager::class.java).currentWindowMetrics.bounds.bottom - rootView.height
        json.put("keyboardVisible", keyboard.visibility == VISIBLE)
        json.put("failureHint", foreground != null)
        json.put("candidateRow", bounds(candidateScroll))
        val candidates = JSONArray()
        for (i in 0 until candidateRow.childCount) {
            val child = candidateRow.getChildAt(i)
            val cand = shownCandidates[i]
            candidates.put(
                bounds(child).put("index", i).put("text", cand.text).put("annotation", cand.annotation ?: JSONObject.NULL),
            )
        }
        json.put("candidates", candidates)
        val keys = JSONObject()
        for ((id, view) in keyViews) keys.put(id, bounds(view))
        for ((id, view) in popupViews) keys.put(id, bounds(view))
        json.put("keys", keys)
        json.put("layer", layer.name.lowercase())
        json.put("rows", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { it.id }) }))
        json.put("enterLabel", enterLabel)
        json.put("popup", JSONArray(popupViews.keys.toList()))
    }

    private var windowTop = 0

    private fun bounds(v: View): JSONObject {
        val loc = IntArray(2)
        v.getLocationInWindow(loc)
        return JSONObject().put("x", loc[0]).put("y", windowTop + loc[1]).put("w", v.width).put("h", v.height)
    }

    private fun buildCandidateBar(): View {
        val bar = LinearLayout(context)
        bar.orientation = HORIZONTAL
        bar.gravity = Gravity.CENTER_VERTICAL
        bar.setBackgroundColor(BAR_BG)
        composingView.apply {
            setTextColor(HINT)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10f), 0, dp(8f), 0)
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
            // 選字鍵 0–9：0 為預設字（空白上屏的字）
            val label = "$pageOffset"
            text.append(label, ForegroundColorSpan(HINT), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
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
        tv.setTextColor(KEY_TEXT)
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
        tv.gravity = Gravity.CENTER
        tv.setPadding(dp(10f), 0, dp(10f), 0)
        tv.minWidth = dp(44f)
        tv.contentDescription = "cand:$index:${cand.text}"
        tv.isClickable = true
        tv.setOnClickListener { actions.onCandidateTap(index) }
        tv.background = pressedBackground()
        return tv
    }

    private fun rebuildKeyboard() {
        rowsView.removeAllViews()
        keyViews.clear()
        rowsFor(layer).forEach { row -> rowsView.addView(buildRow(row), LayoutParams(LayoutParams.MATCH_PARENT, dp(54f))) }
    }

    private fun rowsFor(layer: Layer) = when (layer) {
        Layer.LETTERS -> LETTER_ROWS
        Layer.SYMBOLS -> SYMBOL_ROWS
        Layer.ALT -> ALT_ROWS
    }

    private fun buildRow(keys: List<KeyDef>): View {
        val row = LinearLayout(context)
        row.orientation = HORIZONTAL
        row.gravity = Gravity.CENTER_HORIZONTAL
        row.weightSum = ROW_WEIGHT
        keys.forEach { def ->
            val key = buildKey(def)
            keyViews[def.id] = key
            val lp = LayoutParams(0, LayoutParams.MATCH_PARENT, def.weight)
            lp.setMargins(dp(1f), dp(2f), dp(1f), dp(2f))
            row.addView(key, lp)
        }
        return row
    }

    private fun buildKey(def: KeyDef): View {
        val frame = FrameLayout(context)
        frame.contentDescription = "key:${def.id}"
        frame.isClickable = true
        val label = TextView(context)
        label.text = labelFor(def)
        label.gravity = Gravity.CENTER
        label.setTextColor(if (def.id == "enter") Color.WHITE else if (activeFor(def)) ACCENT else KEY_TEXT)
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, if (label.text.length > 1) 15f else 22f)
        frame.addView(label, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        def.hint?.let { hint ->
            frame.addView(
                TextView(context).apply {
                    text = hint.toString()
                    setTextColor(HINT)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
                    setPadding(0, dp(2f), dp(4f), 0)
                },
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END),
            )
            // 長按輸入數字（組字中即為選字鍵）
            frame.setOnLongClickListener {
                actions.onSoftKey(SoftKey.Text(hint))
                true
            }
        }
        def.popup?.let { popup ->
            frame.setOnLongClickListener {
                showPopup(frame, popup)
                true
            }
        }
        frame.background = when (def.id) {
            "enter" -> actionBackground()
            "space" -> spaceBackground()
            else -> pressedBackground()
        }
        if (def.id == "backspace") attachRepeat(frame) else frame.setOnClickListener { onKey(def) }
        return frame
    }

    /** Shift 按下、ALT 層時以強調色標示。 */
    private fun activeFor(def: KeyDef): Boolean = when (def.id) {
        "shift" -> shifted
        "alt" -> layer == Layer.ALT
        else -> false
    }

    private fun labelFor(def: KeyDef): String = when (def.id) {
        "toggle_english" -> if (!tableLoaded) "無表" else if (mode == InputMode.ENGLISH) "英" else "中"
        "symbols" -> if (layer == Layer.LETTERS) "?123" else "ABC"
        "enter" -> enterLabel
        else -> if (shifted && def.label[0].isLetter()) def.label.uppercase() else def.label
    }

    private fun onKey(def: KeyDef) {
        when (def.id) {
            "shift" -> { shifted = !shifted; rebuildKeyboard() }
            "symbols" -> { layer = if (layer == Layer.LETTERS) Layer.SYMBOLS else Layer.LETTERS; shifted = false; rebuildKeyboard() }
            "alt" -> { layer = if (layer == Layer.ALT) Layer.SYMBOLS else Layer.ALT; rebuildKeyboard() }
            "space" -> actions.onSoftKey(SoftKey.Space)
            "enter" -> actions.onSoftKey(SoftKey.Enter)
            "toggle_english" -> actions.onSoftKey(SoftKey.ToggleEnglish)
            else -> {
                var c = def.char
                if (shifted && c.isLetter()) {
                    c = c.uppercaseChar()
                    shifted = false
                    rebuildKeyboard()
                }
                actions.onSoftKey(SoftKey.Text(c))
            }
        }
    }

    private fun showPopup(anchor: View, popup: List<PopupDef>) {
        popupPanel.removeAllViews()
        popupViews.clear()
        popup.chunked(POPUP_COLUMNS).forEach { chunk ->
            val row = LinearLayout(context)
            row.orientation = HORIZONTAL
            chunk.forEach { p ->
                val tv = TextView(context)
                tv.text = p.label
                tv.gravity = Gravity.CENTER
                tv.setTextColor(KEY_TEXT)
                tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
                tv.contentDescription = "key:popup:${p.id}"
                tv.isClickable = true
                tv.background = pressedBackground()
                tv.setOnClickListener {
                    dismissPopup()
                    if (p.id == SETTINGS) actions.onOpenSettings() else actions.onSoftKey(SoftKey.Text(p.label[0]))
                }
                popupViews["popup:${p.id}"] = tv
                row.addView(tv, LayoutParams(dp(44f), dp(52f)))
            }
            popupPanel.addView(row)
        }
        // 攔截層與按鍵區同高：若用 MATCH_PARENT，wrap_content 的鍵盤區會被撐到整個螢幕高
        popupCatcher.layoutParams = FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, rowsView.height)
        popupCatcher.visibility = VISIBLE
        popupPanel.visibility = VISIBLE
        popupPanel.measure(MeasureSpec.UNSPECIFIED, MeasureSpec.UNSPECIFIED)
        // 彈出區置於按鍵上方、水平對齊按鍵中心，不超出鍵盤區
        val anchorLoc = IntArray(2)
        val keyboardLoc = IntArray(2)
        anchor.getLocationInWindow(anchorLoc)
        keyboard.getLocationInWindow(keyboardLoc)
        val centerX = anchorLoc[0] - keyboardLoc[0] + anchor.width / 2
        popupPanel.translationX = (centerX - popupPanel.measuredWidth / 2)
            .coerceIn(0, keyboard.width - popupPanel.measuredWidth).toFloat()
        popupPanel.translationY = (anchorLoc[1] - keyboardLoc[1] - popupPanel.measuredHeight).coerceAtLeast(0).toFloat()
    }

    private fun dismissPopup() {
        popupCatcher.visibility = GONE
        popupPanel.visibility = GONE
        popupPanel.removeAllViews()
        popupViews.clear()
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
            val action = ev.actionMasked
            if (action == MotionEvent.ACTION_DOWN) {
                v.isPressed = true
                actions.onSoftKey(SoftKey.Backspace)
                repeatHandler.postDelayed(repeat, 400)
            } else if (action != MotionEvent.ACTION_MOVE) {
                // UP 或 CANCEL：放開即停止連續刪除
                v.isPressed = false
                repeatHandler.removeCallbacks(repeat)
            }
            true
        }
    }

    /** 扁平按鍵：平時透明、按下時淺灰。 */
    private fun pressedBackground(): Drawable = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_pressed), GradientDrawable().apply {
            cornerRadius = dp(6f).toFloat()
            setColor(KEY_PRESSED)
        })
        addState(intArrayOf(), GradientDrawable().apply { setColor(Color.TRANSPARENT) })
    }

    /** 空白鍵：灰色長條，按下時加深。 */
    private fun spaceBackground(): Drawable = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_pressed), InsetDrawable(GradientDrawable().apply {
            cornerRadius = dp(4f).toFloat()
            setColor(SPACE_PRESSED)
        }, dp(4f), dp(10f), dp(4f), dp(10f)))
        addState(intArrayOf(), InsetDrawable(GradientDrawable().apply {
            cornerRadius = dp(4f).toFloat()
            setColor(KEY_PRESSED)
        }, dp(4f), dp(10f), dp(4f), dp(10f)))
    }

    /** Enter 鍵：強調色膠囊。 */
    private fun actionBackground(): Drawable = GradientDrawable().apply {
        cornerRadius = dp(24f).toFloat()
        setColor(ACCENT)
    }

    private data class PopupDef(val id: String, val label: String)

    /** 按鍵定義；char 為字元鍵送出的字元（Tab 鍵標籤為 ⇥、送出 \t）。 */
    private data class KeyDef(
        val id: String,
        val label: String,
        val weight: Float = 1f,
        val hint: Char? = null,
        val popup: List<PopupDef>? = null,
        val char: Char = label[0],
    )

    companion object {
        private val BG = Color.rgb(0xEC, 0xEF, 0xF1)
        private val BAR_BG = Color.rgb(0xDD, 0xE2, 0xE5)
        private val KEY_TEXT = Color.rgb(0x37, 0x47, 0x4F)
        private val KEY_PRESSED = Color.rgb(0xCF, 0xD8, 0xDC)
        private val SPACE_PRESSED = Color.rgb(0xB0, 0xBE, 0xC5)
        private val HINT = Color.rgb(0x78, 0x90, 0x9C)
        private val ACCENT = Color.rgb(0x4D, 0xB6, 0xAC)
        private val FAILURE = Color.rgb(0xE5, 0x39, 0x35)

        private const val ROW_WEIGHT = 10f
        private const val POPUP_COLUMNS = 7
        private const val SETTINGS = "settings"

        private val ACTION_LABELS = mapOf(
            EditorInfo.IME_ACTION_GO to "Go",
            EditorInfo.IME_ACTION_SEARCH to "Search",
            EditorInfo.IME_ACTION_SEND to "Send",
            EditorInfo.IME_ACTION_NEXT to "Next",
            EditorInfo.IME_ACTION_DONE to "Done",
            EditorInfo.IME_ACTION_PREVIOUS to "Prev",
        )

        private fun chars(s: String) = s.map { KeyDef(it.toString(), it.toString()) }

        private val PERIOD_POPUP = listOf(":", "/", "&", "(", ")", "-", "+", ";", "@", "'", "\"", "?", "!", ",").map { PopupDef(it, it) }
        private val COMMA_POPUP = listOf(PopupDef(SETTINGS, "⚙"), PopupDef(",", ","))

        /** 各層共用的最下排：中／英 ?123（或 ABC） , 空白 . Enter。 */
        private fun bottomRow(comma: String, period: String, popups: Boolean) = listOf(
            KeyDef("toggle_english", "中", 1.2f),
            KeyDef("symbols", "?123", 1.2f),
            KeyDef(comma, comma, popup = if (popups) COMMA_POPUP else null),
            KeyDef("space", " ", 4f),
            KeyDef(period, period, popup = if (popups) PERIOD_POPUP else null),
            KeyDef("enter", "↵", 1.6f),
        )

        private val LETTER_ROWS = listOf(
            "qwertyuiop".mapIndexed { i, c -> KeyDef(c.toString(), c.toString(), hint = "1234567890"[i]) },
            chars("asdfghjkl"),
            listOf(KeyDef("shift", "⇧", 1.5f)) + chars("zxcvbnm") + KeyDef("backspace", "⌫", 1.5f),
            bottomRow(",", ".", popups = true),
        )

        private val SYMBOL_ROWS = listOf(
            chars("1234567890"),
            chars("@#$%&*-+()"),
            listOf(KeyDef("alt", "ALT", 1.5f)) + chars("!\"':;/?") + KeyDef("backspace", "⌫", 1.5f),
            bottomRow(",", ".", popups = true),
        )

        private val ALT_ROWS = listOf(
            chars("~`|•√π÷×{}"),
            listOf(KeyDef("tab", "⇥", char = '\t')) + chars("£¢€°^_=[]"),
            listOf(KeyDef("alt", "ALT", 1.5f)) + chars("™®©¶\\<>") + KeyDef("backspace", "⌫", 1.5f),
            bottomRow("„", "…", popups = false),
        )
    }
}
