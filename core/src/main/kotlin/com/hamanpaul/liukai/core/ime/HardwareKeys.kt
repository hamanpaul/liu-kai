package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.engine.ImeEvent

/** 實體鍵（Android KeyEvent）轉引擎事件。常數與 android.view.KeyEvent 相同。 */
object HardwareKeys {
    const val KEYCODE_SPACE = 62
    const val KEYCODE_ENTER = 66
    const val KEYCODE_DEL = 67
    const val KEYCODE_PAGE_UP = 92
    const val KEYCODE_PAGE_DOWN = 93
    const val KEYCODE_ESCAPE = 111
    const val KEYCODE_NUMPAD_ENTER = 160

    /** KeyCharacterMap.COMBINING_ACCENT：dead key 的 unicode 旗標。 */
    private const val COMBINING_ACCENT = 0x80000000.toInt()

    private val FUNCTION_KEYS = mapOf(
        KEYCODE_SPACE to ImeEvent.Space,
        KEYCODE_ENTER to ImeEvent.Enter,
        KEYCODE_NUMPAD_ENTER to ImeEvent.Enter,
        KEYCODE_DEL to ImeEvent.Backspace,
        KEYCODE_ESCAPE to ImeEvent.Escape,
        KEYCODE_PAGE_DOWN to ImeEvent.PageDown,
        KEYCODE_PAGE_UP to ImeEvent.PageUp,
    )

    fun translate(keyCode: Int, unicodeChar: Int, ctrl: Boolean, alt: Boolean, meta: Boolean): KeyAction = when {
        ctrl || alt || meta -> KeyAction.PassThrough
        keyCode in FUNCTION_KEYS -> KeyAction.Engine(FUNCTION_KEYS.getValue(keyCode))
        unicodeChar == 0 || unicodeChar and COMBINING_ACCENT != 0 -> KeyAction.PassThrough
        else -> KeyAction.Engine(ImeEvent.Key(unicodeChar.toChar()))
    }
}
