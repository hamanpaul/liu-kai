package com.hamanpaul.liukai.ime

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.media.AudioManager
import android.os.VibrationEffect
import android.os.Vibrator
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
import com.hamanpaul.liukai.core.engine.CodeHint
import com.hamanpaul.liukai.core.engine.InputMode
import com.hamanpaul.liukai.core.engine.Language
import com.hamanpaul.liukai.core.ime.EnterAction
import com.hamanpaul.liukai.core.ime.SoftKey
import com.hamanpaul.liukai.data.KeyboardSettings
import com.hamanpaul.liukai.data.KeyboardTheme
import org.json.JSONArray
import org.json.JSONObject

interface ImeActions {
    fun onSoftKey(key: SoftKey)
    fun onCandidateTap(index: Int)
    fun onOpenSettings()
    /** 候選列右端的麥克風：切換到系統的語音輸入。 */
    fun onVoice()
    /** 中文模式閒置時候選列的常用標點：直接上屏。 */
    fun onPunctuation(text: String)
    /** 長按「同音」選單選的語言模式。 */
    fun onLanguage(language: Language)
    /** 空白鍵左右滑動：移動游標（負數往左）。 */
    fun onCursor(delta: Int)
    /** 退出鍵盤鍵：收起輸入法。 */
    fun onHide()
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
    /** 目前的語言模式與可選的語言模式（長按「同音」的選單）。 */
    val language: Language = Language.TRADITIONAL,
    val languages: List<Language> = listOf(Language.TRADITIONAL),
    /** 同音查碼：從同音字清單上屏的字與字碼，顯示到下一次按鍵。 */
    val codeHint: CodeHint? = null,
    /** 智慧鍵盤：組字中可接的下一碼（null 為不限制）。 */
    val nextKeys: Set<Char>? = null,
)

/**
 * 輸入畫面：上方候選列、下方螢幕鍵盤。偵測到實體鍵盤時只顯示候選列。
 *
 * 螢幕鍵盤照官方嘸蝦米 PRO 的配置：
 * - 中文模式字母層：鍵帽大寫（q–p 附數字提示、長按輸入數字）；同音 Z–M ⌫；En ?123 , 嘸蝦米 .'[] Enter。
 *   「.'[]」鍵點按為字根「.」、長按彈出字根 ' [ ]。
 * - 英文模式字母層：鍵帽小寫；⇧ z–m ⌫；中 ?123 , 空白 . Enter；長按「.」彈出標點。
 * - ?123 層與 ALT 層：左下為「中」、ABC 回字母層；中文模式時空白鍵與「.'[]」鍵同中文字母層。
 * 長按「,」彈出設定；候選列右端為語音鍵。萬用字元 *、反引號 ` 與官方相同放在 ?123／ALT 層，切換鍵盤層不影響組字。
 * 候選與按鍵都是獨立的標準 View（含 contentDescription），確保點擊判定可靠、也可被 UiAutomator 定位。
 */
@SuppressLint("ViewConstructor")
class ImeView(context: Context, private val actions: ImeActions) : LinearLayout(context) {
    private val density = resources.displayMetrics.density
    private fun dp(v: Float) = (v * density).toInt()

    private enum class Layer { LETTERS, SYMBOLS, ALT }

    /** Shift：關、單次大寫（打一個字母後放開）、大寫鎖定。 */
    private enum class Shift { OFF, ONCE, LOCKED }

    private val bar = LinearLayout(context)
    private val composingView = TextView(context)
    private val candidateRow = LinearLayout(context)
    private val candidateScroll = HorizontalScrollView(context)
    private val keyboard = FrameLayout(context)
    private val rowsView = LinearLayout(context)
    /** 彈出鍵盤顯示時蓋住整個鍵盤區：點彈出區以外只收起彈出鍵盤。 */
    private val popupCatcher = View(context)
    private val popupPanel = LinearLayout(context)

    private var layer = Layer.LETTERS
    private var shift = Shift.OFF
    /** 單次大寫是自動大寫設的（游標處不再需要大寫時自動放開）。 */
    private var autoShifted = false
    private val shifted: Boolean get() = shift != Shift.OFF
    /** 在 ?123／ALT 層打過符號：接著按空白或 Enter 時自動返回字母層（設定「自動返回字元輸入模式」）。 */
    private var typedSymbol = false
    private var settings = KeyboardSettings()
    /** 目前主題的顏色與樣式（見 [Palettes]）。 */
    private var pal = Palettes.of(KeyboardTheme.GRAY, true)
    /** 按鍵放大預覽（按住時顯示在按鍵上方）。 */
    private val preview = PreviewDrawable()
    private var previewKey: String? = null
    /** 震動與音效的次數（診斷輸出，供端對端測試確認設定生效）。 */
    private var vibrations = 0
    /** 智慧鍵盤目前的下一碼（見 [UiState.nextKeys]）。 */
    private var smartKeys: Set<Char>? = null
    private var sounds = 0
    private var mode: InputMode = InputMode.CHINESE
    private var tableLoaded = true
    private var enterLabel = "↵"
    private var shownCandidates: List<Candidate> = emptyList()
    private val keyViews = LinkedHashMap<String, View>()
    private val popupViews = LinkedHashMap<String, View>()
    private val punctViews = LinkedHashMap<String, View>()
    private val voiceKey = ImageView(context)
    /** 最近一次 render 的狀態（候選列依它重建；切換鍵盤層時也要重建常用標點）。 */
    private var lastState: UiState? = null
    private val repeatHandler = Handler(Looper.getMainLooper())
    private val failureFrame = GradientDrawable().apply {
        setColor(Color.TRANSPARENT)
        setStroke(dp(3f), FAILURE)
    }

    init {
        orientation = VERTICAL
        addView(buildCandidateBar(), LayoutParams(LayoutParams.MATCH_PARENT, dp(42f)))
        rowsView.orientation = VERTICAL
        rowsView.setPadding(0, dp(2.3f), 0, dp(0.8f))
        keyboard.addView(rowsView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        popupCatcher.visibility = GONE
        popupCatcher.contentDescription = "popup_catcher"
        popupCatcher.setOnClickListener { dismissPopup() }
        keyboard.addView(popupCatcher, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 0))
        popupPanel.orientation = VERTICAL
        popupPanel.visibility = GONE
        popupPanel.elevation = dp(6f).toFloat()
        popupPanel.setPadding(dp(2f), dp(2f), dp(2f), 0)
        keyboard.addView(popupPanel, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        addView(keyboard, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        applyPalette()
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

    /** 中文模式（有字表）：字母層用中文配置、鍵帽大寫。 */
    private val chinese: Boolean get() = tableLoaded && mode == InputMode.CHINESE

    /** 回到字母層、放開 Shift、收起彈出鍵盤（換到新的輸入欄時呼叫）。 */
    fun resetLayout() {
        layer = Layer.LETTERS
        shift = Shift.OFF
        autoShifted = false
        typedSymbol = false
        dismissPopup()
        rebuildKeyboard()
    }

    /** 套用鍵盤設定（輸入法每次顯示鍵盤時讀取）。 */
    fun applySettings(settings: KeyboardSettings) {
        this.settings = settings
        // 震動、音效次數從這次顯示鍵盤開始計算
        vibrations = 0
        sounds = 0
        pal = Palettes.of(settings.theme, settings.showKeys)
        applyPalette()
    }

    /** 套用主題色到整個輸入畫面並重建按鍵。 */
    private fun applyPalette() {
        setBackgroundColor(pal.bg)
        bar.setBackgroundColor(pal.bar)
        composingView.setTextColor(pal.hint)
        voiceKey.setColorFilter(pal.mic)
        // 彈出時其餘按鍵變暗
        popupCatcher.setBackgroundColor(pal.popupDim)
        popupPanel.background = GradientDrawable().apply { setColor(pal.popupBg) }
        rebuildKeyboard()
    }

    /**
     * 自動大寫（英文模式）：游標處需要大寫時設為單次大寫；不再需要時放開自動設的單次大寫。
     * 大寫鎖定與使用者手動設的 Shift 不受影響。
     */
    fun setAutoCaps(caps: Boolean) {
        if (shift == Shift.LOCKED) return
        if (caps && shift == Shift.OFF) {
            shift = Shift.ONCE
            autoShifted = true
            rebuildKeyboard()
        } else if (!caps && autoShifted) {
            shift = Shift.OFF
            autoShifted = false
            rebuildKeyboard()
        }
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
        lastState = state
        if (state.mode != mode || state.tableLoaded != tableLoaded) {
            mode = state.mode
            tableLoaded = state.tableLoaded
            // 中文模式沒有 Shift；切換模式時放開，避免回到英文時仍是大寫
            shift = Shift.OFF
            autoShifted = false
            rebuildKeyboard()
        }
        if (state.nextKeys != smartKeys) {
            smartKeys = state.nextKeys
            rebuildKeyboard()
        }
        composingView.text = when {
            state.homophoneOf != null -> "音:${state.homophoneOf}"
            state.codeHint != null -> hintText(state.codeHint)
            else -> state.composing
        }
        refreshBar()
        candidateScroll.post { candidateScroll.scrollTo(0, 0) }
    }

    /** 同音查碼的顯示文字，例如「忠 qa」（多個字碼以／分隔）。 */
    fun hintText(hint: CodeHint): String = "${hint.text} ${hint.codes.joinToString("／")}"

    /** 重建候選列：有候選時列出候選；字母層閒置時列出常用標點（與官方相同）。 */
    private fun refreshBar() {
        candidateRow.removeAllViews()
        punctViews.clear()
        val state = lastState
        shownCandidates = state?.candidates.orEmpty()
        if (state == null) return
        state.candidates.forEachIndexed { index, cand ->
            candidateRow.addView(candidateView(index, cand, state))
        }
        // 字母層沒有組字時列出常用標點（官方 3.0.9 中英文模式皆同；?123／ALT 層不顯示）
        if (state.candidates.isEmpty() && state.composing.isEmpty() && layer == Layer.LETTERS) {
            PUNCTUATION.forEachIndexed { i, p ->
                if (i > 0) candidateRow.addView(View(context).apply { setBackgroundColor(withAlpha(pal.strip, 0x55)) }, LayoutParams(dp(1f), dp(20f)))
                candidateRow.addView(punctView(p))
            }
        }
    }

    private fun punctView(p: String): View {
        val tv = TextView(context)
        tv.text = p
        tv.setTextColor(pal.strip)
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
        tv.gravity = Gravity.CENTER
        tv.minWidth = dp(38f)
        tv.contentDescription = "key:punct:$p"
        tv.isClickable = true
        tv.setOnClickListener { actions.onPunctuation(p) }
        tv.background = pressedBackground()
        punctViews["punct:$p"] = tv
        return tv
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
        json.put("probe", JSONObject().put("x", PROBE_INSET).put("y", row.getInt("y") + PROBE_INSET).put("color", hex(pal.bar)))
        val candidates = JSONArray()
        for (i in shownCandidates.indices) {
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
        for ((id, view) in punctViews) keys.put(id, bounds(view))
        keys.put("voice", bounds(voiceKey))
        json.put("keys", keys)
        json.put("layer", layer.name.lowercase())
        json.put("rows", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { it.id }) }))
        // 各鍵顯示的標籤；以圖示顯示的鍵（⇧ ⌫ ␣）為其代表字元
        json.put("rowLabels", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { if (iconFor(it) != null) it.label else labelFor(it) }) }))
        json.put("strip", JSONArray(punctViews.keys.toList()))
        json.put("enterLabel", enterLabel)
        json.put("popup", JSONArray(popupViews.keys.toList()))
        json.put("rowRects", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { rect(keyViews.getValue(it.id)) }) }))
        json.put("rowColors", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { hex(if (it.function) pal.fnTop else pal.keyTop) }) }))
        json.put(
            "palette",
            JSONObject().put("bg", hex(pal.bg)).put("bar", hex(pal.bar)).put("text", hex(pal.text)).put("strip", hex(pal.strip))
                .put("cells", pal.cells),
        )
        json.put("touches", touchesHandled)
        json.put("fontScale", fontScale().toDouble())
        json.put("rowHeight", dp(rowHeightDp()))
        json.put("shift", shift.name.lowercase())
        json.put("dimmed", JSONArray(rowsFor(layer).flatten().filter(::dimmed).map { it.id }))
        json.put("hidden", JSONArray(keyViews.filterValues { it.visibility != VISIBLE }.keys.toList()))
        json.put("preview", previewKey ?: JSONObject.NULL)
        json.put("feedback", JSONObject().put("vibrate", vibrations).put("sound", sounds))
    }

    private var windowTop = 0

    /** 已處理完的觸控次數（每次放開手指）；測試點擊後等它增加，確認輸入法處理完才進行下一步。
     *  在主執行緒遞增、由 dump 的 binder 執行緒讀取。 */
    @Volatile private var touchesHandled = 0

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        // 按下鍵盤區時的震動與音效（設定「按鍵時震動」「按鍵時播放音效」）
        if (ev.actionMasked == MotionEvent.ACTION_DOWN && ev.y >= keyboard.top) keyFeedback()
        val handled = super.dispatchTouchEvent(ev)
        // 放開時的點擊（performClick）另外排入佇列執行，計數排在它之後才算處理完
        if (ev.actionMasked == MotionEvent.ACTION_UP) post { touchesHandled++ }
        return handled
    }

    private fun keyFeedback() {
        if (settings.vibrate) {
            context.getSystemService(Vibrator::class.java).vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK))
            vibrations++
        }
        if (settings.sound) {
            context.getSystemService(AudioManager::class.java).playSoundEffect(AudioManager.FX_KEYPRESS_STANDARD, -1f)
            sounds++
        }
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
        bar.orientation = HORIZONTAL
        bar.gravity = Gravity.CENTER_VERTICAL
        composingView.apply {
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
        voiceKey.setImageResource(R.drawable.liukai_ic_mic)
        voiceKey.scaleType = ImageView.ScaleType.CENTER
        voiceKey.contentDescription = "key:voice"
        voiceKey.isClickable = true
        voiceKey.setOnClickListener { actions.onVoice() }
        voiceKey.background = pressedBackground()
        bar.addView(voiceKey, LayoutParams(dp(44f), LayoutParams.MATCH_PARENT))
        return bar
    }

    private fun candidateView(index: Int, cand: Candidate, state: UiState): View {
        val tv = TextView(context)
        val text = SpannableStringBuilder()
        val pageOffset = index - state.pageStart
        if (pageOffset in 0 until state.pageSize) {
            // 選字鍵 0–9：0 為預設字（空白上屏的字）
            val label = "$pageOffset"
            text.append(label, ForegroundColorSpan(pal.hint), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
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
        tv.setTextColor(pal.text)
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
        // 排高依設定的按鍵高度（直式、橫式各五段，量測官方）；按鍵格在排內上 5dp 下 1.6dp 內縮
        val rowHeight = dp(rowHeightDp())
        rowsFor(layer).forEach { row -> rowsView.addView(buildRow(row), LayoutParams(LayoutParams.MATCH_PARENT, rowHeight)) }
        // 常用標點只在中文字母層顯示，切換鍵盤層或模式時跟著更新
        refreshBar()
    }

    private fun rowsFor(layer: Layer): List<List<KeyDef>> = when (layer) {
        Layer.LETTERS -> letterRows(if (chinese) CHINESE_ROWS else LETTER_ROWS)
        Layer.SYMBOLS -> if (chinese) CHINESE_SYMBOL_ROWS else SYMBOL_ROWS
        Layer.ALT -> ALT_ROWS
    }

    /**
     * 字母層依設定調整：「顯示數字鍵」在最上方加一排 1–0，第一排字母不再有數字提示與長按數字；
     * 「顯示退出鍵盤鍵」在最下排最左邊加退出鍵（與官方相同，其餘鍵依比例縮窄）。
     */
    private fun letterRows(base: List<List<KeyDef>>): List<List<KeyDef>> {
        var rows = smartRows(base)
        if (settings.numberRow) rows = listOf(NUMBER_ROW, rows[0].map { it.copy(hint = null) }) + rows.drop(1)
        if (settings.doneKey) rows = rows.dropLast(1) + listOf(listOf(HIDE) + rows.last().map { it.copy(weight = DONE_ROW_WEIGHTS.getValue(it.id)) })
        return rows
    }

    private val landscape: Boolean get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    /** 排高（dp）：直式、橫式各依設定的按鍵高度。 */
    private fun rowHeightDp(): Float =
        if (landscape) LANDSCAPE_ROW_DP[settings.landscapeHeight.ordinal] else PORTRAIT_ROW_DP[settings.portraitHeight.ordinal]

    /** 按鍵字體縮放：直式、橫式各依設定的字體大小。 */
    private fun fontScale(): Float =
        FONT_SCALE[(if (landscape) settings.landscapeFont else settings.portraitFont).ordinal]

    /** 智慧鍵盤作用中：設定開啟、中文字母層、組字中。 */
    private val smartActive: Boolean get() = settings.smartKeyboard && chinese && smartKeys != null

    /**
     * 智慧鍵盤（照官方）：組字中「,」「.'[]」不能接時從最下排移除、空白鍵加寬；字母鍵不能接時淡化（仍可點，點了為組字失敗）；
     * 同音鍵留白。
     */
    private fun smartRows(base: List<List<KeyDef>>): List<List<KeyDef>> {
        if (!smartActive) return base
        val next = smartKeys!!
        val bottom = base.last()
        val hidden = bottom.filter { (it.id == "," && ',' !in next) || (it.id == "." && ROOT_KEYS.none { c -> c in next }) }
        val space = bottom.first { it.id == "space" }
        val widened = bottom.filter { it !in hidden }.map { if (it === space) it.copy(weight = it.weight + hidden.sumOf { h -> h.weight.toDouble() }.toFloat()) else it }
        return base.dropLast(1) + listOf(widened)
    }

    /** 智慧鍵盤淡化的字母鍵。 */
    private fun dimmed(def: KeyDef): Boolean =
        smartActive && def.id.length == 1 && def.id[0] in 'a'..'z' && def.id[0] !in smartKeys!!

    /** 以圖示顯示的鍵；中文模式的空白鍵改顯示「嘸蝦米」文字。 */
    private fun iconFor(def: KeyDef): Int? = if (def.id == "space" && chinese) null else def.icon

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
        val icon = iconFor(def)
        // 不分格時空白鍵為膠囊、Enter 為圓形（量測官方：膠囊高約 26dp、圓直徑約 36dp）
        if (!pal.cells && def.id == "space") frame.addView(shape(pal.spacePill, false), FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, dp(26f), Gravity.CENTER))
        if (!pal.cells && def.id == "enter") frame.addView(shape(pal.enterCircle, true), FrameLayout.LayoutParams(dp(36f), dp(36f), Gravity.CENTER))
        if (icon != null) {
            frame.addView(
                ImageView(context).apply { setImageResource(icon); setColorFilter(pal.icon) },
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
            label.setTextColor(if (def.id == "enter") pal.enterText else pal.text)
            label.typeface = if (label.text.length > 1 || pal.boldLetters) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
            // ?123、ABC、ALT、Next 等功能鍵標籤小一號
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, (if (label.text.length > 1) 15f else 24f) * fontScale())
            if (dimmed(def)) label.alpha = DIM_ALPHA
            frame.addView(label, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        }
        if (def.id == "shift" || def.id == "alt") {
            // Shift、ALT 的指示：經典灰為右上角圓點（啟用黃綠、鎖定橘色），其餘主題為下方橫線
            val indicator = View(context).apply {
                background = GradientDrawable().apply {
                    shape = if (pal.indicatorDot) GradientDrawable.OVAL else GradientDrawable.RECTANGLE
                    setColor(indicatorColor(def))
                }
            }
            frame.addView(
                indicator,
                if (pal.indicatorDot) {
                    FrameLayout.LayoutParams(dp(6f), dp(6f), Gravity.TOP or Gravity.END).apply {
                        topMargin = dp(4f)
                        marginEnd = dp(3.5f)
                    }
                } else {
                    FrameLayout.LayoutParams(dp(24f), dp(2f), Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = dp(3f) }
                },
            )
        }
        def.hint?.let { hint ->
            frame.addView(
                TextView(context).apply {
                    text = hint.toString()
                    setTextColor(pal.hint)
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
        // 智慧鍵盤：組字中同音鍵留白（與官方相同）
        if (def.id == "homophone" && smartActive) frame.visibility = INVISIBLE
        if (def.id == "homophone") {
            // 長按「同音」：語言模式選單（嘸／无／台／日），目前的模式以強調色標示
            frame.setOnLongClickListener {
                val state = lastState!!
                showPopup(frame, state.languages.map { PopupDef("lang:${it.name}", LANGUAGE_LABELS.getValue(it)) }, "lang:${state.language.name}")
                true
            }
        }
        def.accents?.let { accents ->
            // 英文字母長按彈出重音字母（Shift 時為大寫）
            frame.setOnLongClickListener {
                showPopup(frame, accents.map { c -> (if (shifted) c.uppercaseChar() else c).toString().let { PopupDef(it, it) } })
                true
            }
        }
        def.popup?.let { popup ->
            // 右下角「…」表示可長按彈出
            frame.addView(
                TextView(context).apply {
                    text = "…"
                    setTextColor(pal.popupMark)
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
        frame.background = keyBackground(def)
        when (def.id) {
            "backspace" -> attachRepeat(frame)
            "space" -> attachSpaceSlide(frame, def)
            else -> {
                frame.setOnClickListener { onKey(def) }
                if (icon == null && def.id != "enter") attachPreview(frame, def)
            }
        }
        return frame
    }

    /** 指示點是否亮起（只用於 Shift 與 ALT）：Shift 按下、目前在 ALT 層。 */
    private fun activeFor(def: KeyDef): Boolean = if (def.id == "shift") shifted else layer == Layer.ALT

    /** 指示點顏色：大寫鎖定為橘色，其餘啟用為黃綠色。 */
    private fun indicatorColor(def: KeyDef): Int = when {
        !activeFor(def) -> pal.indicatorOff
        def.id == "shift" && shift == Shift.LOCKED -> pal.indicatorLock
        else -> pal.indicatorOn
    }

    /** 按住時在按鍵上方顯示放大的標籤（設定「按鍵時顯示彈出式視窗」）；放開或取消時收起。 */
    @SuppressLint("ClickableViewAccessibility")
    private fun attachPreview(key: View, def: KeyDef) {
        key.setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> if (settings.keyPreview) showPreview(v, def)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> hidePreview()
            }
            false
        }
    }

    private fun showPreview(key: View, def: KeyDef) {
        val keyLoc = IntArray(2)
        val rootLoc = IntArray(2)
        key.getLocationInWindow(keyLoc)
        getLocationInWindow(rootLoc)
        val left = keyLoc[0] - rootLoc[0]
        val top = keyLoc[1] - rootLoc[1]
        preview.text = labelFor(def)
        if (pal.previewAbove) {
            // 經典灰（量測官方）：寬為鍵寬 5/3、高為鍵高 9/7 的黑框，底邊在鍵頂上方 0.3 鍵高
            val w = key.width * 5 / 3
            val h = key.height * 9 / 7
            val bottom = top - key.height * 3 / 10
            val l = left + key.width / 2 - w / 2
            preview.setBounds(l, maxOf(0, bottom - h), l + w, bottom)
        } else {
            // 其餘主題：蓋住按鍵並往上延伸一個鍵高，標籤畫在上半部
            preview.setBounds(left, maxOf(0, top - key.height), left + key.width, top + key.height)
        }
        overlay.remove(preview)
        overlay.add(preview)
        previewKey = def.id
    }

    private fun hidePreview() {
        overlay.remove(preview)
        previewKey = null
    }

    /**
     * 空白鍵：點按輸入空白；沒有組字時按住左右滑動移動游標（每 [CURSOR_STEP_DP] 移一格），滑動過就不輸入空白。
     */
    @SuppressLint("ClickableViewAccessibility")
    private fun attachSpaceSlide(key: View, def: KeyDef) {
        var startX = 0f
        var moved = 0
        var slid = false
        key.setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.isPressed = true
                    startX = ev.x
                    moved = 0
                    slid = false
                }
                MotionEvent.ACTION_MOVE -> if (lastState!!.composing.isEmpty()) {
                    val steps = ((ev.x - startX) / dp(CURSOR_STEP_DP)).toInt()
                    while (moved != steps) {
                        val delta = if (steps > moved) 1 else -1
                        actions.onCursor(delta)
                        moved += delta
                        slid = true
                    }
                }
                MotionEvent.ACTION_UP -> {
                    v.isPressed = false
                    if (!slid) onKey(def)
                }
                else -> v.isPressed = false
            }
            true
        }
    }

    /**
     * 按鍵標籤。中英鍵與官方相同顯示「要切去的模式」：中文字母層顯示 En、英文字母層與 ?123／ALT 層顯示「中」；
     * 沒有字表時顯示「無表」。中文模式與 Shift 時字母鍵帽大寫。
     */
    private fun labelFor(def: KeyDef): String = when (def.id) {
        "toggle_english" -> when {
            !tableLoaded -> "無表"
            layer == Layer.LETTERS && mode == InputMode.CHINESE -> "En"
            else -> "中"
        }
        "symbols" -> if (layer == Layer.LETTERS) "?123" else "ABC"
        "enter" -> enterLabel
        "space" -> "嘸蝦米"
        else -> if ((chinese || shifted) && def.label.length == 1 && def.label[0] in 'a'..'z') def.label.uppercase() else def.label
    }

    private fun onKey(def: KeyDef) {
        when (def.id) {
            "shift" -> {
                // 關 → 單次大寫 → 大寫鎖定 → 關（官方：再按一次為大寫鎖定）
                shift = when (shift) {
                    Shift.OFF -> Shift.ONCE
                    Shift.ONCE -> Shift.LOCKED
                    Shift.LOCKED -> Shift.OFF
                }
                autoShifted = false
                rebuildKeyboard()
            }
            "hide" -> actions.onHide()
            "homophone" -> actions.onSoftKey(SoftKey.Homophone)
            "symbols" -> {
                layer = if (layer == Layer.LETTERS) Layer.SYMBOLS else Layer.LETTERS
                shift = Shift.OFF
                autoShifted = false
                typedSymbol = false
                rebuildKeyboard()
            }
            "alt" -> { layer = if (layer == Layer.ALT) Layer.SYMBOLS else Layer.ALT; rebuildKeyboard() }
            "space" -> { actions.onSoftKey(SoftKey.Space); autoReturn() }
            "enter" -> { actions.onSoftKey(SoftKey.Enter); autoReturn() }
            "toggle_english" -> actions.onSoftKey(SoftKey.ToggleEnglish)
            else -> typeChar(def.char)
        }
    }

    /** 送出字元：Shift 時字母大寫（單次大寫用過即放開）；在 ?123／ALT 層打的字元記為「打過符號」。 */
    private fun typeChar(char: Char) {
        var c = char
        if (shifted && c.isLetter()) {
            c = c.uppercaseChar()
            if (shift == Shift.ONCE) {
                shift = Shift.OFF
                autoShifted = false
                rebuildKeyboard()
            }
        }
        if (layer != Layer.LETTERS) typedSymbol = true
        actions.onSoftKey(SoftKey.Text(c))
    }

    /** 在 ?123／ALT 層打過符號後按空白或 Enter：自動返回字母層（設定「自動返回字元輸入模式」）。 */
    private fun autoReturn() {
        if (settings.autoReturn && typedSymbol) {
            layer = Layer.LETTERS
            typedSymbol = false
            rebuildKeyboard()
        }
    }

    private fun showPopup(anchor: View, popup: List<PopupDef>, selected: String? = null) {
        popupPanel.removeAllViews()
        popupViews.clear()
        popup.chunked(POPUP_COLUMNS).forEach { chunk ->
            val row = LinearLayout(context)
            row.orientation = HORIZONTAL
            chunk.forEach { p ->
                val tv = TextView(context)
                tv.text = p.label
                tv.gravity = Gravity.CENTER
                tv.setTextColor(pal.text)
                tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
                tv.typeface = Typeface.DEFAULT_BOLD
                tv.contentDescription = "key:popup:${p.id}"
                tv.isClickable = true
                tv.background = if (p.id == selected) gradientStates(ACCENT, ACCENT, pal.pressedTop, pal.pressedBottom)
                else gradientStates(pal.popupKeyTop, pal.popupKeyBottom, pal.pressedTop, pal.pressedBottom)
                tv.setOnClickListener {
                    dismissPopup()
                    when {
                        p.id == SETTINGS -> actions.onOpenSettings()
                        p.id.startsWith(LANG) -> actions.onLanguage(Language.valueOf(p.id.removePrefix(LANG)))
                        else -> typeChar(p.label[0])
                    }
                }
                popupViews["popup:${p.id}"] = tv
                row.addView(tv, LayoutParams(dp(40f), dp(56f)).apply { setMargins(dp(1f), dp(1f), dp(1f), dp(1f)) })
            }
            popupPanel.addView(row)
        }
        // 彈出面板底部的線（經典灰為橘色，其餘主題透明）
        popupPanel.addView(View(context).apply { setBackgroundColor(pal.popupLine) }, LayoutParams(LayoutParams.MATCH_PARENT, dp(2.7f)))
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
        addState(intArrayOf(android.R.attr.state_pressed), GradientDrawable().apply { setColor(withAlpha(pal.text, 0x33)) })
        addState(intArrayOf(), GradientDrawable().apply { setColor(Color.TRANSPARENT) })
    }

    /** 上淺下深的漸層按鍵格（官方「經典灰」），按下時改用較亮的漸層。 */
    private fun gradientStates(top: Int, bottom: Int, pressedTop: Int, pressedBottom: Int): Drawable = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_pressed), gradient(pressedTop, pressedBottom))
        addState(intArrayOf(), gradient(top, bottom))
    }

    private fun gradient(top: Int, bottom: Int) =
        GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(top, bottom)).apply { cornerRadius = dp(1f).toFloat() }

    /**
     * 按鍵格：分格時一般鍵與功能鍵各用主題色（經典灰為上淺下深漸層），Enter 用主題的 Enter 色；
     * 不分格時按鍵透明，只有按下時顯示按下色。
     */
    private fun keyBackground(def: KeyDef): Drawable = when {
        !pal.cells -> gradientStates(Color.TRANSPARENT, Color.TRANSPARENT, pal.pressedTop, pal.pressedBottom)
        def.id == "enter" -> gradientStates(pal.enter, pal.enter, pal.pressedTop, pal.pressedBottom)
        def.function -> gradientStates(pal.fnTop, pal.fnBottom, pal.pressedTop, pal.pressedBottom)
        else -> gradientStates(pal.keyTop, pal.keyBottom, pal.pressedTop, pal.pressedBottom)
    }

    /** 不分格時空白鍵的膠囊或 Enter 的圓形。 */
    private fun shape(color: Int, oval: Boolean) = View(context).apply {
        background = GradientDrawable().apply {
            shape = if (oval) GradientDrawable.OVAL else GradientDrawable.RECTANGLE
            cornerRadius = dp(13f).toFloat()
            setColor(color)
        }
    }

    private fun withAlpha(color: Int, a: Int) = Color.argb(a, Color.red(color), Color.green(color), Color.blue(color))

    private data class PopupDef(val id: String, val label: String)

    /** 按鍵放大預覽：圓角底色，標籤畫在上半部（按鍵上方）。 */
    private inner class PreviewDrawable : Drawable() {
        var text = ""
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
        private val border = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1.5f).toFloat() }
        private val fg = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textAlign = Paint.Align.CENTER
            typeface = Typeface.DEFAULT_BOLD
            textSize = dp(34f).toFloat()
        }

        override fun draw(canvas: Canvas) {
            val b = bounds
            bg.color = pal.previewBg
            fg.color = pal.text
            canvas.drawRoundRect(RectF(b), dp(4f).toFloat(), dp(4f).toFloat(), bg)
            // 經典灰：獨立黑框（灰色邊），字置中；其餘主題：長條上半部放字
            val textY = if (pal.previewAbove) {
                border.color = GRAY_PREVIEW_BORDER
                canvas.drawRoundRect(RectF(b), dp(4f).toFloat(), dp(4f).toFloat(), border)
                b.exactCenterY()
            } else {
                b.top + b.height() / 4f
            }
            canvas.drawText(text, b.exactCenterX(), textY - (fg.descent() + fg.ascent()) / 2, fg)
        }

        override fun setAlpha(alpha: Int) = Unit
        override fun setColorFilter(colorFilter: ColorFilter?) = Unit
        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
    }

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
        /** 英文字母長按彈出的重音字母（官方：a、s、c、n）。 */
        val accents: String? = null,
    )

    companion object {
        // 主題色見 Palettes（量測官方 PRO 各主題）；以下為與主題無關的顏色
        /** 經典灰按鍵預覽黑框的灰色邊。 */
        private val GRAY_PREVIEW_BORDER = Color.rgb(0x46, 0x46, 0x46)
        private val ACCENT = Color.rgb(0x4D, 0xB6, 0xAC)
        private val FAILURE = Color.rgb(0xE5, 0x39, 0x35)

        private const val ROW_WEIGHT = 10f
        /**
         * 各段按鍵高度的排高（dp，依 KeyHeight：高、稍高、適中、稍低、低），量測官方 PRO 3.0.9：
         * 直式排距 188／172／157／141／125px，橫式 144／132／120／108／96px（420dpi，1dp＝2.625px）。
         */
        private val PORTRAIT_ROW_DP = floatArrayOf(71.62f, 65.52f, 59.81f, 53.71f, 47.62f)
        private val LANDSCAPE_ROW_DP = floatArrayOf(54.86f, 50.29f, 45.71f, 41.14f, 36.57f)
        /** 各段字體大小相對「適中」的比例（依 FontSize：大、稍大、適中、稍小、小），量測官方字形高度。 */
        private val FONT_SCALE = floatArrayOf(1.2f, 1.09f, 1f, 0.92f, 0.8f)

        /** 空白鍵滑動移動游標：每滑動這個距離（dp）移一格。 */
        private const val CURSOR_STEP_DP = 12f
        /** 智慧鍵盤淡化的字母鍵標籤透明度。 */
        private const val DIM_ALPHA = 0.3f
        /** 「.'[]」鍵可輸入的字根。 */
        private const val ROOT_KEYS = ".'[]"
        private const val PROBE_INSET = 20
        private const val POPUP_COLUMNS = 7
        private const val SETTINGS = "settings"
        private const val LANG = "lang:"
        /** 語言模式選單的標籤（官方：嘸 无 台 日）。 */
        private val LANGUAGE_LABELS = mapOf(
            Language.TRADITIONAL to "嘸",
            Language.SIMPLIFIED to "无",
            Language.TW_SIMPLIFIED to "台",
            Language.JAPANESE to "日",
        )

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
        /** 中文模式「.'[]」鍵長按：其餘的字根 ' [ ]。 */
        private val ROOT_POPUP = listOf("'", "[", "]").map { PopupDef(it, it) }
        /** 中文模式閒置時候選列的常用標點（官方手冊截圖）。 */
        private val PUNCTUATION = listOf("!", "?", ",", "\"", ":", "(", ")", "-")

        /** 各層共用的最下排：中英鍵 ?123（或 ABC） 逗號鍵 空白 句點鍵 Enter。 */
        private fun bottomRow(comma: KeyDef, period: KeyDef) = listOf(
            KeyDef("toggle_english", "中", 1.3f, function = true),
            KeyDef("symbols", "?123", 1.2f, function = true),
            comma,
            KeyDef("space", "␣", 4f, char = ' ', function = true, icon = R.drawable.liukai_ic_space),
            period,
            KeyDef("enter", "↵", 1.5f, function = true),
        )

        private val COMMA = KeyDef(",", ",", popup = COMMA_POPUP, function = true)
        private val PERIOD = KeyDef(".", ".", popup = PERIOD_POPUP, function = true)
        /** 中文模式的句點鍵：點按為字根「.」，長按彈出 ' [ ]。 */
        private val ROOT_PERIOD = KeyDef(".", ".'[]", popup = ROOT_POPUP, char = '.', function = true)

        private val SHIFT = KeyDef("shift", "⇧", 1.5f, function = true, icon = R.drawable.liukai_ic_shift)
        private val HOMOPHONE = KeyDef("homophone", "同音", 1.5f, function = true)
        private val BACKSPACE = KeyDef("backspace", "⌫", 1.5f, function = true, icon = R.drawable.liukai_ic_backspace)
        private val ALT = KeyDef("alt", "ALT", 1.5f, function = true)

        private val TOP_LETTERS = "qwertyuiop".mapIndexed { i, c -> KeyDef(c.toString(), c.toString(), hint = "1234567890"[i]) }
        /** 設定「顯示數字鍵」時字母層最上方的數字列。 */
        private val NUMBER_ROW = chars("1234567890")
        /** 退出鍵盤鍵（設定「顯示退出鍵盤鍵」時放在字母層最下排最左邊）。 */
        private val HIDE = KeyDef("hide", "⌨", 1.1f, function = true, icon = R.drawable.liukai_ic_keyboard_hide)
        /** 最下排加退出鍵後各鍵的比例（量測官方截圖）。 */
        private val DONE_ROW_WEIGHTS = mapOf(
            "toggle_english" to 1.1f, "symbols" to 1.1f, "," to 1f, "space" to 3.2f, "." to 1f, "enter" to 1.5f,
        )
        /** 英文字母長按的重音字母（官方實測）。 */
        private val ACCENTS = mapOf('a' to "àáâãäåæ", 's' to "§ß", 'c' to "ç", 'n' to "ñ")

        private val LETTER_ROWS = listOf(
            TOP_LETTERS,
            "asdfghjkl".map { KeyDef(it.toString(), it.toString(), accents = ACCENTS[it]) },
            listOf(SHIFT) + "zxcvbnm".map { KeyDef(it.toString(), it.toString(), accents = ACCENTS[it]) } + BACKSPACE,
            bottomRow(COMMA, PERIOD),
        )

        private val CHINESE_ROWS = listOf(
            TOP_LETTERS,
            chars("asdfghjkl"),
            listOf(HOMOPHONE) + chars("zxcvbnm") + BACKSPACE,
            bottomRow(COMMA, ROOT_PERIOD),
        )

        private val SYMBOL_KEYS = listOf(
            chars("1234567890"),
            chars("@#$%&*-+()"),
            listOf(ALT) + chars("!\"':;/?") + BACKSPACE,
        )
        private val SYMBOL_ROWS = SYMBOL_KEYS + listOf(bottomRow(COMMA, PERIOD))
        private val CHINESE_SYMBOL_ROWS = SYMBOL_KEYS + listOf(bottomRow(COMMA, ROOT_PERIOD))

        private val ALT_ROWS = listOf(
            chars("~`|•√π÷×{}"),
            listOf(KeyDef("tab", "⇥", char = '\t')) + chars("£¢€°^_=[]"),
            listOf(ALT) + chars("™®©¶\\<>") + BACKSPACE,
            bottomRow(KeyDef("„", "„", function = true), KeyDef("…", "…", function = true)),
        )
    }
}
