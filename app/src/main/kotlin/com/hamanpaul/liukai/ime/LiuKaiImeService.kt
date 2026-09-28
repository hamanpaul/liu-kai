package com.hamanpaul.liukai.ime

import android.content.res.Configuration
import android.inputmethodservice.InputMethodService
import android.provider.Settings
import android.text.InputType
import android.util.Log
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import com.hamanpaul.liukai.core.engine.EngineEvent
import com.hamanpaul.liukai.core.engine.EngineResult
import com.hamanpaul.liukai.core.engine.LiuEngine
import com.hamanpaul.liukai.data.TableStore

/**
 * liu-kai 輸入法服務：把實體鍵盤 KeyEvent 與軟鍵盤觸控轉成引擎事件，
 * 再把引擎結果轉成 InputConnection 的 commitText／setComposingText。
 */
class LiuKaiImeService : InputMethodService(), ImeActions {
    private val engine = LiuEngine(null)
    private var view: ImeView? = null
    private var tableLoaded = false
    private var passwordField = false

    /** 目前 App 欄位內是否有 liu-kai 設定的組字區。 */
    private var composingShown = false

    /** keyDown 被輸入法消耗的鍵，其 keyUp 也要攔下，避免 App 收到不成對的事件。 */
    private val consumedKeys = HashSet<Int>()

    /** 單按 Shift 切換中英：Shift 按下期間若有其他鍵，就不算單按。 */
    private var shiftDown = false
    private var shiftUsedWithOtherKey = false

    override fun onCreate() {
        super.onCreate()
        reloadTables()
    }

    private fun reloadTables() {
        val loaded = runCatching { TableStore.load(this) }
            .onFailure { Log.e(TAG, "字表載入失敗", it) }
            .getOrNull()
        tableLoaded = loaded != null
        engine.setTables(loaded?.traditional, loaded?.japanese, loaded?.readings ?: com.hamanpaul.liukai.core.reading.Readings.EMPTY)
    }

    override fun onCreateInputView(): View = ImeView(this, this).also { view = it }

    /** 實體鍵盤模式也顯示輸入畫面（只有候選列），讓候選可以被點選。 */
    override fun onEvaluateInputViewShown(): Boolean {
        super.onEvaluateInputViewShown()
        return true
    }

    /**
     * 預設實作在有實體鍵盤時會拒絕 App 的隱含顯示請求（點輸入欄），導致候選列不出現；
     * liu-kai 需要候選列，一律接受。
     */
    override fun onShowInputRequested(flags: Int, configChange: Boolean): Boolean = true

    override fun onStartInput(attribute: EditorInfo?, restarting: Boolean) {
        super.onStartInput(attribute, restarting)
        if (!restarting) {
            reloadTables()
        }
        engine.reset()
        composingShown = false
        passwordField = attribute != null && isPassword(attribute.inputType)
        consumedKeys.clear()
    }

    override fun onStartInputView(info: EditorInfo?, restarting: Boolean) {
        super.onStartInputView(info, restarting)
        view?.setKeyboardVisible(shouldShowSoftKeyboard())
        render()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        view?.setKeyboardVisible(shouldShowSoftKeyboard())
    }

    override fun onFinishInput() {
        super.onFinishInput()
        engine.reset()
        composingShown = false
    }

    /** 組字中游標被移到組字區以外（例如點擊文字其他位置）時結束組字。 */
    override fun onUpdateSelection(
        oldSelStart: Int, oldSelEnd: Int, newSelStart: Int, newSelEnd: Int,
        candidatesStart: Int, candidatesEnd: Int,
    ) {
        super.onUpdateSelection(oldSelStart, oldSelEnd, newSelStart, newSelEnd, candidatesStart, candidatesEnd)
        if (!composingShown || candidatesStart < 0) return
        if (newSelStart != candidatesEnd || newSelEnd != candidatesEnd) {
            engine.reset()
            currentInputConnection?.finishComposingText()
            composingShown = false
            render()
        }
    }

    private fun shouldShowSoftKeyboard(): Boolean {
        val config = resources.configuration
        val hardKeyboard = config.keyboard != Configuration.KEYBOARD_NOKEYS &&
            config.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_NO
        val showWithHard = Settings.Secure.getInt(contentResolver, "show_ime_with_hard_keyboard", 0) == 1
        return !hardKeyboard || showWithHard
    }

    private fun isPassword(inputType: Int): Boolean {
        val cls = inputType and InputType.TYPE_MASK_CLASS
        val variation = inputType and InputType.TYPE_MASK_VARIATION
        return (cls == InputType.TYPE_CLASS_TEXT && variation in setOf(
            InputType.TYPE_TEXT_VARIATION_PASSWORD,
            InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD,
            InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD,
        )) || (cls == InputType.TYPE_CLASS_NUMBER && variation == InputType.TYPE_NUMBER_VARIATION_PASSWORD)
    }

    private val imeActive: Boolean get() = !passwordField && tableLoaded

    // ---- 實體鍵盤 ----

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_SHIFT_LEFT || keyCode == KeyEvent.KEYCODE_SHIFT_RIGHT) {
            if (event.repeatCount == 0) {
                shiftDown = true
                shiftUsedWithOtherKey = false
            }
            return super.onKeyDown(keyCode, event)
        }
        if (shiftDown) shiftUsedWithOtherKey = true
        if (!imeActive) return super.onKeyDown(keyCode, event)

        if (event.isCtrlPressed && keyCode == KeyEvent.KEYCODE_J) {
            return consume(keyCode, apply(engine.handle(EngineEvent.ToggleJapanese)))
        }
        if (event.isCtrlPressed || event.isAltPressed || event.isMetaPressed) return super.onKeyDown(keyCode, event)

        val engineEvent = when (keyCode) {
            KeyEvent.KEYCODE_SPACE -> EngineEvent.Space
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> EngineEvent.Enter
            KeyEvent.KEYCODE_DEL -> EngineEvent.Backspace
            KeyEvent.KEYCODE_ESCAPE -> EngineEvent.Escape
            KeyEvent.KEYCODE_PAGE_DOWN -> EngineEvent.PageDown
            KeyEvent.KEYCODE_PAGE_UP -> EngineEvent.PageUp
            else -> {
                val unicode = event.getUnicodeChar(event.metaState)
                if (unicode == 0 || unicode and KeyCharacterMap.COMBINING_ACCENT != 0) {
                    return super.onKeyDown(keyCode, event)
                }
                EngineEvent.Key(unicode.toChar())
            }
        }
        val result = engine.handle(engineEvent)
        return if (apply(result)) consume(keyCode, true) else super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_SHIFT_LEFT || keyCode == KeyEvent.KEYCODE_SHIFT_RIGHT) {
            val single = shiftDown && !shiftUsedWithOtherKey
            shiftDown = false
            if (single && imeActive) {
                apply(engine.handle(EngineEvent.ToggleEnglish))
            }
            return super.onKeyUp(keyCode, event)
        }
        if (consumedKeys.remove(keyCode)) return true
        return super.onKeyUp(keyCode, event)
    }

    private fun consume(keyCode: Int, consumed: Boolean): Boolean {
        if (consumed) consumedKeys += keyCode
        return consumed
    }

    // ---- 軟鍵盤與候選列 ----

    override fun onSoftKey(key: SoftKey) {
        when (key) {
            SoftKey.ToggleEnglish -> { apply(engine.handle(EngineEvent.ToggleEnglish)); return }
            SoftKey.ToggleJapanese -> { apply(engine.handle(EngineEvent.ToggleJapanese)); return }
            else -> Unit
        }
        if (!imeActive) {
            passThroughSoft(key)
            return
        }
        val event = when (key) {
            is SoftKey.Text -> EngineEvent.Key(key.char)
            SoftKey.Backspace -> EngineEvent.Backspace
            SoftKey.Space -> EngineEvent.Space
            SoftKey.Enter -> EngineEvent.Enter
            else -> return
        }
        if (!apply(engine.handle(event))) passThroughSoft(key)
    }

    override fun onCandidateTap(index: Int) {
        apply(engine.handle(EngineEvent.Select(index)))
    }

    override fun onCandidateLongPress(index: Int) {
        apply(engine.handle(EngineEvent.Homophone(index)))
    }

    /** 軟鍵盤沒有對應的 App 端 KeyEvent，未被引擎消耗的鍵由這裡直接送出。 */
    private fun passThroughSoft(key: SoftKey) {
        val ic = currentInputConnection ?: return
        when (key) {
            is SoftKey.Text -> ic.commitText(key.char.toString(), 1)
            SoftKey.Space -> ic.commitText(" ", 1)
            SoftKey.Backspace -> sendDownUpKeyEvents(KeyEvent.KEYCODE_DEL)
            SoftKey.Enter -> sendEnter()
            else -> Unit
        }
    }

    private fun sendEnter() {
        val info = currentInputEditorInfo
        val action = info?.imeOptions?.and(EditorInfo.IME_MASK_ACTION) ?: EditorInfo.IME_ACTION_NONE
        val noEnterAction = info != null && info.imeOptions and EditorInfo.IME_FLAG_NO_ENTER_ACTION != 0
        val multiLine = info != null && info.inputType and InputType.TYPE_TEXT_FLAG_MULTI_LINE != 0
        if (!noEnterAction && !multiLine && action != EditorInfo.IME_ACTION_NONE && action != EditorInfo.IME_ACTION_UNSPECIFIED) {
            currentInputConnection?.performEditorAction(action)
        } else {
            sendDownUpKeyEvents(KeyEvent.KEYCODE_ENTER)
        }
    }

    // ---- 套用引擎結果 ----

    /** 上屏與更新組字、候選；回傳引擎是否消耗了這個按鍵。 */
    private fun apply(result: EngineResult): Boolean {
        val ic = currentInputConnection
        if (ic != null) {
            ic.beginBatchEdit()
            result.commit?.let {
                ic.commitText(it, 1)
                composingShown = false
            }
            if (engine.isComposing) {
                ic.setComposingText(engine.composing.ifEmpty { engine.homophoneOf ?: "" }, 1)
                composingShown = true
            } else if (composingShown) {
                // 組字被清空（Esc、刪光字根）：移除畫面上的組字文字，而不是把它定案留在欄位裡。
                ic.setComposingText("", 1)
                ic.finishComposingText()
                composingShown = false
            }
            ic.endBatchEdit()
        }
        // 實體鍵盤直接打字時輸入畫面可能尚未顯示；開始組字就主動顯示候選列。
        if (engine.isComposing && !isInputViewShown) requestShowSelf(0)
        render()
        return result.consumed
    }

    private fun render() {
        view?.render(
            UiState(
                mode = engine.mode,
                composing = engine.composing,
                candidates = engine.candidates,
                pageStart = engine.pageStart,
                pageSize = engine.config.pageSize,
                homophoneOf = engine.homophoneOf,
                tableLoaded = tableLoaded,
            ),
        )
    }

    companion object {
        private const val TAG = "LiuKaiIme"
    }
}
