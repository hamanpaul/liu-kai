package com.hamanpaul.liukai.core.ime

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class EditorPolicyTest {
    // Android InputType／EditorInfo 常數字面值
    private val classText = 0x1
    private val classNumber = 0x2
    private val textPassword = 0x80
    private val textVisiblePassword = 0x90
    private val textWebPassword = 0xe0
    private val textEmail = 0x20
    private val numberPassword = 0x10
    private val multiLine = 0x20000
    private val actionUnspecified = 0
    private val actionNone = 1
    private val actionGo = 2
    private val actionSearch = 3
    private val actionSend = 4
    private val noEnterAction = 0x40000000

    @Test
    fun `文字密碼三種變體與數字密碼都判定為密碼欄`() {
        assertTrue(EditorPolicy.isPassword(classText or textPassword))
        assertTrue(EditorPolicy.isPassword(classText or textVisiblePassword))
        assertTrue(EditorPolicy.isPassword(classText or textWebPassword))
        assertTrue(EditorPolicy.isPassword(classNumber or numberPassword))
    }

    @Test
    fun `一般文字、email、一般數字不是密碼欄`() {
        assertFalse(EditorPolicy.isPassword(classText))
        assertFalse(EditorPolicy.isPassword(classText or textEmail))
        assertFalse(EditorPolicy.isPassword(classNumber))
        assertFalse(EditorPolicy.isPassword(classNumber or textPassword))
    }

    @Test
    fun `單行欄位有指定動作時 Enter 觸發欄位動作`() {
        assertEquals(EnterAction.EditorAction(actionSearch), EditorPolicy.enterAction(actionSearch, classText))
        assertEquals(EnterAction.EditorAction(actionGo), EditorPolicy.enterAction(actionGo, classText))
        assertEquals(EnterAction.EditorAction(actionSend), EditorPolicy.enterAction(actionSend or 0x10000000, classText))
    }

    @Test
    fun `多行、要求不觸發動作、或沒有指定動作時 Enter 換行`() {
        assertEquals(EnterAction.NewLine, EditorPolicy.enterAction(actionSearch, classText or multiLine))
        assertEquals(EnterAction.NewLine, EditorPolicy.enterAction(actionSearch or noEnterAction, classText))
        assertEquals(EnterAction.NewLine, EditorPolicy.enterAction(actionNone, classText))
        assertEquals(EnterAction.NewLine, EditorPolicy.enterAction(actionUnspecified, classText))
    }
}
