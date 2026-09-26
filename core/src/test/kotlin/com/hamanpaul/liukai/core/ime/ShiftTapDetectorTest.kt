package com.hamanpaul.liukai.core.ime

import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ShiftTapDetectorTest {
    private val shiftLeft = 59
    private val shiftRight = 60
    private val keyA = 29

    @Test
    fun `單按左右 Shift 視為一次切換`() {
        val d = ShiftTapDetector()
        d.onKeyDown(shiftLeft, 0)
        assertTrue(d.onKeyUp(shiftLeft))
        d.onKeyDown(shiftRight, 0)
        assertTrue(d.onKeyUp(shiftRight))
    }

    @Test
    fun `Shift 按住期間按了其他鍵就不算單按`() {
        val d = ShiftTapDetector()
        d.onKeyDown(shiftLeft, 0)
        d.onKeyDown(keyA, 0)
        assertFalse(d.onKeyUp(shiftLeft))
    }

    @Test
    fun `長按 Shift 的重複事件不會重置狀態`() {
        val d = ShiftTapDetector()
        d.onKeyDown(shiftLeft, 0)
        d.onKeyDown(keyA, 0)
        d.onKeyDown(shiftLeft, 1)
        assertFalse(d.onKeyUp(shiftLeft))
    }

    @Test
    fun `沒有先按下就放開、或放開一般鍵，都不是單按`() {
        val d = ShiftTapDetector()
        assertFalse(d.onKeyUp(shiftLeft))
        d.onKeyDown(keyA, 0)
        assertFalse(d.onKeyUp(keyA))
    }

    @Test
    fun `isShift 只認左右 Shift`() {
        assertTrue(ShiftTapDetector.isShift(shiftLeft))
        assertTrue(ShiftTapDetector.isShift(shiftRight))
        assertFalse(ShiftTapDetector.isShift(keyA))
    }
}
