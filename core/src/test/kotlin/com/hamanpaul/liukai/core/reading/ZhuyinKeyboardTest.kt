package com.hamanpaul.liukai.core.reading

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ZhuyinKeyboardTest {
    @Test
    fun `實體鍵位與聲調鍵對應注音符號`() {
        val symbolLayout = ZhuyinKeyboard.layout.filter { ZhuyinKeyboard.isSymbol(it.physicalKey) }
        assertEquals(
            "ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ",
            symbolLayout.joinToString("") { it.glyph },
        )
        assertEquals("1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/ -".replace(" ", ""), symbolLayout.joinToString("") { it.physicalKey.toString() })
        assertEquals(41, ZhuyinKeyboard.layout.size)
        assertEquals(mapOf('3' to "ˇ", '4' to "ˋ", '6' to "ˊ", '7' to "˙"), ZhuyinKeyboard.layout.filter { ZhuyinKeyboard.isTone(it.physicalKey) }.associate { it.physicalKey to it.glyph })
        assertEquals("ˊ", ZhuyinKeyboard.glyphForKey('6'))
        assertEquals("ˇ", ZhuyinKeyboard.glyphForKey('3'))
        assertEquals("ˋ", ZhuyinKeyboard.glyphForKey('4'))
        assertEquals("˙", ZhuyinKeyboard.glyphForKey('7'))
        assertEquals("ˉ", ZhuyinKeyboard.glyphForKey(' '))
        assertEquals("ㄅ", ZhuyinKeyboard.glyphForKey('1'))
        assertNull(ZhuyinKeyboard.glyphForKey('='))
        assertTrue(ZhuyinKeyboard.isSymbol('Q'))
        assertFalse(ZhuyinKeyboard.isSymbol('?'))
        assertTrue(ZhuyinKeyboard.isTone('6'))
        assertFalse(ZhuyinKeyboard.isTone('a'))
    }

    @Test
    fun `按鍵可依注音類別組音並正規顯示`() {
        var input = ""
        for (key in "5/j") input = ZhuyinKeyboard.appendCanonical(input, key)
        assertEquals("5j/", input)
        assertEquals("ㄓㄨㄥ", ZhuyinKeyboard.display(input))
        assertEquals("ㄓㄨㄥ", ZhuyinKeyboard.reading(input))

        input = ZhuyinKeyboard.appendCanonical(input, 'm')
        assertEquals("5m/", input)
        input = ZhuyinKeyboard.appendCanonical(input, '3')
        input = ZhuyinKeyboard.appendCanonical(input, '4')
        assertEquals("5m/4", input)
        assertEquals("ㄓㄩㄥˋ", ZhuyinKeyboard.display(input))
        assertEquals("ㄓㄩㄥˋ", ZhuyinKeyboard.reading(input))
    }

    @Test
    fun `滿長讀音仍可替換中介音與聲調`() {
        assertEquals("5m/4", ZhuyinKeyboard.appendCanonical("5j/4", 'm'))
        assertEquals("5j/3", ZhuyinKeyboard.appendCanonical("5j/4", '3'))
    }

    @Test
    fun `輕聲讀音置於讀音前且空輸入及無效讀音回傳 null`() {
        assertEquals("˙ㄓㄨㄥ", ZhuyinKeyboard.reading("5j/7"))
        assertNull(ZhuyinKeyboard.reading(""))
        assertNull(ZhuyinKeyboard.reading("5j/44"))
        assertNull(ZhuyinKeyboard.reading("577"))
        assertNull(ZhuyinKeyboard.reading("45j/"))
        assertNull(ZhuyinKeyboard.reading("5j=="))
        assertNull(ZhuyinKeyboard.reading("55"))
        assertNull(ZhuyinKeyboard.reading("j5"))
        assertNull(ZhuyinKeyboard.reading("/5"))
        assertNull(ZhuyinKeyboard.reading("jj"))
        assertNull(ZhuyinKeyboard.reading("/j"))
        assertNull(ZhuyinKeyboard.reading("//"))
        assertNull(ZhuyinKeyboard.reading("4"))
        assertEquals("ㄨ", ZhuyinKeyboard.reading("j"))
        assertEquals("ㄢ", ZhuyinKeyboard.reading("0"))
        assertEquals("ㄦ", ZhuyinKeyboard.reading("-"))
    }

    @Test
    fun `追加未知鍵不改組音而不合法實體鍵不產生讀音`() {
        assertEquals("5", ZhuyinKeyboard.appendCanonical("", '5'))
        assertEquals("5", ZhuyinKeyboard.appendCanonical("5", '='))
        assertNull(ZhuyinKeyboard.glyphForKey('!'))
        assertEquals("", ZhuyinKeyboard.display("!"))
        assertNull(ZhuyinKeyboard.reading("="))
        assertNull(ZhuyinKeyboard.reading("5j/= "))
    }
}
