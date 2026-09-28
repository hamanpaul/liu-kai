package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.engine.EngineEvent

/** 實體鍵的處理方式：交給引擎，或放行給 App。 */
sealed interface KeyAction {
    data class Engine(val event: EngineEvent) : KeyAction
    data object PassThrough : KeyAction
}

/** Enter 的處理方式：觸發欄位動作（搜尋、送出…），或送出換行鍵。 */
sealed interface EnterAction {
    data class EditorAction(val actionId: Int) : EnterAction
    data object NewLine : EnterAction
}

/** 軟鍵盤送出的按鍵。 */
sealed interface SoftKey {
    data class Text(val char: Char) : SoftKey
    data object Backspace : SoftKey
    data object Space : SoftKey
    data object Enter : SoftKey
    data object ToggleEnglish : SoftKey
    /** 中文模式的「同音」鍵。 */
    data object Homophone : SoftKey
}

/** 要對 App 的 InputConnection 執行的操作，依序執行。 */
sealed interface IcOp {
    data class Commit(val text: String) : IcOp
    data class SetComposing(val text: String) : IcOp
    /** 移除畫面上的組字文字（setComposingText("") 後 finishComposingText）。 */
    data object ClearComposing : IcOp
    /** 保留組字文字並結束組字（finishComposingText）。 */
    data object FinishComposing : IcOp
    data class SendKey(val keyCode: Int) : IcOp
    data class EditorAction(val actionId: Int) : IcOp
    /**
     * 把目前的實體鍵事件（Enter、Backspace 等控制鍵）經 InputConnection 轉送給 App。
     * 放行的可見字元不用這個操作，改以 Commit 上屏，避免與組字、上屏操作亂序。
     */
    data object ForwardKey : IcOp
}

/** 實體鍵事件的處理結果：consumed=false 表示還要交給 App。 */
data class ImeOutcome(val consumed: Boolean, val ops: List<IcOp>)
