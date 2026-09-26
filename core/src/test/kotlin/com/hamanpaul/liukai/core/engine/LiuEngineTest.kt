package com.hamanpaul.liukai.core.engine

import com.hamanpaul.liukai.core.Fixtures
import com.hamanpaul.liukai.core.engine.EngineEvent.Key
import com.hamanpaul.liukai.core.reading.Readings
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class LiuEngineTest {
    private val e = Fixtures.engine()

    private fun type(s: String): List<EngineResult> = s.map { e.handle(Key(it)) }

    private fun texts() = e.candidates.map { it.text }

    @Test
    fun `輸入字碼顯示候選，空白上屏首選`() {
        type("ba")
        assertEquals("ba", e.composing)
        assertEquals(listOf("日", "月"), texts())
        assertEquals(EngineResult(true, "日"), e.handle(EngineEvent.Space))
        assertEquals("", e.composing)
        assertTrue(e.candidates.isEmpty())
    }

    @Test
    fun `無組字時空白交給 App；有組字無候選時保留組字`() {
        assertEquals(EngineResult.PASS, e.handle(EngineEvent.Space))
        type("xv")
        assertEquals(EngineResult.CONSUMED, e.handle(EngineEvent.Space))
        assertEquals("xv", e.composing)
    }

    @Test
    fun `觸控點選與數字鍵選字`() {
        type("ba")
        assertEquals(EngineResult(true, "月"), e.handle(EngineEvent.Select(1)))
        type("c")
        assertEquals(EngineResult(true, "火"), e.handle(Key('2')))
    }

    @Test
    fun `VRSF：碼加鍵非任何字碼前綴且碼為完整碼時選第 2 到 5 候選`() {
        type("ba")
        assertEquals(EngineResult(true, "月"), e.handle(Key('v')))
        type("c")
        assertEquals(EngineResult(true, "土"), e.handle(Key('r')))
    }

    @Test
    fun `VRSF 衝突：碼加鍵是合法碼的前綴時當字根`() {
        type("abv")
        assertEquals("abv", e.composing)
        assertEquals(listOf("地"), texts())
        e.handle(EngineEvent.Escape)
        type("vv")
        assertEquals(listOf("風"), texts())
    }

    @Test
    fun `VRSF：候選不足或碼不完整時當字根附加`() {
        type("br")
        assertEquals("br", e.composing)
        assertTrue(e.candidates.isEmpty())
        e.handle(EngineEvent.Escape)
        type("cs")
        assertEquals("cs", e.composing)
        e.handle(EngineEvent.Escape)
        type("xv")
        assertEquals("xv", e.composing)
    }

    @Test
    fun `標點也可以是字根`() {
        type("x,")
        assertEquals(listOf("雲"), texts())
    }

    @Test
    fun `組字長度不超過最長碼`() {
        type("abcda")
        assertEquals("abcd", e.composing)
        assertEquals(listOf("和"), texts())
    }

    @Test
    fun `Backspace、Esc、Enter`() {
        type("ab")
        e.handle(EngineEvent.Backspace)
        assertEquals("a", e.composing)
        e.handle(EngineEvent.Escape)
        assertEquals("", e.composing)
        assertEquals(EngineResult.PASS, e.handle(EngineEvent.Backspace))
        assertEquals(EngineResult.PASS, e.handle(EngineEvent.Escape))
        type("ab")
        assertEquals(EngineResult(true, "ab"), e.handle(EngineEvent.Enter))
        assertEquals(EngineResult.PASS, e.handle(EngineEvent.Enter))
    }

    @Test
    fun `翻頁後數字鍵與空白以目前頁為準`() {
        type("a")
        assertEquals(12, e.candidates.size)
        e.handle(EngineEvent.PageDown)
        assertEquals(10, e.pageStart)
        assertEquals(EngineResult(true, "丑"), e.handle(Key('2')))
        type("a")
        e.handle(Key('='))
        assertEquals(EngineResult(true, "子"), e.handle(EngineEvent.Space))
        type("a")
        e.handle(EngineEvent.PageDown)
        e.handle(EngineEvent.PageDown)
        assertEquals(10, e.pageStart)
        e.handle(Key('-'))
        assertEquals(0, e.pageStart)
    }

    @Test
    fun `組字中按非字根鍵：先上屏首選再把按鍵交給 App`() {
        type("ba")
        assertEquals(EngineResult(false, "日"), e.handle(Key('!')))
        type("ba")
        assertEquals(EngineResult(false, "日"), e.handle(Key('A')))
        assertEquals(EngineResult.PASS, e.handle(Key('A')))
    }

    @Test
    fun `萬用字元候選附字碼`() {
        type("a?")
        val tian = e.candidates.first { it.text == "天" }
        assertEquals("ab", tian.annotation)
        e.handle(EngineEvent.Escape)
        type("*d")
        assertEquals(listOf("和"), texts())
    }

    @Test
    fun `同音字：組字後按反引號查首選的讀音與同音字`() {
        type("q")
        e.handle(Key('`'))
        assertEquals("中", e.homophoneOf)
        assertEquals(listOf("中", "鐘", "忠"), texts())
        assertEquals("ㄓㄨㄥ／ㄓㄨㄥˋ", e.candidates[0].annotation)
        assertEquals("ㄓㄨㄥ", e.candidates[1].annotation)
        assertEquals(EngineResult(true, "忠"), e.handle(EngineEvent.Select(2)))
        assertNull(e.homophoneOf)
    }

    @Test
    fun `同音模式 Backspace 回到原字碼候選；長按查指定候選`() {
        type("q")
        e.handle(EngineEvent.Homophone(1))
        assertEquals(listOf("仲"), texts())
        e.handle(EngineEvent.Backspace)
        assertNull(e.homophoneOf)
        assertEquals(listOf("中", "仲"), texts())
    }

    @Test
    fun `沒有讀音資料時仍顯示原字`() {
        val noReadings = LiuEngine(Fixtures.traditional, Fixtures.japanese, Readings.EMPTY)
        "q".forEach { noReadings.handle(Key(it)) }
        noReadings.handle(Key('`'))
        assertEquals(listOf("中"), noReadings.candidates.map { it.text })
        assertEquals("無讀音資料", noReadings.candidates[0].annotation)
    }

    @Test
    fun `中英切換`() {
        e.handle(EngineEvent.ToggleEnglish)
        assertEquals(InputMode.ENGLISH, e.mode)
        assertEquals(EngineResult.PASS, e.handle(Key('a')))
        e.handle(EngineEvent.ToggleEnglish)
        assertEquals(InputMode.CHINESE, e.mode)
    }

    @Test
    fun `日文模式：羅馬拼音加逗號為平假名、加句點為片假名，候選順序照字表`() {
        e.handle(EngineEvent.ToggleJapanese)
        assertEquals(InputMode.JAPANESE, e.mode)
        type("ka,")
        assertEquals(listOf("か"), texts())
        e.handle(EngineEvent.Space)
        type("ka.")
        assertEquals(listOf("カ"), texts())
        e.handle(EngineEvent.Escape)
        type("a,")
        assertEquals(listOf("あ", "ぁ"), texts())
        assertEquals(EngineResult(true, "ぁ"), e.handle(Key('v')))
        type("aa")
        assertEquals(listOf("寸"), texts())
        e.handle(EngineEvent.ToggleEnglish)
        e.handle(EngineEvent.ToggleEnglish)
        assertEquals(InputMode.JAPANESE, e.mode)
        e.handle(EngineEvent.ToggleJapanese)
        assertEquals(InputMode.CHINESE, e.mode)
    }

    @Test
    fun `沒有日文表時切換無作用；沒有字表時按鍵全部交給 App`() {
        val noJp = LiuEngine(Fixtures.traditional, null, Fixtures.readings)
        noJp.handle(EngineEvent.ToggleJapanese)
        assertEquals(InputMode.CHINESE, noJp.mode)
        val empty = LiuEngine(null)
        assertEquals(EngineResult.PASS, empty.handle(Key('a')))
        assertEquals(EngineResult.PASS, empty.handle(EngineEvent.Space))
    }
}
