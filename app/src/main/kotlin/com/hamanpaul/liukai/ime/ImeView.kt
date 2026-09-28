package com.hamanpaul.liukai.ime

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.LayerDrawable
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
import com.hamanpaul.liukai.data.VoiceMode
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.abs

interface ImeActions {
    fun onSoftKey(key: SoftKey)
    fun onCandidateTap(index: Int)
    fun onOpenSettings()
    /** 麥克風鍵、長按「,」／麥克風鍵選單的 🎤、候選列的麥克風：切換到系統的語音輸入。 */
    fun onVoice()
    /** 閒置時候選列的常用標點（選用設定）：直接上屏。 */
    fun onPunctuation(text: String)
    /** 中文模式左右滑動空白鍵切換的語言模式。 */
    fun onLanguage(language: Language)
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
    /** 目前的語言模式與可切換的語言模式（左右滑動空白鍵依序切換）。 */
    val language: Language,
    val languages: List<Language>,
    /** 同音查碼：從同音字清單上屏的字與字碼，顯示到下一次按鍵。 */
    val codeHint: CodeHint?,
    /** 智慧鍵盤：組字中可接的下一碼（null 為不限制）。 */
    val nextKeys: Set<Char>?,
    /** 輸入欄有文字（含組字）：中文模式空白鍵的 ⎵ 改為底線（與官方相同）。 */
    val fieldHasText: Boolean,
)

/**
 * 輸入畫面：上方候選列、下方螢幕鍵盤。偵測到實體鍵盤時只顯示候選列。
 *
 * 螢幕鍵盤照使用者手機上的官方非 PRO「嘸蝦米輸入法」2.6.8：
 * - 中文模式字母層：鍵帽大寫（Q–P 附數字提示、長按輸入數字）；同音 Z–M ⌫；En ?123 , 空白 .'[] ↵。
 *   空白鍵標示「◂ 嘸 ▸」，左右滑動依序切換 嘸／无／台／日；輸入欄有文字時 ⎵ 改為橘色底線。
 *   長按「,」彈出 ⚙ 🎤；長按「.'[]」彈出常用標點。
 * - 英文模式字母層：鍵帽小寫；⇧ z–m ⌫；中 ?123 🎤 空白 . Enter（Enter 依欄位動作顯示 Next、Search…）；
 *   麥克風的位置依設定「語音輸入」（主鍵盤、符號鍵盤或關閉，不在的位置為「,」），長按彈出 ⚙ 🎤。
 * - ?123 層與 ALT 層：中文模式最下排為 En 中 , 空白 .'[] ↵（En 切到英文、中 回中文字母層）；
 *   英文模式為 中 ABC , 空白 . Enter。萬用字元 *、反引號 ` 與官方相同放在 ?123／ALT 層，切換鍵盤層不影響組字。
 * - 智慧鍵盤：組字中不能接的鍵留白（不顯示標籤、點了沒有反應），同音鍵留白。
 * 候選列照官方橫式畫面：橘色候選、預設字粗體、灰色分隔線；字碼顯示在輸入欄。官方直式畫面不顯示候選列是官方的
 * 問題，liu-kai 直式、橫式都顯示。候選與按鍵都是獨立的標準 View（含 contentDescription），確保點擊判定可靠、
 * 也可被 UiAutomator 定位。
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
    private var sounds = 0
    /** 智慧鍵盤目前的下一碼（見 [UiState.nextKeys]）。 */
    private var smartKeys: Set<Char>? = null
    private var mode: InputMode = InputMode.CHINESE
    private var tableLoaded = true
    /** 空白鍵顯示的語言模式與底線（輸入欄有文字）。 */
    private var language = Language.TRADITIONAL
    private var fieldHasText = false
    private var enterLabel = "↵"
    private var shownCandidates: List<Candidate> = emptyList()
    private val candidateViews = ArrayList<View>()
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

    /** Enter 鍵標籤依輸入欄的動作（Next、Search…）；會換行的欄位顯示 ↵。中文模式與官方相同一律顯示 ↵。 */
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
        if (state.nextKeys != smartKeys || state.language != language || state.fieldHasText != fieldHasText) {
            smartKeys = state.nextKeys
            language = state.language
            fieldHasText = state.fieldHasText
            rebuildKeyboard()
        }
        // 字碼顯示在輸入欄（與官方相同），候選列左側只顯示同音查詢的字與同音查碼的結果
        composingView.text = when {
            state.homophoneOf != null -> "音:${state.homophoneOf}"
            state.codeHint != null -> hintText(state.codeHint)
            else -> ""
        }
        refreshBar()
        candidateScroll.post { candidateScroll.scrollTo(0, 0) }
    }

    /** 同音查碼的顯示文字，例如「忠 qa」（多個字碼以／分隔）。 */
    fun hintText(hint: CodeHint): String = "${hint.text} ${hint.codes.joinToString("／")}"

    /**
     * 重建候選列：有候選時列出候選（每個候選後面一條分隔線）；設定「閒置時顯示常用標點」時，字母層閒置時列出
     * 常用標點（官方 PRO），並在右端顯示麥克風。
     */
    private fun refreshBar() {
        candidateRow.removeAllViews()
        candidateViews.clear()
        punctViews.clear()
        voiceKey.visibility = if (settings.idleStrip) VISIBLE else GONE
        val state = lastState
        shownCandidates = state?.candidates.orEmpty()
        if (state == null) return
        state.candidates.forEachIndexed { index, cand ->
            val view = candidateView(index, cand, state)
            candidateViews += view
            candidateRow.addView(view)
            candidateRow.addView(View(context).apply { setBackgroundColor(pal.separator) }, LayoutParams(dp(0.8f), dp(20f)))
        }
        if (settings.idleStrip && state.candidates.isEmpty() && state.composing.isEmpty() && layer == Layer.LETTERS) {
            PUNCTUATION.forEachIndexed { i, p ->
                if (i > 0) candidateRow.addView(View(context).apply { setBackgroundColor(pal.separator) }, LayoutParams(dp(1f), dp(20f)))
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
        // 探測點：候選列左上角內縮 20px（避開組字失敗的紅框；橫式時加上視窗的左偏移），畫面上一定是候選列底色；
        // 測試以截圖確認鍵盤真的畫在螢幕上
        json.put("probe", JSONObject().put("x", bounds(bar).getInt("x") + PROBE_INSET).put("y", row.getInt("y") + PROBE_INSET).put("color", hex(pal.bar)))
        val candidates = JSONArray()
        shownCandidates.forEachIndexed { i, cand ->
            val view = candidateViews[i] as TextView
            candidates.put(
                bounds(view).put("index", i).put("text", cand.text).put("annotation", cand.annotation ?: JSONObject.NULL)
                    .put("bold", view.typeface == Typeface.DEFAULT_BOLD).put("color", hex(view.currentTextColor)),
            )
        }
        json.put("candidates", candidates)
        val keys = JSONObject()
        for ((id, view) in keyViews) keys.put(id, bounds(view))
        for ((id, view) in popupViews) keys.put(id, bounds(view))
        for ((id, view) in punctViews) keys.put(id, bounds(view))
        if (voiceKey.visibility == VISIBLE) keys.put("voice", bounds(voiceKey))
        json.put("keys", keys)
        json.put("layer", layer.name.lowercase())
        json.put("rows", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map { it.id }) }))
        // 各鍵顯示的標籤：以圖示顯示的鍵（⇧ ⌫ ␣ ↵ 🎤）為其代表字元，中文模式空白鍵為語言模式，留白的鍵為空字串
        json.put("rowLabels", JSONArray(rowsFor(layer).map { row -> JSONArray(row.map(::shownLabel)) }))
        json.put("strip", JSONArray(punctViews.keys.toList()))
        json.put("enterLabel", shownLabel(ENTER))
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
        json.put("blank", JSONArray(rowsFor(layer).flatten().filter(::blank).map { it.id }))
        json.put("spaceUnderline", chinese && fieldHasText)
        json.put("preview", previewKey ?: JSONObject.NULL)
        json.put("feedback", JSONObject().put("vibrate", vibrations).put("sound", sounds))
    }

    /** 診斷輸出用的按鍵標籤（見 describe 的 rowLabels）。 */
    private fun shownLabel(def: KeyDef): String = when {
        blank(def) -> ""
        def.id == "space" -> if (chinese) LANGUAGE_LABELS.getValue(language) else def.label
        iconFor(def) != null -> def.label
        else -> labelFor(def)
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

    /** x 取螢幕座標（橫式時輸入法視窗在螢幕缺口右側，視窗內座標少了視窗的左偏移；視窗不會水平移動），y 見 [describe]。 */
    private fun bounds(v: View): JSONObject {
        val loc = IntArray(2)
        val screen = IntArray(2)
        v.getLocationInWindow(loc)
        v.getLocationOnScreen(screen)
        return JSONObject().put("x", screen[0]).put("y", windowTop + loc[1]).put("w", v.width).put("h", v.height)
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
            setPadding(dp(10f), 0, 0, 0)
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

    /** 候選（量測官方橫式畫面）：橘色 22sp、預設字（空白上屏的字）粗體、左右各 14.5dp。 */
    private fun candidateView(index: Int, cand: Candidate, state: UiState): View {
        val tv = TextView(context)
        val text = SpannableStringBuilder(cand.text)
        cand.annotation?.let {
            val start = text.length
            text.append(" ").append(it)
            text.setSpan(RelativeSizeSpan(0.6f), start, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text.setSpan(ForegroundColorSpan(ACCENT), start, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        tv.text = text
        tv.setTextColor(pal.strip)
        tv.typeface = if (index == state.pageStart) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
        tv.gravity = Gravity.CENTER
        tv.setPadding(dp(14.5f), 0, dp(14.5f), 0)
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
        // 常用標點只在字母層顯示，切換鍵盤層或模式時跟著更新
        refreshBar()
    }

    private fun rowsFor(layer: Layer): List<List<KeyDef>> = when (layer) {
        Layer.LETTERS -> doneRow(
            if (chinese) CHINESE_LETTERS + listOf(bottomRow(comma(), ROOT_PERIOD))
            else ENGLISH_LETTERS + listOf(bottomRow(f1(VoiceMode.MAIN), PERIOD)),
        )
        Layer.SYMBOLS -> SYMBOL_KEYS + listOf(if (chinese) bottomRow(comma(), ROOT_PERIOD) else bottomRow(f1(VoiceMode.SYMBOLS), PERIOD))
        Layer.ALT -> ALT_ROWS
    }

    /** 英文模式最下排「,」的位置：設定的語音輸入位置在這一層時為麥克風鍵，否則為「,」。 */
    private fun f1(voiceHere: VoiceMode): KeyDef = if (settings.voice == voiceHere) MIC else comma()

    /** 「,」鍵：長按彈出 ⚙（設定），語音輸入沒有關閉時再加 🎤。 */
    private fun comma(): KeyDef = if (settings.voice == VoiceMode.OFF) COMMA_NO_VOICE else COMMA

    /** 設定「顯示退出鍵盤鍵」：字母層最下排最左邊加退出鍵（與官方相同，其餘鍵依比例縮窄）。 */
    private fun doneRow(rows: List<List<KeyDef>>): List<List<KeyDef>> =
        if (!settings.doneKey) rows
        else rows.dropLast(1) + listOf(listOf(HIDE) + rows.last().map { it.copy(weight = DONE_ROW_WEIGHTS.getValue(it.id)) })

    private val landscape: Boolean get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    /** 排高（dp）：直式、橫式各依設定的按鍵高度。 */
    private fun rowHeightDp(): Float =
        if (landscape) LANDSCAPE_ROW_DP[settings.landscapeHeight.ordinal] else PORTRAIT_ROW_DP[settings.portraitHeight.ordinal]

    /** 按鍵字體縮放：直式、橫式各依設定的字體大小。 */
    private fun fontScale(): Float =
        FONT_SCALE[(if (landscape) settings.landscapeFont else settings.portraitFont).ordinal]

    /**
     * 智慧鍵盤留白的鍵（照官方）：設定開啟、中文模式、組字中，不能接的字母鍵、「,」與「.'[]」，以及同音鍵。
     * 留白的鍵只剩鍵格，不顯示標籤與數字提示，點了沒有反應。
     */
    private fun blank(def: KeyDef): Boolean {
        val next = smartKeys
        if (!settings.smartKeyboard || !chinese || next == null) return false
        return when {
            def.id == "homophone" -> true
            def.id == "," -> ',' !in next
            def.id == "." -> ROOT_KEYS.none { it in next }
            def.id.length == 1 && def.id[0] in 'a'..'z' -> def.id[0] !in next
            else -> false
        }
    }

    /** 以圖示顯示的鍵；Enter 在中文模式或會換行的欄位顯示 ↵ 圖示（空白鍵另外繪製，見 [spaceContent]）。 */
    private fun iconFor(def: KeyDef): Int? =
        if (def.id == "enter" && (chinese || enterLabel == "↵")) R.drawable.liukai_ic_enter else def.icon

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
        frame.background = keyBackground(def)
        def.popup?.let {
            // 右下角「…」表示可長按彈出（留白時仍顯示，與官方相同）
            frame.addView(
                TextView(context).apply {
                    text = "…"
                    setTextColor(pal.popupMark)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
                    setPadding(0, 0, dp(6f), 0)
                },
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.END),
            )
        }
        // 智慧鍵盤留白：只剩鍵格，點了沒有反應
        if (blank(def)) return frame
        val icon = iconFor(def)
        // 不分格時空白鍵為膠囊、Enter 為圓形（量測官方：膠囊高約 26dp、圓直徑約 36dp）
        if (!pal.cells && def.id == "space") frame.addView(shape(pal.spacePill, false), FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, dp(26f), Gravity.CENTER))
        if (!pal.cells && def.id == "enter") frame.addView(shape(pal.enterCircle, true), FrameLayout.LayoutParams(dp(36f), dp(36f), Gravity.CENTER))
        when {
            def.id == "space" -> spaceContent(frame)
            icon != null -> frame.addView(
                ImageView(context).apply { setImageResource(icon); setColorFilter(if (def.id == "enter") pal.enterText else pal.icon) },
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER),
            )
            else -> {
                val label = TextView(context)
                label.text = labelFor(def)
                label.gravity = Gravity.CENTER
                label.setTextColor(if (def.id == "enter") pal.enterText else pal.text)
                label.typeface = if (label.text.length > 1 || pal.boldLetters) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
                // ?123、ABC、ALT、Next 等功能鍵標籤小一號
                label.setTextSize(TypedValue.COMPLEX_UNIT_SP, (if (label.text.length > 1) 15f else 24f) * fontScale())
                frame.addView(label, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            }
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
        def.accents?.let { accents ->
            // 英文字母長按彈出重音字母（Shift 時為大寫）
            frame.setOnLongClickListener {
                showPopup(frame, accents.map { c -> (if (shifted) c.uppercaseChar() else c).toString().let { PopupDef(it, it) } })
                true
            }
        }
        def.popup?.let { popup ->
            frame.setOnLongClickListener {
                showPopup(frame, popup)
                true
            }
        }
        when (def.id) {
            "backspace" -> attachRepeat(frame)
            "space" -> attachSpace(frame, def)
            else -> {
                frame.setOnClickListener { onKey(def) }
                if (def.id !in NO_PREVIEW) attachPreview(frame, def)
            }
        }
        return frame
    }

    /**
     * 空白鍵的內容（量測官方，以鍵的中心定位，與按鍵高度無關）：中文模式在中心上方 5.7dp 顯示「◂ 嘸 ▸」，
     * 下方 11dp 為 ⎵，輸入欄有文字時 ⎵ 改為帶光暈的底線；英文模式只有 ⎵。
     */
    private fun spaceContent(frame: FrameLayout) {
        if (chinese) {
            val label = TextView(context).apply {
                text = LANGUAGE_LABELS.getValue(language)
                setTextColor(pal.spaceLabel)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f * fontScale())
                compoundDrawablePadding = dp(8f)
                setCompoundDrawablesWithIntrinsicBounds(R.drawable.liukai_ic_arrow_left, 0, R.drawable.liukai_ic_arrow_right, 0)
                compoundDrawableTintList = ColorStateList.valueOf(pal.spaceLabel)
                translationY = -dp(5.7f).toFloat()
            }
            frame.addView(label, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        }
        if (chinese && fieldHasText) {
            // 底線：寬為鍵寬 71%（以 weightSum 分配）、核心 1.5dp，外圍 1.5dp 半透明光暈
            val line = View(context).apply {
                background = LayerDrawable(
                    arrayOf(
                        GradientDrawable().apply { cornerRadius = dp(2.25f).toFloat(); setColor(withAlpha(pal.spaceLine, 0x60)) },
                        GradientDrawable().apply { cornerRadius = dp(0.75f).toFloat(); setColor(pal.spaceLine) },
                    ),
                ).apply { setLayerInset(1, dp(1.5f), dp(1.5f), dp(1.5f), dp(1.5f)) }
            }
            val holder = LinearLayout(context).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                weightSum = 1f
                translationY = dp(12.4f).toFloat()
                addView(line, LayoutParams(0, LayoutParams.MATCH_PARENT, SPACE_LINE_RATIO))
            }
            frame.addView(holder, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, dp(4.5f), Gravity.CENTER))
        } else {
            frame.addView(
                ImageView(context).apply {
                    setImageResource(R.drawable.liukai_ic_space)
                    setColorFilter(pal.icon)
                    translationY = dp(11f).toFloat()
                },
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER),
            )
        }
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
        val icon = if (def.id == "space") R.drawable.liukai_ic_space else iconFor(def)
        preview.text = if (icon == null) labelFor(def) else ""
        preview.icon = icon?.let { context.getDrawable(it)!!.mutate() }
        preview.iconOffset = if (def.id == "space") dp(14f) else 0
        preview.style()
        if (pal.previewAbove) {
            // 經典灰（量測官方）：寬為鍵寬加 2/3 個字母鍵寬（字母鍵即 5/3 鍵寬）、高為鍵高 9/7 的黑框，底邊在鍵頂上方 0.3 鍵高
            val letterWidth = rowsView.width / ROW_WEIGHT.toInt() - dp(5f)
            val w = key.width + letterWidth * 2 / 3
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
     * 空白鍵：點按輸入空白；中文模式左右滑動超過 [LANGUAGE_SWIPE_DP] 依序切換語言模式（往右 嘸→无→台→日→嘸，往左反向），
     * 不輸入空白（照官方）。英文模式滑動仍是輸入空白。按鍵被移除（例如按住時鍵盤重建）時收到 CANCEL，不輸入空白。
     */
    @SuppressLint("ClickableViewAccessibility")
    private fun attachSpace(key: View, def: KeyDef) {
        var startX = 0f
        key.setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.isPressed = true
                    startX = ev.x
                    if (settings.keyPreview) showPreview(v, def)
                }
                MotionEvent.ACTION_UP -> {
                    v.isPressed = false
                    hidePreview()
                    val dx = ev.x - startX
                    if (chinese && abs(dx) >= dp(LANGUAGE_SWIPE_DP)) switchLanguage(if (dx > 0) 1 else -1) else onKey(def)
                }
                MotionEvent.ACTION_CANCEL -> {
                    v.isPressed = false
                    hidePreview()
                }
            }
            true
        }
    }

    /** 切到可用語言模式清單中的下一個（step＝1）或上一個（step＝−1），頭尾相接。 */
    private fun switchLanguage(step: Int) {
        val state = lastState!!
        val langs = state.languages
        actions.onLanguage(langs[(langs.indexOf(state.language) + step + langs.size) % langs.size])
    }

    /**
     * 按鍵標籤。中英鍵與官方相同顯示「要切去的模式」：中文模式各層顯示 En，英文模式顯示「中」；?123／ALT 層的
     * 第二顆鍵回字母層，中文模式標「中」、英文模式標「ABC」。沒有字表時中英鍵顯示「無表」。中文模式與 Shift 時字母鍵帽大寫。
     */
    private fun labelFor(def: KeyDef): String = when (def.id) {
        "toggle_english" -> when {
            !tableLoaded -> "無表"
            mode == InputMode.CHINESE -> "En"
            else -> "中"
        }
        "symbols" -> when {
            layer == Layer.LETTERS -> "?123"
            chinese -> "中"
            else -> "ABC"
        }
        "enter" -> enterLabel
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
            "mic" -> actions.onVoice()
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
            "toggle_english" -> {
                // 切換中英文一律回到字母層（官方：?123／ALT 層的 En、中 都切到另一模式的字母層）
                layer = Layer.LETTERS
                typedSymbol = false
                actions.onSoftKey(SoftKey.ToggleEnglish)
                rebuildKeyboard()
            }
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

    private fun showPopup(anchor: View, popup: List<PopupDef>) {
        popupPanel.removeAllViews()
        popupViews.clear()
        popup.chunked(POPUP_COLUMNS).forEach { chunk ->
            val row = LinearLayout(context)
            row.orientation = HORIZONTAL
            chunk.forEach { p ->
                val cell: View = if (p.icon != null) {
                    ImageView(context).apply {
                        setImageResource(p.icon)
                        setColorFilter(pal.text)
                        scaleType = ImageView.ScaleType.CENTER
                    }
                } else {
                    TextView(context).apply {
                        text = p.label
                        gravity = Gravity.CENTER
                        setTextColor(pal.text)
                        setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
                        typeface = Typeface.DEFAULT_BOLD
                    }
                }
                cell.contentDescription = "key:popup:${p.id}"
                cell.isClickable = true
                cell.background = gradientStates(pal.popupKeyTop, pal.popupKeyBottom, pal.pressedTop, pal.pressedBottom)
                cell.setOnClickListener {
                    dismissPopup()
                    when (p.id) {
                        SETTINGS -> actions.onOpenSettings()
                        VOICE -> actions.onVoice()
                        else -> typeChar(p.label[0])
                    }
                }
                popupViews["popup:${p.id}"] = cell
                row.addView(cell, LayoutParams(dp(40f), dp(56f)).apply { setMargins(dp(1f), dp(1f), dp(1f), dp(1f)) })
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
                // 先排下一次再刪：刪到輸入欄變空時鍵盤會同步重建、送 CANCEL 移除排程，若刪完才排會把自己排回去而停不下來
                repeatHandler.postDelayed(this, 60)
                actions.onSoftKey(SoftKey.Backspace)
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

    /** 彈出鍵盤的一格：icon 不為 null 時以圖示顯示（⚙ 以外的 🎤）。 */
    private data class PopupDef(val id: String, val label: String, val icon: Int? = null)

    /** 按鍵放大預覽：圓角底色（經典灰加灰色邊）與放大的標籤或圖示；底色與邊由 GradientDrawable 繪製。 */
    private inner class PreviewDrawable : GradientDrawable() {
        var text = ""
        var icon: Drawable? = null
        /** 圖示在中心下方的位移（空白鍵的 ⎵ 與按鍵上相同偏下）。 */
        var iconOffset = 0
        private val fg = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textAlign = Paint.Align.CENTER
            typeface = Typeface.DEFAULT_BOLD
            textSize = dp(34f).toFloat()
        }

        /** 依主題設定底色與邊框：經典灰為獨立黑框加灰色邊，其餘主題為半透明長條。 */
        fun style() {
            cornerRadius = dp(4f).toFloat()
            setColor(pal.previewBg)
            setStroke(if (pal.previewAbove) dp(1.5f) else 0, GRAY_PREVIEW_BORDER)
            fg.color = pal.text
            icon?.setTint(pal.text)
        }

        override fun draw(canvas: Canvas) {
            super.draw(canvas)
            val b = bounds
            // 經典灰：置中；其餘主題：在長條上半部（按鍵上方）
            val cy = if (pal.previewAbove) b.exactCenterY() else b.top + b.height() / 4f
            val d = icon
            if (d == null) {
                canvas.drawText(text, b.exactCenterX(), cy - (fg.descent() + fg.ascent()) / 2, fg)
            } else {
                val l = b.centerX() - d.intrinsicWidth / 2
                val t = cy.toInt() + iconOffset - d.intrinsicHeight / 2
                d.setBounds(l, t, l + d.intrinsicWidth, t + d.intrinsicHeight)
                d.draw(canvas)
            }
        }
    }

    /**
     * 按鍵定義；char 為字元鍵送出的字元（Tab 鍵標籤為 ⇥、送出 \t）；function 為功能鍵（深灰底）；
     * icon 為以圖示取代文字標籤的按鍵（Shift、Backspace、麥克風）。
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
        // 主題色見 Palettes；以下為與主題無關的顏色
        /** 經典灰按鍵預覽黑框的灰色邊。 */
        private val GRAY_PREVIEW_BORDER = Color.rgb(0x46, 0x46, 0x46)
        private val ACCENT = Color.rgb(0x4D, 0xB6, 0xAC)
        private val FAILURE = Color.rgb(0xE5, 0x39, 0x35)

        private const val ROW_WEIGHT = 10f
        /**
         * 各段按鍵高度的排高（dp，依 KeyHeight：高、稍高、適中、稍低、低），量測官方（非 PRO 2.6.8 與 PRO 3.0.9 相同）：
         * 直式排距 188／172／157／141／125px，橫式 144／132／120／108／96px（420dpi，1dp＝2.625px）。
         */
        private val PORTRAIT_ROW_DP = floatArrayOf(71.62f, 65.52f, 59.81f, 53.71f, 47.62f)
        private val LANDSCAPE_ROW_DP = floatArrayOf(54.86f, 50.29f, 45.71f, 41.14f, 36.57f)
        /** 各段字體大小相對「適中」的比例（依 FontSize：大、稍大、適中、稍小、小），量測官方字形高度。 */
        private val FONT_SCALE = floatArrayOf(1.2f, 1.09f, 1f, 0.92f, 0.8f)

        /** 中文模式空白鍵左右滑動超過這個距離（dp）切換語言模式。 */
        private const val LANGUAGE_SWIPE_DP = 30f
        /** 中文模式空白鍵底線的寬度相對鍵寬的比例（量測官方 298／419px）。 */
        private const val SPACE_LINE_RATIO = 0.71f
        /** 「.'[]」鍵可輸入的字根。 */
        private const val ROOT_KEYS = ".'[]"
        private const val PROBE_INSET = 20
        private const val POPUP_COLUMNS = 7
        private const val SETTINGS = "settings"
        private const val VOICE = "voice"
        /** 按住不顯示放大預覽的鍵（官方：Shift、切換鍵盤的鍵、退格）。 */
        private val NO_PREVIEW = setOf("shift", "toggle_english", "symbols", "alt", "hide")
        /** 空白鍵上的語言模式標籤（官方：嘸 无 台 日）。 */
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

        /** 長按「.」與中文模式「.'[]」的常用標點（官方實測）。 */
        private val PERIOD_POPUP = listOf(":", "/", "&", "(", ")", "-", "+", ";", "@", "'", "\"", "?", "!", ",").map { PopupDef(it, it) }
        private val SETTINGS_POPUP = PopupDef(SETTINGS, "⚙")
        private val VOICE_POPUP = PopupDef(VOICE, "🎤", R.drawable.liukai_ic_mic)
        /** 閒置時候選列的常用標點（官方 PRO 手冊截圖）。 */
        private val PUNCTUATION = listOf("!", "?", ",", "\"", ":", "(", ")", "-")

        private val ENTER = KeyDef("enter", "↵", 1.5f, function = true)

        /** 各層共用的最下排：中英鍵 ?123（或 中、ABC） 逗號位置 空白 句點鍵 Enter。 */
        private fun bottomRow(comma: KeyDef, period: KeyDef) = listOf(
            KeyDef("toggle_english", "中", 1.3f, function = true),
            KeyDef("symbols", "?123", 1.2f, function = true),
            comma,
            KeyDef("space", "␣", 4f, char = ' ', function = true),
            period,
            ENTER,
        )

        /** 「,」：長按彈出 ⚙ 🎤（語音輸入關閉時只有 ⚙）。 */
        private val COMMA = KeyDef(",", ",", popup = listOf(SETTINGS_POPUP, VOICE_POPUP), function = true)
        private val COMMA_NO_VOICE = KeyDef(",", ",", popup = listOf(SETTINGS_POPUP), function = true)
        /** 麥克風鍵：點按語音輸入，長按彈出 ⚙ 🎤。 */
        private val MIC = KeyDef("mic", "🎤", popup = listOf(SETTINGS_POPUP, VOICE_POPUP), function = true, icon = R.drawable.liukai_ic_mic)
        private val PERIOD = KeyDef(".", ".", popup = PERIOD_POPUP, function = true)
        /** 中文模式的句點鍵：點按為字根「.」，長按彈出常用標點。 */
        private val ROOT_PERIOD = KeyDef(".", ".'[]", popup = PERIOD_POPUP, char = '.', function = true)

        private val SHIFT = KeyDef("shift", "⇧", 1.5f, function = true, icon = R.drawable.liukai_ic_shift)
        private val HOMOPHONE = KeyDef("homophone", "同音", 1.5f, function = true)
        private val BACKSPACE = KeyDef("backspace", "⌫", 1.5f, function = true, icon = R.drawable.liukai_ic_backspace)
        private val ALT = KeyDef("alt", "ALT", 1.5f, function = true)

        private val TOP_LETTERS = "qwertyuiop".mapIndexed { i, c -> KeyDef(c.toString(), c.toString(), hint = "1234567890"[i]) }
        /** 退出鍵盤鍵（設定「顯示退出鍵盤鍵」時放在字母層最下排最左邊）。 */
        private val HIDE = KeyDef("hide", "⌨", 1.1f, function = true, icon = R.drawable.liukai_ic_keyboard_hide)
        /** 最下排加退出鍵後各鍵的比例（量測官方截圖）。 */
        private val DONE_ROW_WEIGHTS = mapOf(
            "toggle_english" to 1.1f, "symbols" to 1.1f, "," to 1f, "mic" to 1f, "space" to 3.2f, "." to 1f, "enter" to 1.5f,
        )
        /** 英文字母長按的重音字母（官方實測）。 */
        private val ACCENTS = mapOf('a' to "àáâãäåæ", 's' to "§ß", 'c' to "ç", 'n' to "ñ")

        /** 英文字母層的前三排。 */
        private val ENGLISH_LETTERS = listOf(
            TOP_LETTERS,
            "asdfghjkl".map { KeyDef(it.toString(), it.toString(), accents = ACCENTS[it]) },
            listOf(SHIFT) + "zxcvbnm".map { KeyDef(it.toString(), it.toString(), accents = ACCENTS[it]) } + BACKSPACE,
        )

        private val CHINESE_LETTERS = listOf(
            TOP_LETTERS,
            chars("asdfghjkl"),
            listOf(HOMOPHONE) + chars("zxcvbnm") + BACKSPACE,
        )

        private val SYMBOL_KEYS = listOf(
            chars("1234567890"),
            chars("@#$%&*-+()"),
            listOf(ALT) + chars("!\"':;/?") + BACKSPACE,
        )

        private val ALT_ROWS = listOf(
            chars("~`|•√π÷×{}"),
            listOf(KeyDef("tab", "⇥", char = '\t')) + chars("£¢€°^_=[]"),
            listOf(ALT) + chars("™®©¶\\<>") + BACKSPACE,
            bottomRow(KeyDef("„", "„", function = true), KeyDef("…", "…", function = true)),
        )
    }
}
