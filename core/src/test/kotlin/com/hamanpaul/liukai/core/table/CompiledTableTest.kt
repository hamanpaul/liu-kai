package com.hamanpaul.liukai.core.table

import com.hamanpaul.liukai.core.Fixtures
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class CompiledTableTest {
    private val t = Fixtures.traditional

    @Test
    fun `候選依來源順位`() {
        assertEquals(listOf("日", "月"), t.candidates("ba"))
        assertEquals(12, t.candidates("a").size)
        assertEquals(emptyList(), t.candidates("zz"))
    }

    @Test
    fun `前綴與完整碼判定`() {
        assertTrue(t.hasPrefix("ab"))
        assertTrue(t.hasPrefix("abv"))
        assertTrue(t.hasPrefix("abc"))
        assertFalse(t.hasPrefix("bav"))
        assertTrue(t.hasPrefix("r"))
        assertTrue(t.isCode("ab"))
        assertFalse(t.isCode("abcx"))
    }

    @Test
    fun `字根集與最長碼`() {
        assertEquals(4, t.maxCodeLength)
        assertTrue(',' in t.alphabet)
        assertFalse('z' in t.alphabet)
    }

    @Test
    fun `萬用字元：星號比對零到多個字根，短碼優先並標示字碼`() {
        assertEquals(setOf("a", "ba", "qa"), t.wildcard("*a", 20).map { it.second }.toSet())
        assertEquals(emptyList(), t.wildcard("a?", 10))
        val star = t.wildcard("ab*", 10)
        assertEquals(listOf("天", "人", "地", "和"), star.map { it.first })
        assertEquals("ab", star.first().second)
        assertEquals(3, t.wildcard("a*", 3).size)
    }

    @Test
    fun `字反查字碼與頻率`() {
        assertEquals(listOf("ab"), t.codesOf("天"))
        assertEquals(120L, t.freqOf("天"))
        assertTrue(t.containsText("雲"))
    }

    @Test
    fun `未知的字與超出範圍的前綴`() {
        assertEquals(0L, t.freqOf("無"))
        assertEquals(emptyList(), t.codesOf("無"))
        assertFalse(t.hasPrefix("zzz"))
    }

    @Test
    fun `空字表`() {
        val empty = CompiledTable.build(emptyList())
        assertEquals(0, empty.maxCodeLength)
        assertTrue(empty.alphabet.isEmpty())
        assertFalse(empty.hasPrefix("a"))
    }

    @Test
    fun `建表時同碼同字只保留一次，同字多碼時萬用字元結果去重並取短碼`() {
        val table = CompiledTable.build(
            listOf(TableEntry("ab", "甲", 1), TableEntry("ab", "甲", 9), TableEntry("ac", "甲", 2), TableEntry("a", "乙")),
        )
        assertEquals(listOf("甲"), table.candidates("ab"))
        assertEquals(9L, table.freqOf("甲"))
        assertEquals(listOf("ab", "ac"), table.codesOf("甲"))
        assertEquals(listOf("乙" to "a", "甲" to "ab"), table.wildcard("a*", 10))
    }
}
