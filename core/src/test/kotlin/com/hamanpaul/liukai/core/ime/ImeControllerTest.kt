package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.Fixtures
import com.hamanpaul.liukai.core.engine.InputMode
import com.hamanpaul.liukai.core.engine.LiuEngine
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ImeControllerTest {
    private val keyA = 29
    private val keyB = 30
    private val keyJ = 38
    private val space = 62
    private val shiftLeft = 59
    private val del = 67
    private val enter = 66
    private val classText = 0x1
    private val textPassword = 0x80
    private val multiLine = 0x20000
    private val actionSearch = 3

    private val c = ImeController(Fixtures.engine()).apply {
        tableLoaded = true
        startInput(classText)
    }

    private fun down(keyCode: Int, ch: Char = 0.toChar(), ctrl: Boolean = false, repeat: Int = 0) =
        c.keyDown(keyCode, repeat, ch.code, ctrl, alt = false, meta = false)

    @Test
    fun `服務端讀取結果欄位：consumed、ops 與各操作的內容`() {
        assertTrue(c.tableLoaded)
        val typed = down(keyB, 'b')
        assertTrue(typed.consumed)
        assertEquals("b", (typed.ops.single() as IcOp.SetComposing).text)
        assertEquals("木", (down(space, ' ').ops.single() as IcOp.Commit).text)
        assertEquals(del, (c.softKey(SoftKey.Backspace, 0, classText).single() as IcOp.SendKey).keyCode)
        assertEquals(actionSearch, (c.softKey(SoftKey.Enter, actionSearch, classText).single() as IcOp.EditorAction).actionId)
    }

    @Test
    fun `實體鍵盤打字：組字、空白上屏，並攔下對應的 keyUp`() {
        assertEquals(ImeOutcome(true, listOf(IcOp.SetComposing("b"))), down(keyB, 'b'))
        assertTrue(c.needsCandidates)
        assertEquals(ImeOutcome(true, emptyList()), c.keyUp(keyB))
        assertEquals(ImeOutcome(true, listOf(IcOp.Commit("木"))), down(space, ' '))
        assertEquals(ImeOutcome(true, emptyList()), c.keyUp(space))
        assertFalse(c.needsCandidates)
    }

    @Test
    fun `引擎放行的可見字元直接上屏（與組字、上屏同走有序的文字通道），keyUp 攔下`() {
        assertEquals(ImeOutcome(true, listOf(IcOp.Commit(" "))), down(space, ' '))
        assertEquals(ImeOutcome(true, emptyList()), c.keyUp(space))
        assertEquals(ImeOutcome(false, emptyList()), c.keyUp(space))
        assertEquals(ImeOutcome(true, listOf(IcOp.Commit("1"))), down(8, '1'))
        down(shiftLeft)
        c.keyUp(shiftLeft)
        assertEquals(InputMode.ENGLISH, c.engine.mode)
        assertEquals(ImeOutcome(true, listOf(IcOp.Commit("a"))), down(keyA, 'a'))
    }

    @Test
    fun `引擎放行的控制鍵（Enter、Backspace 等）經 InputConnection 轉送按鍵事件，keyUp 也轉送一次`() {
        assertEquals(ImeOutcome(true, listOf(IcOp.ForwardKey)), down(enter, '\n'))
        assertEquals(ImeOutcome(true, listOf(IcOp.ForwardKey)), c.keyUp(enter))
        assertEquals(ImeOutcome(true, listOf(IcOp.ForwardKey)), down(del))
        assertEquals(ImeOutcome(true, listOf(IcOp.ForwardKey)), c.keyUp(del))
    }

    @Test
    fun `修飾鍵組合與無字元的鍵直接交給系統`() {
        assertEquals(ImeOutcome(false, emptyList()), down(keyA, 'a', ctrl = true))
        assertEquals(ImeOutcome(false, emptyList()), down(19))
        assertEquals(ImeOutcome(false, emptyList()), c.keyUp(19))
    }

    @Test
    fun `組字中按標點：組字失敗，移除組字且不放行按鍵`() {
        down(keyB, 'b')
        assertEquals(ImeOutcome(true, listOf(IcOp.ClearComposing)), down(8, '!'))
        assertEquals(ImeOutcome(true, emptyList()), c.keyUp(8))
        assertTrue(c.engine.failed)
    }

    @Test
    fun `單按 Shift 切換中英；Shift 本身一律放行給系統`() {
        assertEquals(ImeOutcome(false, emptyList()), down(shiftLeft))
        assertEquals(ImeOutcome(false, emptyList()), c.keyUp(shiftLeft))
        assertEquals(InputMode.ENGLISH, c.engine.mode)
        down(shiftLeft)
        down(keyA, 'A')
        c.keyUp(shiftLeft)
        assertEquals(InputMode.ENGLISH, c.engine.mode)
    }

    @Test
    fun `Ctrl+J 與其他修飾鍵組合一樣交給 App（沒有日文模式）`() {
        assertEquals(ImeOutcome(false, emptyList()), down(keyJ, 'j', ctrl = true))
        assertEquals(InputMode.CHINESE, c.engine.mode)
    }

    @Test
    fun `密碼欄與未匯入字表時輸入法不介入，Shift 單按也不切換`() {
        c.startInput(classText or textPassword)
        assertFalse(c.imeActive)
        assertEquals(ImeOutcome(false, emptyList()), down(keyB, 'b'))
        down(shiftLeft)
        c.keyUp(shiftLeft)
        assertEquals(InputMode.CHINESE, c.engine.mode)
        val noTable = ImeController(LiuEngine(null)).apply { startInput(classText) }
        assertFalse(noTable.imeActive)
    }

    @Test
    fun `軟鍵盤：字元鍵組字、點選候選上屏、反引號查同音`() {
        assertEquals(listOf(IcOp.SetComposing("c")), c.softKey(SoftKey.Text('c'), 0, classText))
        assertEquals(listOf(IcOp.Commit("火")), c.tapCandidate(1))
        c.softKey(SoftKey.Text('q'), 0, classText)
        assertEquals(listOf(IcOp.SetComposing("q")), c.softKey(SoftKey.Text('`'), 0, classText))
        assertEquals("中", c.engine.homophoneOf)
    }

    @Test
    fun `軟鍵盤：引擎不處理的鍵直接送出`() {
        assertEquals(listOf(IcOp.Commit(" ")), c.softKey(SoftKey.Space, 0, classText))
        assertEquals(listOf(IcOp.SendKey(del)), c.softKey(SoftKey.Backspace, 0, classText))
        assertEquals(listOf(IcOp.Commit("!")), c.softKey(SoftKey.Text('!'), 0, classText))
        assertEquals(listOf(IcOp.SendKey(enter)), c.softKey(SoftKey.Enter, 0, classText or multiLine))
        assertEquals(listOf(IcOp.EditorAction(actionSearch)), c.softKey(SoftKey.Enter, actionSearch, classText))
    }

    @Test
    fun `軟鍵盤：組字中按非字根鍵為組字失敗；空碼按空白出空白`() {
        c.softKey(SoftKey.Text('b'), 0, classText)
        assertEquals(listOf(IcOp.ClearComposing), c.softKey(SoftKey.Text('!'), 0, classText))
        c.softKey(SoftKey.Text('x'), 0, classText)
        assertEquals(listOf(IcOp.Commit(" ")), c.softKey(SoftKey.Space, 0, classText))
    }

    @Test
    fun `軟鍵盤：切換鍵與輸入法不介入時的放行`() {
        assertEquals(emptyList(), c.softKey(SoftKey.ToggleEnglish, 0, classText))
        assertEquals(InputMode.ENGLISH, c.engine.mode)
        c.softKey(SoftKey.ToggleEnglish, 0, classText)
        assertEquals(InputMode.CHINESE, c.engine.mode)
        c.startInput(classText or textPassword)
        assertEquals(listOf(IcOp.Commit("b")), c.softKey(SoftKey.Text('b'), 0, classText or textPassword))
    }

    @Test
    fun `游標移出組字區時結束組字並重置引擎`() {
        down(keyB, 'b')
        assertEquals(emptyList(), c.selectionMoved(1, 1, 0, 1))
        assertEquals(listOf(IcOp.FinishComposing), c.selectionMoved(0, 0, 0, 1))
        assertEquals("", c.engine.composing)
    }

    @Test
    fun `startInput 與 finishInput 清除狀態`() {
        down(keyB, 'b')
        c.finishInput()
        assertEquals("", c.engine.composing)
        assertEquals(emptyList(), c.selectionMoved(0, 0, 0, 1))
        down(keyB, 'b')
        down(space, ' ')
        c.startInput(classText)
        assertEquals(ImeOutcome(false, emptyList()), c.keyUp(keyB))
        assertEquals(ImeOutcome(false, emptyList()), c.keyUp(space))
    }
}
