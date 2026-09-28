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
    fun `萬用字元：問號恰一個、星號零到多個，短碼優先並標示字碼`() {
        assertEquals(listOf("天" to "ab", "日" to "ba", "月" to "ba"), t.wildcard("??", 10).filter { it.second in setOf("ab", "ba") })
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
}
