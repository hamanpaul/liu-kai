package com.hamanpaul.liukai.core.ime

import com.hamanpaul.liukai.core.engine.ImeEvent
import com.hamanpaul.liukai.core.Fixtures
import com.hamanpaul.liukai.core.engine.EngineEvent
import com.hamanpaul.liukai.core.engine.EngineResult
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class CompositionTrackerTest {
    private val engine = Fixtures.engine()
    private val tracker = CompositionTracker()

    private fun key(c: Char) = tracker.sync(engine.handle(ImeEvent.Key(c)), engine)

    @Test
    fun `組字中設定組字區`() {
        assertEquals(listOf(IcOp.SetComposing("b")), key('b'))
        assertTrue(tracker.shown)
    }

    @Test
    fun `上屏時先 commit，之後沒有組字就不再動組字區`() {
        key('b')
        assertEquals(listOf(IcOp.Commit("木")), tracker.sync(engine.handle(ImeEvent.Space), engine))
        assertFalse(tracker.shown)
    }

    @Test
    fun `組字被清空時移除組字區`() {
        key('b')
        assertEquals(listOf(IcOp.ClearComposing), tracker.sync(engine.handle(ImeEvent.Escape), engine))
        assertFalse(tracker.shown)
    }

    @Test
    fun `畫面上沒有組字區時不做任何操作`() {
        assertEquals(emptyList(), tracker.sync(EngineResult.PASS, engine))
    }

    @Test
    fun `同音模式組字區顯示原字碼`() {
        key('q')
        assertEquals(listOf(IcOp.SetComposing("q")), tracker.sync(engine.handle(ImeEvent.Key('`')), engine))
    }

    @Test
    fun `游標移出組字區時要求結束組字；仍在組字區尾端則不動`() {
        key('b')
        assertFalse(tracker.selectionMoved(1, 1, 0, 1))
        assertTrue(tracker.selectionMoved(0, 0, 0, 1))
        assertFalse(tracker.shown)
        key('b')
        assertTrue(tracker.selectionMoved(1, 2, 0, 1))
    }

    @Test
    fun `沒有組字區或 App 回報無組字區時忽略游標移動`() {
        assertFalse(tracker.selectionMoved(0, 0, 0, 1))
        key('b')
        assertFalse(tracker.selectionMoved(0, 0, -1, -1))
    }

    @Test
    fun `reset 後視為沒有組字區`() {
        key('b')
        tracker.reset()
        assertFalse(tracker.shown)
    }
}
