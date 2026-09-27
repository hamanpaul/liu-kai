package com.hamanpaul.liukai.ime

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
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
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.hamanpaul.liukai.R
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
        addView(buildCandidateBar(), LayoutParams(LayoutParams.MATCH_PARENT, dp(42f)))
        rowsView.orientation = VERTICAL
        rowsView.setPadding(0, dp(2.3f), 0, dp(0.8f))
        keyboard.addView(rowsView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        popupCatcher.visibility = GONE
        popupCatcher.contentDescription = "popup_catcher"
        popupCatcher.setOnClickListener { dismissPopup() }
        // 彈出時其餘按鍵變暗（官方「經典灰」的彈出效果）
        popupCatcher.setBackgroundColor(POPUP_DIM)
        keyboard.addView(popupCatcher, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 0))
        popupPanel.orientation = VERTICAL
        popupPanel.visibility = GONE
        popupPanel.elevation = dp(6f).toFloat()
        popupPanel.background = GradientDrawable().apply { setColor(POPUP_BG) }
        popupPanel.setPadding(dp(2f), dp(2f), dp(2f), 0)
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
        val row = bounds(candidateScroll)
        json.put("candidateRow", row)
        // 探測點：候選列左上角內縮 20px（避開組字文字與組字失敗的紅框），畫面上一定是候選列底色；
        // 測試以截圖確認鍵盤真的畫在螢幕上
        json.put("probe", JSONObject().put("x", PROBE_INSET).put("y", row.getInt("y") + PROBE_INSET).put("color", hex(BAR_BG)))
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
        json.put("rowRects", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { rect(keyViews.getValue(it.id)) }) }))
        json.put("rowColors", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { hex(if (it.function) FUNCTION_TOP else KEY_TOP) }) }))
        json.put("touches", touchesHandled)
    }

    private var windowTop = 0

    /** 已處理完的觸控次數（每次放開手指）；測試點擊後等它增加，確認輸入法處理完才進行下一步。
     *  在主執行緒遞增、由 dump 的 binder 執行緒讀取。 */
    @Volatile private var touchesHandled = 0

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        val handled = super.dispatchTouchEvent(ev)
        // 放開時的點擊（performClick）另外排入佇列執行，計數排在它之後才算處理完
        if (ev.actionMasked == MotionEvent.ACTION_UP) post { touchesHandled++ }
        return handled
    }

    private fun bounds(v: View): JSONObject {
        val loc = IntArray(2)
        v.getLocationInWindow(loc)
        return JSONObject().put("x", loc[0]).put("y", windowTop + loc[1]).put("w", v.width).put("h", v.height)
    }

    private fun rect(v: View): JSONArray {
        val b = bounds(v)
        return JSONArray(listOf(b.getInt("x"), b.getInt("y"), b.getInt("w"), b.getInt("h")))
    }

    private fun hex(color: Int) = String.format("#%06X", color and 0xFFFFFF)

    private fun buildCandidateBar(): View {
        val bar = LinearLayout(context)
        bar.orientation = HORIZONTAL
        bar.gravity = Gravity.CENTER_VERTICAL
        bar.setBackgroundColor(BAR_BG)
        composingView.apply {
            setTextColor(COMPOSING_TEXT)
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
            text.append(label, ForegroundColorSpan(CANDIDATE_LABEL), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
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
        // 每排 71.65dp（按鍵格約 65dp）＝官方「直式按鍵：高」
        rowsFor(layer).forEach { row -> rowsView.addView(buildRow(row), LayoutParams(LayoutParams.MATCH_PARENT, dp(71.65f))) }
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
            // 按鍵獨立分格：寬度依比例分給外層的位置格（每排 10 等分，與官方相同），
            // 按鍵格在位置格內左右各縮 2.5dp、上 5dp 下 1.5dp；若把間隙設在按比例分配的子 View 上，
            // LinearLayout 會先扣掉間隙再分配，按鍵數不同的排（a–l 只有 9 鍵）格寬就會不一致。
            val slot = FrameLayout(context)
            val lp = FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
            lp.setMargins(dp(2.5f), dp(5f), dp(2.5f), dp(1.6f))
            slot.addView(key, lp)
            row.addView(slot, LayoutParams(0, LayoutParams.MATCH_PARENT, def.weight))
        }
        return row
    }

    private fun buildKey(def: KeyDef): View {
        val frame = FrameLayout(context)
        frame.contentDescription = "key:${def.id}"
        frame.isClickable = true
        val icon = def.icon
        if (icon != null) {
            frame.addView(
                ImageView(context).apply { setImageResource(icon) },
                // 空白鍵的 ⎵ 在按鍵下方三分之一處，其他圖示置中
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    if (def.id == "space") Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL else Gravity.CENTER,
                ).apply { bottomMargin = if (def.id == "space") dp(19.5f) else 0 },
            )
        } else {
            val label = TextView(context)
            label.text = labelFor(def)
            label.gravity = Gravity.CENTER
            label.setTextColor(KEY_TEXT)
            label.typeface = Typeface.DEFAULT_BOLD
            // ?123、ABC、ALT、Next 等功能鍵標籤小一號
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, if (label.text.length > 1) 15f else 24f)
            frame.addView(label, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        }
        if (def.id == "shift" || def.id == "alt") {
            // Shift、ALT 右上角的指示點；啟用中為黃綠色
            frame.addView(
                View(context).apply {
                    background = GradientDrawable().apply {
                        shape = GradientDrawable.OVAL
                        setColor(if (activeFor(def)) INDICATOR_ON else INDICATOR_OFF)
                    }
                },
                FrameLayout.LayoutParams(dp(6f), dp(6f), Gravity.TOP or Gravity.END).apply {
                    topMargin = dp(4f)
                    marginEnd = dp(3.5f)
                },
            )
        }
        def.hint?.let { hint ->
            frame.addView(
                TextView(context).apply {
                    text = hint.toString()
                    setTextColor(HINT)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
                    typeface = Typeface.DEFAULT_BOLD
                    setPadding(0, dp(1f), dp(3f), 0)
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
            // 右下角「…」表示可長按彈出
            frame.addView(
                TextView(context).apply {
                    text = "…"
                    setTextColor(POPUP_HINT)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
                    setPadding(0, 0, dp(6f), 0)
                },
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.END),
            )
            frame.setOnLongClickListener {
                showPopup(frame, popup)
                true
            }
        }
        frame.background = keyBackground(def.function)
        if (def.id == "backspace") attachRepeat(frame) else frame.setOnClickListener { onKey(def) }
        return frame
    }

    /** 指示點是否亮起（只用於 Shift 與 ALT）：Shift 按下、目前在 ALT 層。 */
    private fun activeFor(def: KeyDef): Boolean = if (def.id == "shift") shifted else layer == Layer.ALT

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
                tv.typeface = Typeface.DEFAULT_BOLD
                tv.contentDescription = "key:popup:${p.id}"
                tv.isClickable = true
                tv.background = gradientStates(POPUP_TOP, POPUP_BOTTOM, POPUP_PRESSED_TOP, POPUP_PRESSED_BOTTOM)
                tv.setOnClickListener {
                    dismissPopup()
                    if (p.id == SETTINGS) actions.onOpenSettings() else actions.onSoftKey(SoftKey.Text(p.label[0]))
                }
                popupViews["popup:${p.id}"] = tv
                row.addView(tv, LayoutParams(dp(40f), dp(56f)).apply { setMargins(dp(1f), dp(1f), dp(1f), dp(1f)) })
            }
            popupPanel.addView(row)
        }
        // 彈出面板底部的橘色線
        popupPanel.addView(View(context).apply { setBackgroundColor(POPUP_LINE) }, LayoutParams(LayoutParams.MATCH_PARENT, dp(2.7f)))
        // 攔截層與按鍵區同高：若用 MATCH_PARENT，wrap_content 的鍵盤區會被撐到整個螢幕高
        popupCatcher.layoutParams = FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, rowsView.height)
        popupCatcher.visibility = VISIBLE
        popupPanel.visibility = VISIBLE
        popupPanel.measure(MeasureSpec.UNSPECIFIED, MeasureSpec.UNSPECIFIED)
        // 彈出區置於按鍵上方；與官方相同，面板最右一格（預設字）對齊按下的鍵、往左展開，不超出鍵盤區
        val anchorLoc = IntArray(2)
        val keyboardLoc = IntArray(2)
        anchor.getLocationInWindow(anchorLoc)
        keyboard.getLocationInWindow(keyboardLoc)
        val anchorRight = anchorLoc[0] - keyboardLoc[0] + anchor.width
        popupPanel.translationX = (anchorRight - popupPanel.measuredWidth)
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

    /** 候選：平時透明、按下時加深。 */
    private fun pressedBackground(): Drawable = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_pressed), GradientDrawable().apply { setColor(CANDIDATE_PRESSED) })
        addState(intArrayOf(), GradientDrawable().apply { setColor(Color.TRANSPARENT) })
    }

    /** 上淺下深的漸層按鍵格（官方「經典灰」），按下時改用較亮的漸層。 */
    private fun gradientStates(top: Int, bottom: Int, pressedTop: Int, pressedBottom: Int): Drawable = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_pressed), gradient(pressedTop, pressedBottom))
        addState(intArrayOf(), gradient(top, bottom))
    }

    private fun gradient(top: Int, bottom: Int) =
        GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(top, bottom)).apply { cornerRadius = dp(1f).toFloat() }

    /** 按鍵格：一般鍵淺灰漸層、功能鍵深灰漸層。 */
    private fun keyBackground(function: Boolean): Drawable =
        if (function) gradientStates(FUNCTION_TOP, FUNCTION_BOTTOM, PRESSED_TOP, PRESSED_BOTTOM)
        else gradientStates(KEY_TOP, KEY_BOTTOM, PRESSED_TOP, PRESSED_BOTTOM)

    private data class PopupDef(val id: String, val label: String)

    /**
     * 按鍵定義；char 為字元鍵送出的字元（Tab 鍵標籤為 ⇥、送出 \t）；function 為功能鍵（淺灰底）；
     * icon 為以圖示取代文字標籤的按鍵（Shift、Backspace）。
     */
    private data class KeyDef(
        val id: String,
        val label: String,
        val weight: Float = 1f,
        val hint: Char? = null,
        val popup: List<PopupDef>? = null,
        val char: Char = label[0],
        val function: Boolean = false,
        val icon: Int? = null,
    )

    companion object {
        // 色彩取自官方嘸蝦米 PRO「經典灰＋顯示按鍵」截圖取樣（使用者手機的設定）
        private val BG = Color.BLACK
        private val BAR_BG = Color.BLACK
        private val KEY_TOP = Color.rgb(0x8A, 0x8A, 0x8A)
        private val KEY_BOTTOM = Color.rgb(0x6F, 0x6F, 0x6F)
        private val FUNCTION_TOP = Color.rgb(0x4B, 0x4C, 0x4C)
        private val FUNCTION_BOTTOM = Color.rgb(0x32, 0x32, 0x32)
        private val PRESSED_TOP = Color.rgb(0x9D, 0x9D, 0x9D)
        private val PRESSED_BOTTOM = Color.rgb(0x88, 0x88, 0x88)
        private val KEY_TEXT = Color.WHITE
        private val HINT = Color.rgb(0xC4, 0xC4, 0xC4)
        private val POPUP_HINT = Color.rgb(0x76, 0x76, 0x76)
        private val INDICATOR_OFF = Color.rgb(0x3A, 0x3B, 0x3B)
        private val INDICATOR_ON = Color.rgb(0xD0, 0xDD, 0x27)
        private val POPUP_BG = Color.rgb(0x14, 0x14, 0x14)
        private val POPUP_TOP = Color.rgb(0x68, 0x68, 0x68)
        private val POPUP_BOTTOM = Color.rgb(0x4B, 0x4B, 0x4B)
        private val POPUP_PRESSED_TOP = Color.rgb(0xA5, 0xA5, 0xA5)
        private val POPUP_PRESSED_BOTTOM = Color.rgb(0x74, 0x74, 0x74)
        private val POPUP_LINE = Color.rgb(0xC3, 0x76, 0x29)
        private val POPUP_DIM = Color.argb(0x8F, 0, 0, 0)
        private val COMPOSING_TEXT = Color.rgb(0xBD, 0xBD, 0xBD)
        private val CANDIDATE_LABEL = Color.rgb(0x9E, 0x9E, 0x9E)
        private val CANDIDATE_PRESSED = Color.rgb(0x33, 0x33, 0x33)
        private val ACCENT = Color.rgb(0x4D, 0xB6, 0xAC)
        private val FAILURE = Color.rgb(0xE5, 0x39, 0x35)

        private const val ROW_WEIGHT = 10f
        private const val PROBE_INSET = 20
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
            KeyDef("toggle_english", "中", 1.3f, function = true),
            KeyDef("symbols", "?123", 1.2f, function = true),
            KeyDef(comma, comma, popup = if (popups) COMMA_POPUP else null, function = true),
            KeyDef("space", "␣", 4f, char = ' ', function = true, icon = R.drawable.liukai_ic_space),
            KeyDef(period, period, popup = if (popups) PERIOD_POPUP else null, function = true),
            KeyDef("enter", "↵", 1.5f, function = true),
        )

        private val SHIFT = KeyDef("shift", "⇧", 1.5f, function = true, icon = R.drawable.liukai_ic_shift)
        private val BACKSPACE = KeyDef("backspace", "⌫", 1.5f, function = true, icon = R.drawable.liukai_ic_backspace)
        private val ALT = KeyDef("alt", "ALT", 1.5f, function = true)

        private val LETTER_ROWS = listOf(
            "qwertyuiop".mapIndexed { i, c -> KeyDef(c.toString(), c.toString(), hint = "1234567890"[i]) },
            chars("asdfghjkl"),
            listOf(SHIFT) + chars("zxcvbnm") + BACKSPACE,
            bottomRow(",", ".", popups = true),
        )

        private val SYMBOL_ROWS = listOf(
            chars("1234567890"),
            chars("@#$%&*-+()"),
            listOf(ALT) + chars("!\"':;/?") + BACKSPACE,
            bottomRow(",", ".", popups = true),
        )

        private val ALT_ROWS = listOf(
            chars("~`|•√π÷×{}"),
            listOf(KeyDef("tab", "⇥", char = '\t')) + chars("£¢€°^_=[]"),
            listOf(ALT) + chars("™®©¶\\<>") + BACKSPACE,
            bottomRow("„", "…", popups = false),
        )
    }
}
