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
            "bǐ" to "ㄅㄧˇ", "ń" to "ㄣˊ", "ḿ" to "ㄇˊ", "ňg" to "ㄫˇ", "ê" to "˙ㄝ", "nu:" to "˙ㄋㄩ", "lve" to "˙ㄌㄩㄝ",
        )
        for ((py, zy) in cases) assertEquals(zy, PinyinZhuyin.convert(py), py)
    }

    @Test
    fun `無法轉換的拼音回傳 null`() {
        for (bad in listOf("", "123", "žo", "ê̄", "hm", "bx")) assertNull(PinyinZhuyin.convert(bad), bad)
    }

    @Test
    fun `同音字限字表內，依年級、碼長、頻率排序`() {
        val r = Fixtures.readings
        assertEquals(listOf("ㄓㄨㄥ", "ㄓㄨㄥˋ"), r.of("中"))
        // 鐘（2 年級）排在忠（無年級，雖然字表頻率較高）之前
        assertEquals(listOf("中", "鐘", "忠"), r.homophones("忠", Fixtures.traditional))
        assertEquals(listOf("仲"), r.homophones("仲", Fixtures.traditional))
    }

    @Test
    fun `沒有讀音、或讀音在字表中找不到同音字時回傳空清單`() {
        val r = Fixtures.readings
        assertEquals(emptyList(), r.homophones("無", Fixtures.traditional))
        assertEquals(emptyList(), r.homophones("聽", Fixtures.traditional))
    }

    @Test
    fun `讀音表解析略過註解、空行與格式不完整的列`() {
        val r = Readings.parse(sequenceOf("# c", "", "甲", "\tㄅ", "乙\t ", "丙\tㄅㄧㄥˇ ㄅㄧㄥ\tx", "丁\tㄉㄧㄥ\t3"))
        assertEquals(emptyList(), r.of("甲"))
        assertEquals(emptyList(), r.of("乙"))
        assertEquals(listOf("ㄅㄧㄥˇ", "ㄅㄧㄥ"), r.of("丙"))
        assertEquals(listOf("ㄉㄧㄥ"), r.of("丁"))
    }

    @Test
    fun `內建讀音表可載入並含年級欄`() {
        val r = Readings.loadBundled()
        assertEquals(listOf("ㄓㄨㄥ"), r.of("中"))
        assertEquals(listOf("ㄕˋ"), r.of("是"))
        assertTrue(Readings.EMPTY.of("中").isEmpty())
    }

    @Test
    fun `假名判定涵蓋平假名與片假名區塊`() {
        for (c in "あゝゞアヽヾーぁ") assertTrue(Kana.isKana(c), c.toString())
        for (c in "a中、") assertFalse(Kana.isKana(c), c.toString())
    }
}
