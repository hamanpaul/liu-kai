package com.hamanpaul.liukai.core.ime

/** 判斷「單按 Shift」：Shift 按下到放開之間沒有按其他鍵。 */
class ShiftTapDetector {
    private var down = false
    private var usedWithOtherKey = false

    fun onKeyDown(keyCode: Int, repeatCount: Int) {
        if (!isShift(keyCode)) {
            usedWithOtherKey = true
        } else if (repeatCount == 0) {
            down = true
            usedWithOtherKey = false
        }
    }

    /** 放開 Shift 且構成單按時回傳 true。 */
    fun onKeyUp(keyCode: Int): Boolean {
        if (!isShift(keyCode)) return false
        val single = down && !usedWithOtherKey
        down = false
        return single
    }

    companion object {
        const val KEYCODE_SHIFT_LEFT = 59
        const val KEYCODE_SHIFT_RIGHT = 60

        fun isShift(keyCode: Int): Boolean = keyCode == KEYCODE_SHIFT_LEFT || keyCode == KEYCODE_SHIFT_RIGHT
    }
}
