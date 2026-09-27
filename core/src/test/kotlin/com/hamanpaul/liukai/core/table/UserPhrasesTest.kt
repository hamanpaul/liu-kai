package com.hamanpaul.liukai.core.table

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class UserPhrasesTest {
    @Test
    fun `拆碼規則照官方：英文字母、數字、逗號句點，第一碼不可為數字`() {
        assertTrue(UserPhrases.validCode("liu"))
        assertTrue(UserPhrases.validCode("a1"))
        assertTrue(UserPhrases.validCode(",a"))
        assertTrue(UserPhrases.validCode(".b2"))
        assertTrue(UserPhrases.validCode("LIU"))
        assertFalse(UserPhrases.validCode(""))
        assertFalse(UserPhrases.validCode("1a"))
        assertFalse(UserPhrases.validCode("a b"))
        assertFalse(UserPhrases.validCode("a'"))
        // 字母範圍前後的字元：@ [ ` ~ 與非 ASCII
        for (bad in listOf("a@", "a[", "a`", "a~", "a中")) assertFalse(UserPhrases.validCode(bad), bad)
    }

    @Test
    fun `解析與輸出 TSV：拆碼轉小寫、略過空行與註解`() {
        val list = UserPhrases.parse("# 自訂字詞\nLIU\t嘸蝦米輸入法\n\nliu\tboshiamy.com\na1\t甲一\n")
        assertEquals(listOf(UserPhrase("liu", "嘸蝦米輸入法"), UserPhrase("liu", "boshiamy.com"), UserPhrase("a1", "甲一")), list)
        assertEquals("liu\t嘸蝦米輸入法\nliu\tboshiamy.com\na1\t甲一\n", UserPhrases.serialize(list))
        assertEquals(list, UserPhrases.parse(UserPhrases.serialize(list)))
    }

    @Test
    fun `格式錯誤的列回報列號`() {
        assertEquals("第 2 列：缺少字詞", assertFailsWith<IllegalArgumentException> { UserPhrases.parse("a\t甲\nb\n") }.message)
        assertEquals("第 1 列：拆碼不合法（1a）", assertFailsWith<IllegalArgumentException> { UserPhrases.parse("1a\t甲") }.message)
        assertEquals("第 1 列：缺少字詞", assertFailsWith<IllegalArgumentException> { UserPhrases.parse("a\t") }.message)
    }

    @Test
    fun `上移下移：在清單內調整順序，超出邊界不動`() {
        val list = listOf(UserPhrase("a", "1"), UserPhrase("a", "2"), UserPhrase("b", "3"))
        assertEquals(listOf(UserPhrase("a", "2"), UserPhrase("a", "1"), UserPhrase("b", "3")), UserPhrases.moveUp(list, 1))
        assertEquals(list, UserPhrases.moveUp(list, 0))
        assertEquals(listOf(UserPhrase("a", "1"), UserPhrase("b", "3"), UserPhrase("a", "2")), UserPhrases.moveDown(list, 1))
        assertEquals(list, UserPhrases.moveDown(list, 2))
    }

    @Test
    fun `與字表合併：自訂字詞排在同碼候選前面`() {
        val table = CompiledTable.build(
            UserPhrases.merge(listOf(TableEntry("ba", "日"), TableEntry("ba", "月")), listOf(UserPhrase("ba", "巴"), UserPhrase("liu", "嘸蝦米"))),
        )
        assertEquals(listOf("巴", "日", "月"), table.candidates("ba"))
        assertEquals(listOf("嘸蝦米"), table.candidates("liu"))
    }
}
