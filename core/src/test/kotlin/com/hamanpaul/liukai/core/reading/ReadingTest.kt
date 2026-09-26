package com.hamanpaul.liukai.core.reading

import com.hamanpaul.liukai.core.Fixtures
import com.hamanpaul.liukai.core.kana.Kana
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ReadingTest {
    @Test
    fun `拼音轉注音`() {
        val cases = mapOf(
            "zhōng" to "ㄓㄨㄥ", "tiān" to "ㄊㄧㄢ", "hǎo" to "ㄏㄠˇ", "shì" to "ㄕˋ", "zi" to "˙ㄗ",
            "lǜ" to "ㄌㄩˋ", "jué" to "ㄐㄩㄝˊ", "qióng" to "ㄑㄩㄥˊ", "xuǎn" to "ㄒㄩㄢˇ", "jūn" to "ㄐㄩㄣ",
            "yī" to "ㄧ", "yǒu" to "ㄧㄡˇ", "yuè" to "ㄩㄝˋ", "yòng" to "ㄩㄥˋ", "wéi" to "ㄨㄟˊ", "wǒ" to "ㄨㄛˇ",
            "ér" to "ㄦˊ", "ài" to "ㄞˋ", "guǐ" to "ㄍㄨㄟˇ", "liù" to "ㄌㄧㄡˋ", "dūn" to "ㄉㄨㄣ", "bō" to "ㄅㄛ",
            "ń" to "ㄣˊ", "ê̄" to null,
        )
        for ((py, zy) in cases) assertEquals(zy, PinyinZhuyin.convert(py), py)
        assertNull(PinyinZhuyin.convert("hm"))
        assertNull(PinyinZhuyin.convert("123"))
    }

    @Test
    fun `同音字限字表內，依年級、碼長、頻率排序`() {
        val r = Fixtures.readings
        assertEquals(listOf("ㄓㄨㄥ", "ㄓㄨㄥˋ"), r.of("中"))
        assertEquals(1, r.gradeOf("中"))
        // 鐘（2 年級）排在忠（無年級，雖然字表頻率較高）之前
        assertEquals(listOf("中", "鐘", "忠"), r.homophones("忠", Fixtures.traditional))
        assertEquals(listOf("仲"), r.homophones("仲", Fixtures.traditional))
        assertEquals(emptyList(), r.homophones("無", Fixtures.traditional))
    }

    @Test
    fun `假名判定與轉換`() {
        assertEquals("キャ", Kana.toKatakana("きゃ"))
        assertEquals("ヽ", Kana.toKatakana("ゝ"))
        assertTrue(Kana.isAllHiragana("らーめん"))
        assertFalse(Kana.isAllHiragana("蚊"))
        assertFalse(Kana.isAllHiragana("ー"))
        assertTrue(Kana.isKana('カ'))
    }
}
