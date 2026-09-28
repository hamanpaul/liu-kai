package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.engine.ImeEvent
import com.hamanpaul.liukai.core.engine.EngineEvent
import kotlin.test.Test
import kotlin.test.assertEquals

class HardwareKeysTest {
    // Android KeyEvent 常數（以字面值驗證對應，避免測試與實作共用同一份常數而失去意義）
    private val space = 62
    private val enter = 66
    private val numpadEnter = 160
    private val del = 67
    private val escape = 111
    private val pageUp = 92
    private val pageDown = 93
    private val keyJ = 38
    private val keyA = 29

    private fun t(keyCode: Int, unicode: Int = 0, ctrl: Boolean = false, alt: Boolean = false, meta: Boolean = false) =
        HardwareKeys.translate(keyCode, unicode, ctrl, alt, meta)

    @Test
    fun `功能鍵對應引擎事件`() {
        assertEquals(KeyAction.Engine(ImeEvent.Space), t(space, ' '.code))
        assertEquals(KeyAction.Engine(ImeEvent.Enter), t(enter))
        assertEquals(KeyAction.Engine(ImeEvent.Enter), t(numpadEnter))
        assertEquals(KeyAction.Engine(ImeEvent.Backspace), t(del))
        assertEquals(KeyAction.Engine(ImeEvent.Escape), t(escape))
        assertEquals(KeyAction.Engine(ImeEvent.PageDown), t(pageDown))
        assertEquals(KeyAction.Engine(ImeEvent.PageUp), t(pageUp))
    }

    @Test
    fun `可見字元鍵轉成 Key 事件`() {
        assertEquals(KeyAction.Engine(ImeEvent.Key('a')), t(keyA, 'a'.code))
        assertEquals(KeyAction.Engine(ImeEvent.Key('?')), t(76, '?'.code))
    }

    @Test
    fun `修飾鍵組合一律放行（沒有 Ctrl+J 日文模式）`() {
        assertEquals(KeyAction.PassThrough, t(keyJ, 'j'.code, ctrl = true))
        assertEquals(KeyAction.PassThrough, t(keyA, 'a'.code, ctrl = true))
        assertEquals(KeyAction.PassThrough, t(keyA, 'a'.code, alt = true))
        assertEquals(KeyAction.PassThrough, t(keyA, 'a'.code, meta = true))
        assertEquals(KeyAction.PassThrough, t(space, ' '.code, ctrl = true))
    }

    @Test
    fun `沒有字元或是 dead key 時放行`() {
        assertEquals(KeyAction.PassThrough, t(keyA, 0))
        assertEquals(KeyAction.PassThrough, t(keyA, 0x80000000.toInt() or '`'.code))
    }
}
