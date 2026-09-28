package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.engine.EngineResult
import com.hamanpaul.liukai.core.engine.LiuEngine

/**
 * 追蹤 App 欄位內是否有輸入法設定的組字區，並把引擎結果轉成 InputConnection 操作。
 * 只有確實有組字區時才清除，避免誤刪使用者選取的文字。組字區的字碼照官方以大寫顯示（例如「SA」）。
 */
class CompositionTracker {
    var shown: Boolean = false
        private set

    fun sync(result: EngineResult, engine: LiuEngine): List<IcOp> {
        val ops = ArrayList<IcOp>()
        result.commit?.let {
            ops += IcOp.Commit(it)
            shown = false
        }
        if (engine.isComposing) {
            ops += IcOp.SetComposing(engine.displayComposing.uppercase())
            shown = true
        } else if (shown) {
            ops += IcOp.ClearComposing
            shown = false
        }
        return ops
    }

    /** 游標被移出組字區（例如點擊文字其他位置）時回傳 true，並視為組字區已結束。 */
    fun selectionMoved(newSelStart: Int, newSelEnd: Int, candidatesStart: Int, candidatesEnd: Int): Boolean {
        if (!shown || candidatesStart < 0) return false
        if (newSelStart == candidatesEnd && newSelEnd == candidatesEnd) return false
        shown = false
        return true
    }

    fun reset() {
        shown = false
    }
}
