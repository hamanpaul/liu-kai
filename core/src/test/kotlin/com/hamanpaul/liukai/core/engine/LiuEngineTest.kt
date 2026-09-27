package com.hamanpaul.liukai.core.engine

import com.hamanpaul.liukai.core.Fixtures
import com.hamanpaul.liukai.core.engine.ImeEvent.Key
import com.hamanpaul.liukai.core.reading.Readings
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
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
        assertEquals(EngineResult(true, "日"), e.handle(ImeEvent.Space))
        assertEquals("", e.composing)
        assertTrue(e.candidates.isEmpty())
    }

    @Test
    fun `無組字時空白交給 App；空碼按空白清除組字並直接出空白`() {
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.Space))
        type("xv")
        assertEquals(EngineResult(true, " "), e.handle(ImeEvent.Space))
        assertEquals("", e.composing)
        assertFalse(e.failed)
    }

    @Test
    fun `觸控點選與數字鍵選字：0 為預設字，1–9 為第 2–10 個候選`() {
        type("ba")
        assertEquals(EngineResult(true, "月"), e.handle(ImeEvent.Select(1)))
        type("c")
        assertEquals(EngineResult(true, "土"), e.handle(Key('2')))
        type("c")
        assertEquals(EngineResult(true, "水"), e.handle(Key('0')))
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
        e.handle(ImeEvent.Escape)
        type("vv")
        assertEquals(listOf("風"), texts())
    }

    @Test
    fun `VRSF：候選不足或碼不完整時當字根附加`() {
        type("br")
        assertEquals("br", e.composing)
        assertTrue(e.candidates.isEmpty())
        e.handle(ImeEvent.Escape)
        type("cs")
        assertEquals("cs", e.composing)
        e.handle(ImeEvent.Escape)
        type("xv")
        assertEquals("xv", e.composing)
    }

    @Test
    fun `標點也可以是字根`() {
        type("x,")
        assertEquals(listOf("雲"), texts())
    }

    @Test
    fun `打滿最長碼後再打字根為組字失敗`() {
        type("abcd")
        assertEquals(listOf("和"), texts())
        assertEquals(EngineResult.CONSUMED, e.handle(Key('a')))
        assertEquals("", e.composing)
        assertTrue(e.failed)
    }

    @Test
    fun `Backspace、Esc、Enter`() {
        type("ab")
        e.handle(ImeEvent.Backspace)
        assertEquals("a", e.composing)
        e.handle(ImeEvent.Escape)
        assertEquals("", e.composing)
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.Backspace))
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.Escape))
        type("ab")
        assertEquals(EngineResult(true, "ab"), e.handle(ImeEvent.Enter))
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.Enter))
    }

    @Test
    fun `翻頁後數字鍵與空白以目前頁為準`() {
        type("a")
        assertEquals(12, e.candidates.size)
        e.handle(ImeEvent.PageDown)
        assertEquals(10, e.pageStart)
        assertEquals(EngineResult(true, "丑"), e.handle(Key('1')))
        type("a")
        e.handle(Key('='))
        assertEquals(EngineResult(true, "子"), e.handle(ImeEvent.Space))
        type("a")
        e.handle(ImeEvent.PageDown)
        e.handle(ImeEvent.PageDown)
        assertEquals(10, e.pageStart)
        e.handle(Key('-'))
        assertEquals(0, e.pageStart)
    }

    @Test
    fun `組字中按非字根鍵：組字失敗，清除組字、不出字並標示失敗，下一個事件清除標示`() {
        type("ba")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('!')))
        assertEquals("", e.composing)
        assertTrue(e.candidates.isEmpty())
        assertTrue(e.failed)
        type("b")
        assertFalse(e.failed)
        assertEquals(EngineResult.CONSUMED, e.handle(Key('A')))
        assertTrue(e.failed)
        assertEquals(EngineResult.PASS, e.handle(Key('A')))
        assertFalse(e.failed)
    }

    @Test
    fun `重置引擎時清除失敗標示`() {
        type("ba!")
        assertTrue(e.failed)
        e.reset()
        assertFalse(e.failed)
    }

    @Test
    fun `萬用字元 * 比對零到多個字根，候選附字碼`() {
        type("a*")
        assertEquals("甲", texts().first())
        val tian = e.candidates.first { it.text == "天" }
        assertEquals("ab", tian.annotation)
        e.handle(ImeEvent.Escape)
        type("*d")
        assertEquals(listOf("和"), texts())
    }

    @Test
    fun `問號不是萬用字元：組字中按下為組字失敗，無組字時交給 App`() {
        type("ba")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('?')))
        assertTrue(e.failed)
        assertEquals(EngineResult.PASS, e.handle(Key('?')))
    }

    @Test
    fun `同音字：組字後按反引號查首選的讀音與同音字`() {
        type("q")
        e.handle(Key('`'))
        assertEquals("中", e.homophoneOf)
        assertEquals(listOf("中", "鐘", "忠"), texts())
        assertEquals("ㄓㄨㄥ／ㄓㄨㄥˋ", e.candidates[0].annotation)
        assertEquals("ㄓㄨㄥ", e.candidates[1].annotation)
        assertEquals(EngineResult(true, "忠"), e.handle(ImeEvent.Select(2)))
        assertNull(e.homophoneOf)
    }

    @Test
    fun `同音鍵前置查詢：按同音鍵後打字碼，空白選字列出該字的同音字，組字區清空`() {
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.HomophoneKey))
        assertTrue(e.isComposing)
        assertEquals("'", e.displayComposing)
        type("q")
        assertEquals("'q", e.displayComposing)
        assertEquals(listOf("中", "仲"), texts())
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.Space))
        assertEquals("中", e.homophoneOf)
        assertEquals(listOf("中", "鐘", "忠"), texts())
        assertTrue(e.isComposing)
        assertEquals("", e.displayComposing)
        assertEquals(EngineResult(true, "忠"), e.handle(ImeEvent.Select(2)))
        assertNull(e.homophoneOf)
        assertFalse(e.isComposing)
    }

    @Test
    fun `同音鍵前置查詢：數字鍵與觸控選的是要查同音的字`() {
        e.handle(ImeEvent.HomophoneKey)
        type("q1")
        assertEquals("仲", e.homophoneOf)
        e.handle(ImeEvent.Escape)
        assertFalse(e.isComposing)
        e.handle(ImeEvent.HomophoneKey)
        type("q")
        e.handle(ImeEvent.Select(0))
        assertEquals("中", e.homophoneOf)
    }

    @Test
    fun `同音鍵前置查詢：Backspace 從同音字回到字碼，再刪光字碼後離開查詢`() {
        e.handle(ImeEvent.HomophoneKey)
        type("q")
        e.handle(ImeEvent.Space)
        e.handle(ImeEvent.Backspace)
        assertNull(e.homophoneOf)
        assertEquals("'q", e.displayComposing)
        e.handle(ImeEvent.Backspace)
        assertEquals("'", e.displayComposing)
        assertTrue(e.isComposing)
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.Backspace))
        assertFalse(e.isComposing)
        assertEquals("", e.displayComposing)
    }

    @Test
    fun `同音鍵前置查詢：還沒打字碼時 Enter 只離開查詢，打了字碼時送出字碼`() {
        e.handle(ImeEvent.HomophoneKey)
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.Enter))
        assertFalse(e.isComposing)
        e.handle(ImeEvent.HomophoneKey)
        type("q")
        assertEquals(EngineResult(true, "q"), e.handle(ImeEvent.Enter))
    }

    @Test
    fun `組字中按同音鍵與反引號相同，查首選的同音字；同音字列表中再按不動作`() {
        type("q")
        e.handle(ImeEvent.HomophoneKey)
        assertEquals("中", e.homophoneOf)
        assertEquals("q", e.displayComposing)
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.HomophoneKey))
        assertEquals("中", e.homophoneOf)
        assertEquals(listOf("中", "鐘", "忠"), texts())
    }

    @Test
    fun `英文模式同音鍵交給 App`() {
        e.handle(EngineEvent.ToggleEnglish)
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.HomophoneKey))
    }

    @Test
    fun `同音模式 Backspace 回到原字碼候選`() {
        type("q`")
        assertEquals("中", e.homophoneOf)
        e.handle(ImeEvent.Backspace)
        assertNull(e.homophoneOf)
        assertEquals(listOf("中", "仲"), texts())
    }

    @Test
    fun `沒有讀音資料時仍顯示原字`() {
        val noReadings = LiuEngine(Fixtures.traditional, Readings.EMPTY)
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
    fun `假名：一般模式直接打羅馬拼音加逗號為平假名、加句點為片假名，候選順序照字表`() {
        type("ka,")
        assertEquals(listOf("か"), texts())
        assertEquals(EngineResult(true, "か"), e.handle(ImeEvent.Space))
        type("ka.")
        assertEquals(listOf("カ"), texts())
        e.handle(ImeEvent.Escape)
        type("a,")
        assertEquals(listOf("あ", "ぁ"), texts())
        assertEquals(EngineResult(true, "ぁ"), e.handle(Key('v')))
    }

    @Test
    fun `日文區段只併入假名，日文漢字不會出現在候選`() {
        type("aa")
        assertTrue(e.candidates.isEmpty())
        e.handle(ImeEvent.Escape)
        type("x,")
        assertEquals(listOf("雲"), texts())
    }

    @Test
    fun `沒有字表時按鍵全部交給 App`() {
        val empty = LiuEngine(null)
        assertEquals(EngineResult.PASS, empty.handle(Key('a')))
        assertEquals(EngineResult.PASS, empty.handle(ImeEvent.Space))
    }

    @Test
    fun `setTables 換表並重置組字；預設設定每頁 10 個候選`() {
        type("b")
        e.setTables(Fixtures.traditional, Readings.EMPTY)
        assertEquals("", e.composing)
        assertEquals(10, e.config.pageSize)
    }

    @Test
    fun `翻頁事件：沒有組字時放行，第一頁往前與最後一頁往後都停在原頁`() {
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.PageDown))
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.PageUp))
        type("a")
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.PageUp))
        assertEquals(0, e.pageStart)
        e.handle(ImeEvent.PageDown)
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.PageDown))
        assertEquals(10, e.pageStart)
    }

    @Test
    fun `數字 0 選目前頁預設字、9 選第 10 個候選；超出候選數的數字鍵為組字失敗`() {
        type("a")
        assertEquals(EngineResult(true, "甲"), e.handle(Key('0')))
        type("a")
        assertEquals(EngineResult(true, "癸"), e.handle(Key('9')))
        type("c")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('5')))
        assertEquals("", e.composing)
        assertTrue(e.failed)
    }

    @Test
    fun `自訂每頁 5 個候選時，數字 5 以上不選字（組字失敗）`() {
        val small = LiuEngine(Fixtures.traditional, Readings.EMPTY, EngineConfig(pageSize = 5))
        small.handle(Key('a'))
        assertEquals(EngineResult.CONSUMED, small.handle(Key('5')))
        assertTrue(small.failed)
        small.handle(Key('a'))
        assertEquals(EngineResult(true, "戊"), small.handle(Key('4')))
    }

    @Test
    fun `非 ASCII 數字不當選字鍵，視為非字根鍵`() {
        type("ba")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('٣')))
        assertTrue(e.failed)
    }

    @Test
    fun `萬用字元組字中按 VRSF 鍵一律當字根`() {
        type("a*v")
        assertEquals("a*v", e.composing)
    }

    @Test
    fun `空碼時按非字根鍵同樣是組字失敗`() {
        type("xv")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('!')))
        assertEquals("", e.composing)
        assertTrue(e.failed)
    }

    @Test
    fun `點選不存在的候選不動作；無候選時按反引號不進同音模式`() {
        type("ba")
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.Select(-1)))
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.Select(2)))
        e.handle(ImeEvent.Escape)
        type("xv`")
        assertNull(e.homophoneOf)
    }

    @Test
    fun `刪光字根後候選清空，再按 Backspace 交給 App`() {
        type("b")
        assertEquals(EngineResult.CONSUMED, e.handle(ImeEvent.Backspace))
        assertEquals("", e.composing)
        assertTrue(e.candidates.isEmpty())
        assertEquals(EngineResult.PASS, e.handle(ImeEvent.Backspace))
    }

    @Test
    fun `同音模式：數字鍵選字、翻頁鍵翻頁、其他鍵為組字失敗`() {
        type("q`")
        assertEquals(EngineResult(true, "鐘"), e.handle(Key('1')))
        type("q`")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('=')))
        assertEquals(EngineResult.CONSUMED, e.handle(Key('-')))
        assertEquals("中", e.homophoneOf)
        assertEquals(EngineResult.CONSUMED, e.handle(Key('!')))
        assertNull(e.homophoneOf)
        assertEquals("", e.composing)
        assertTrue(e.failed)
        type("q`")
        assertEquals(EngineResult.CONSUMED, e.handle(Key('A')))
        assertTrue(e.failed)
    }
}
