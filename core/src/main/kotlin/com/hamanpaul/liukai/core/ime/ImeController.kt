package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.engine.ImeEvent
import com.hamanpaul.liukai.core.engine.EngineEvent
import com.hamanpaul.liukai.core.engine.LiuEngine

/**
 * 輸入法的平台無關控制層：接收實體鍵、軟鍵盤、候選列與游標事件，
 * 回傳要對 App 執行的 InputConnection 操作。Android 服務只負責轉送事件與執行操作。
 */
class ImeController(val engine: LiuEngine) {
    var tableLoaded: Boolean = false
    private var passwordField = false
    private val tracker = CompositionTracker()
    private val shift = ShiftTapDetector()
    /** keyDown 被消耗的鍵，其 keyUp 也要攔下，避免 App 收到不成對的事件。 */
    private val consumedKeys = HashSet<Int>()
    /** keyDown 經 InputConnection 轉送的鍵，其 keyUp 也要轉送。 */
    private val forwardedKeys = HashSet<Int>()

    val imeActive: Boolean get() = tableLoaded && !passwordField

    /** 目前正在組字，需要顯示候選列。 */
    val needsCandidates: Boolean get() = engine.isComposing

    fun startInput(inputType: Int) {
        engine.reset()
        tracker.reset()
        consumedKeys.clear()
        forwardedKeys.clear()
        passwordField = EditorPolicy.isPassword(inputType)
    }

    fun finishInput() {
        engine.reset()
        tracker.reset()
    }

    fun keyDown(keyCode: Int, repeatCount: Int, unicodeChar: Int, ctrl: Boolean, alt: Boolean, meta: Boolean): ImeOutcome {
        shift.onKeyDown(keyCode, repeatCount)
        if (ShiftTapDetector.isShift(keyCode) || !imeActive) return PASS
        val action = HardwareKeys.translate(keyCode, unicodeChar, ctrl, alt, meta)
        if (action !is KeyAction.Engine) return PASS
        val result = engine.handle(action.event)
        val ops = tracker.sync(result, engine)
        if (result.consumed) {
            consumedKeys += keyCode
            return ImeOutcome(true, ops)
        }
        // 引擎不處理的鍵：經 InputConnection 轉送，確保排在先前的上屏操作之後
        forwardedKeys += keyCode
        return ImeOutcome(true, ops + IcOp.ForwardKey)
    }

    fun keyUp(keyCode: Int): ImeOutcome {
        if (shift.onKeyUp(keyCode) && imeActive) {
            return ImeOutcome(false, tracker.sync(engine.handle(EngineEvent.ToggleEnglish), engine))
        }
        return when {
            consumedKeys.remove(keyCode) -> ImeOutcome(true, emptyList())
            forwardedKeys.remove(keyCode) -> ImeOutcome(true, listOf(IcOp.ForwardKey))
            else -> PASS
        }
    }

    fun softKey(key: SoftKey, imeOptions: Int, inputType: Int): List<IcOp> {
        val event = when (key) {
            is SoftKey.Text -> ImeEvent.Key(key.char)
            SoftKey.Backspace -> ImeEvent.Backspace
            SoftKey.Space -> ImeEvent.Space
            SoftKey.Enter -> ImeEvent.Enter
            SoftKey.ToggleEnglish -> return tracker.sync(engine.handle(EngineEvent.ToggleEnglish), engine)
        }
        if (!imeActive) return passThrough(key, imeOptions, inputType)
        val result = engine.handle(event)
        val ops = tracker.sync(result, engine)
        return if (result.consumed) ops else ops + passThrough(key, imeOptions, inputType)
    }

    fun tapCandidate(index: Int): List<IcOp> = tracker.sync(engine.handle(ImeEvent.Select(index)), engine)

    fun selectionMoved(newSelStart: Int, newSelEnd: Int, candidatesStart: Int, candidatesEnd: Int): List<IcOp> {
        if (!tracker.selectionMoved(newSelStart, newSelEnd, candidatesStart, candidatesEnd)) return emptyList()
        engine.reset()
        return listOf(IcOp.FinishComposing)
    }

    /** 軟鍵盤沒有對應的 App 端 KeyEvent，未被引擎消耗的鍵由此直接送出。 */
    private fun passThrough(key: SoftKey, imeOptions: Int, inputType: Int): List<IcOp> = listOf(
        when (key) {
            is SoftKey.Text -> IcOp.Commit(key.char.toString())
            SoftKey.Space -> IcOp.Commit(" ")
            SoftKey.Backspace -> IcOp.SendKey(HardwareKeys.KEYCODE_DEL)
            else -> when (val enter = EditorPolicy.enterAction(imeOptions, inputType)) {
                is EnterAction.EditorAction -> IcOp.EditorAction(enter.actionId)
                EnterAction.NewLine -> IcOp.SendKey(HardwareKeys.KEYCODE_ENTER)
            }
        },
    )
}

private val PASS = ImeOutcome(false, emptyList())
