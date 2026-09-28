package com.hamanpaul.liukai.core.table

import com.hamanpaul.liukai.core.Fixtures
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull
import kotlin.test.assertTrue

class TableImportTest {
    @Test
    fun `IBus 解析只讀 BEGIN_TABLE 區塊並保留頻率`() {
        val entries = IbusTableParser.parse(Fixtures.text("synthetic-ibus.txt").lineSequence())
        assertEquals(45, entries.size)
        assertEquals(TableEntry("a", "甲", 90), entries.first())
        assertEquals(TableEntry("x,", "雫", 40), entries.last())
    }

    @Test
    fun `CIN 多區段解析：無標頭首段、gen_inp 切段、略過 keyname`() {
        val sections = CinParser.parse(Fixtures.text("synthetic-lime.txt").lineSequence())
        assertEquals(listOf(31, 2, 2, 8), sections.map { it.entries.size })
        assertEquals(listOf(0, 1, 0, 1), sections.map { it.keynames.size })
        assertNull(sections[0].cname)
        assertEquals("合成簡體", sections[1].cname)
        assertEquals("合成台簡", sections[2].cname)
        assertEquals("合成日文", sections[3].cname)
    }

    @Test
    fun `以 CIN 邊界切分 IBus 並判定區段用途`() {
        val split = SectionSplitter.split(
            IbusTableParser.parse(Fixtures.text("synthetic-ibus.txt").lineSequence()),
            CinParser.parse(Fixtures.text("synthetic-lime.txt").lineSequence()),
        )
        assertEquals(
            listOf(SectionKind.TRADITIONAL, SectionKind.SIMPLIFIED, SectionKind.TW_SIMPLIFIED, SectionKind.JAPANESE),
            split.map { it.kind },
        )
        assertEquals(120L, split[0].entries.first { it.code == "Ab" }.freq)
    }

    @Test
    fun `IBus 夾帶 keyname 列時一併對齊並丟棄；不含 keyname 時也可切分`() {
        val cin = listOf(
            CinSection(null, null, listOf(TableEntry("a", "甲"))),
            CinSection("日文", null, listOf(TableEntry("ka", "か")), keynames = listOf(TableEntry("k", "Ｋ"))),
        )
        val withKeyname = listOf(TableEntry("a", "甲", 5), TableEntry("k", "Ｋ"), TableEntry("ka", "か", 7))
        val split = SectionSplitter.split(withKeyname, cin)
        assertEquals(listOf(listOf(TableEntry("a", "甲", 5)), listOf(TableEntry("ka", "か", 7))), split.map { it.entries })
        val dataOnly = listOf(TableEntry("a", "甲", 5), TableEntry("ka", "か", 7))
        assertEquals(split, SectionSplitter.split(dataOnly, cin))
        val all = Fixtures.importResult.bundle.sections.flatMap { it.entries }.map { it.text }
        assertTrue("Ａ" !in all && "Ｋ" !in all)
    }

    @Test
    fun `區段內容與 IBus 不一致時拒絕`() {
        val ibus = listOf(TableEntry("a", "甲"), TableEntry("b", "乙"))
        val cin = listOf(CinSection(null, null, listOf(TableEntry("a", "甲"), TableEntry("b", "丙"))))
        assertFailsWith<SectionMismatchException> { SectionSplitter.split(ibus, cin) }
        assertFailsWith<SectionMismatchException> { SectionSplitter.split(ibus, cin.map { it.copy(entries = it.entries.take(1)) }) }
    }

    @Test
    fun `匯入：保留繁、簡、台簡、日四段（語言模式用），字碼小寫、去重並取最大頻率`() {
        val result = Fixtures.importResult
        assertEquals(
            listOf(SectionKind.TRADITIONAL, SectionKind.SIMPLIFIED, SectionKind.TW_SIMPLIFIED, SectionKind.JAPANESE),
            result.bundle.sections.map { it.kind },
        )
        assertEquals(listOf("甲", "简"), result.bundle.section(SectionKind.SIMPLIFIED)!!.entries.map { it.text })
        assertEquals(8, result.bundle.section(SectionKind.JAPANESE)!!.entries.size)
        assertEquals(4, result.allSections.size)
        assertEquals(result.bundle.sections.map { it.kind }, result.bundle.stats.map { it.kind })
        val trad = result.bundle.section(SectionKind.TRADITIONAL)!!
        val tian = trad.entries.filter { it.text == "天" }
        assertEquals(listOf(TableEntry("ab", "天", 120)), tian)
        val stats = result.bundle.stats.first { it.kind == SectionKind.TRADITIONAL }
        // 繁中 31 列（去重後 30 組）＋ 日文區段併入的 6 個假名字碼
        assertEquals(37, stats.rawRows)
        assertEquals(36, stats.uniquePairs)
        assertEquals(2, result.bundle.sources.size)
        assertTrue(result.bundle.sources.all { it.sha256.length == 64 })
    }

    @Test
    fun `日文區段只把羅馬拼音加逗號或句點的假名字碼併入繁中，日文漢字不併入`() {
        val trad = Fixtures.importResult.bundle.section(SectionKind.TRADITIONAL)!!.entries
        val merged = trad.filter { it.text.all { c -> c.code in 0x3041..0x30FF } }
        assertEquals(
            listOf(
                TableEntry("a,", "あ", 100), TableEntry("a,", "ぁ", 50), TableEntry("a.", "ア", 100),
                TableEntry("ka,", "か", 100), TableEntry("ka.", "カ", 100), TableEntry("kya,", "きゃ", 90),
            ),
            merged,
        )
        assertTrue(trad.none { it.text == "寸" || it.text == "雫" })
    }

    @Test
    fun `SectionStats 保留區段名稱、字碼數與字數`() {
        val jp = Fixtures.importResult.allSections.first { it.kind == SectionKind.JAPANESE }
        assertEquals("合成日文", jp.name)
        assertEquals(7, jp.uniqueCodes)
        assertEquals(8, jp.uniqueTexts)
    }

    @Test
    fun `只有 IBus 檔時拒絕匯入`() {
        val e = assertFailsWith<TableImportException> {
            TableImporter.import(listOf(NamedBytes("x.txt", Fixtures.bytes("synthetic-ibus.txt"))))
        }
        assertTrue("lime_liu7" in e.message!!)
    }

    @Test
    fun `單一 CIN 檔可匯入，頻率為 0`() {
        val result = TableImporter.import(listOf(NamedBytes("lime.txt", Fixtures.bytes("synthetic-lime.txt"))))
        val trad = result.bundle.section(SectionKind.TRADITIONAL)!!
        assertTrue(trad.entries.all { it.freq == 0L })
    }

    @Test
    fun `中性 TSV 往返`() {
        val tsv = TableImporter.writeNeutral(Fixtures.importResult.bundle.sections)
        val again = TableImporter.import(listOf(NamedBytes("liu.tsv", tsv.toByteArray())))
        assertEquals(Fixtures.importResult.bundle.sections.map { it.entries }, again.bundle.sections.map { it.entries })
    }

    @Test
    fun `二進位字表檔往返（區段名稱可有可無）`() {
        val named = TableImporter.import(
            listOf(NamedBytes("liu.cin", "%gen_inp\n%cname\t蝦\n%chardef\tbegin\na\t甲\n%chardef\tend\n".toByteArray())),
        ).bundle
        assertEquals("蝦", named.sections.single().name)
        for (bundle in listOf(Fixtures.importResult.bundle, named)) {
            val out = ByteArrayOutputStream()
            bundle.write(out)
            assertEquals(bundle, TableBundle.read(ByteArrayInputStream(out.toByteArray())))
        }
    }

    @Test
    fun `無標頭首段超過 64KB 的 LIME 檔仍判定為 CIN`() {
        // 真實 lime_liu7.txt 的第一個 % 指令在第 28,817 行（約 250KB 之後）。
        val body = (0 until 12_000).joinToString("\n") { "a$it\t甲" }
        val text = "$body\n%gen_inp\n%cname\t日文蝦\n%chardef\tbegin\nka\tか\n"
        assertEquals(TableFormat.CIN, TableImporter.detect(text))
    }

    @Test
    fun `無法辨識的檔案拒絕匯入`() {
        assertFailsWith<TableImportException> { TableImporter.import(listOf(NamedBytes("x", "hello".toByteArray()))) }
    }

    private fun bytesOf(text: String) = text.toByteArray()

    private fun importText(vararg files: Pair<String, String>) =
        TableImporter.import(files.map { (name, text) -> NamedBytes(name, bytesOf(text)) })

    @Test
    fun `不支援的檔案組合：未選檔、同格式多檔、TSV 混 CIN`() {
        val ibus = Fixtures.text("synthetic-ibus.txt")
        val lime = Fixtures.text("synthetic-lime.txt")
        val tsv = TableImporter.writeNeutral(Fixtures.importResult.bundle.sections)
        for (files in listOf(emptyList(), listOf("a" to ibus, "b" to ibus), listOf("t" to tsv, "c" to lime))) {
            val e = assertFailsWith<TableImportException> { importText(*files.toTypedArray()) }
            assertTrue("不支援的檔案組合" in e.message!!, e.message)
        }
    }

    @Test
    fun `IBus 與 CIN 內容對不上時以匯入失敗回報`() {
        val ibus = "BEGIN_TABLE\na\t甲\t1\nEND_TABLE\n"
        val cin = "%chardef begin\na 乙\n%chardef end\n"
        val e = assertFailsWith<TableImportException> { importText("i" to ibus, "c" to cin) }
        assertTrue("區段對不上" in e.message!!)
    }

    @Test
    fun `IBus 與 CIN 字碼不同也視為對不上`() {
        val cin = listOf(CinSection(null, null, listOf(TableEntry("a", "甲"), TableEntry("c", "乙"))))
        assertFailsWith<SectionMismatchException> {
            SectionSplitter.split(listOf(TableEntry("a", "甲"), TableEntry("b", "乙")), cin)
        }
    }

    @Test
    fun `沒有日文段時只保留繁中；沒有繁中段時拒絕`() {
        val onlyTrad = importText("t" to "# liu-kai-tsv v1\nTRADITIONAL\ta\t甲\t3\n")
        assertEquals(listOf(SectionKind.TRADITIONAL), onlyTrad.bundle.sections.map { it.kind })
        val e = assertFailsWith<TableImportException> { importText("t" to "# liu-kai-tsv v1\nJAPANESE\ta,\tあ\t1\n") }
        assertTrue("找不到繁中區段" in e.message!!)
    }

    @Test
    fun `中性 TSV 欄位不足或類型未知時拒絕；頻率欄可省略或非數字`() {
        assertFailsWith<TableImportException> { importText("t" to "# liu-kai-tsv v1\nTRADITIONAL\ta\n") }
        assertFailsWith<TableImportException> { importText("t" to "# liu-kai-tsv v1\nKOREAN\ta\t가\t1\n") }
        val r = importText("t" to "# liu-kai-tsv v1\nTRADITIONAL\ta\t甲\nTRADITIONAL\tb\t乙\tx\n\n")
        assertEquals(listOf(TableEntry("a", "甲", 0), TableEntry("b", "乙", 0)), r.bundle.sections.single().entries)
    }

    @Test
    fun `IBus 解析略過表內空行、註解與不完整的列，頻率可省略`() {
        val text = listOf(
            "\uFEFF### header", "a\tout-of-table", "BEGIN_TABLE", "", "### note", "a\t甲\t5", "b\t乙",
            "c", "\t丙\t1", "d\t\t1", "e 戊 7", "END_TABLE", "f\t己\t1",
        ).joinToString("\n")
        assertEquals(
            listOf(TableEntry("a", "甲", 5), TableEntry("b", "乙", 0), TableEntry("e", "戊", 7)),
            IbusTableParser.parse(text.lineSequence()),
        )
    }

    @Test
    fun `CIN 解析：檔頭 gen_inp、註解、不完整列與格式錯誤的 keyname`() {
        val text = listOf(
            "%gen_inp", "%ename liu", "%cname 蝦", "# comment", "%keyname end", "%keyname begin", "a Ａ", "bad", "%keyname end",
            "%chardef begin", "a 甲", "x", "\t乙", "b\t", "%chardef end", "%gen_inp", "%cname 空", "%gen_inp",
        ).joinToString("\n")
        val sections = CinParser.parse(text.lineSequence())
        assertEquals(2, sections.size)
        assertEquals(CinSection("空", null, emptyList()), sections[1])
        assertEquals("蝦", sections[0].cname)
        assertEquals("liu", sections[0].ename)
        assertEquals(listOf(TableEntry("a", "甲")), sections[0].entries)
        assertEquals(listOf(TableEntry("a", "Ａ")), sections[0].keynames)
        assertEquals(emptyList(), CinParser.parse(emptySequence()))
        val headerOnly = CinParser.parse(sequenceOf("%cname 只有標頭"))
        assertEquals(listOf(CinSection("只有標頭", null, emptyList())), headerOnly)
        assertEquals(listOf(CinSection(null, "only-ename", emptyList())), CinParser.parse(sequenceOf("%ename only-ename")))
    }

    @Test
    fun `區段用途判定：簡體、台簡（打繁出簡）與 lime_liu7 只叫「蝦」的台簡段`() {
        val any = listOf(TableEntry("a", "甲"))
        assertEquals(SectionKind.SIMPLIFIED, SectionSplitter.guessKind(1, "簡體蝦", "liu", any))
        assertEquals(SectionKind.TW_SIMPLIFIED, SectionSplitter.guessKind(0, "台簡蝦", "liu", any))
        assertEquals(SectionKind.TW_SIMPLIFIED, SectionSplitter.guessKind(2, "蝦", "liu", any))
        assertEquals(SectionKind.TRADITIONAL, SectionSplitter.guessKind(0, "嘸蝦米", "liu", any))
        assertEquals(SectionKind.TRADITIONAL, SectionSplitter.guessKind(0, "蝦", "liu", any))
        assertEquals(SectionKind.JAPANESE, SectionSplitter.guessKind(3, "日文蝦", "liu", any))
    }

    @Test
    fun `區段用途判定：名稱含日文或 japan、假名比例、空區段`() {
        val kana = listOf(TableEntry("ka,", "か"), TableEntry("ki,", "き"))
        val kanji = listOf(TableEntry("a", "甲"))
        assertEquals(SectionKind.JAPANESE, SectionSplitter.guessKind(3, null, "Japanese", kanji))
        assertEquals(SectionKind.JAPANESE, SectionSplitter.guessKind(3, null, "liu-jp", kanji))
        assertEquals(SectionKind.JAPANESE, SectionSplitter.guessKind(3, "x", null, kana))
        assertEquals(SectionKind.OTHER, SectionSplitter.guessKind(3, "x", null, emptyList()))
        assertEquals(SectionKind.TRADITIONAL, SectionSplitter.guessKind(0, null, null, emptyList()))
    }

    @Test
    fun `字表檔 magic 或版本不符時拒絕讀取`() {
        assertFailsWith<IllegalArgumentException> { TableBundle.read(ByteArrayInputStream("NOTLIUKAI_______".toByteArray())) }
        val badVersion = ByteArrayOutputStream().also {
            it.write("LIUKAITB".toByteArray())
            it.write(byteArrayOf(0, 0, 0, 2))
        }
        val e = assertFailsWith<IllegalArgumentException> { TableBundle.read(ByteArrayInputStream(badVersion.toByteArray())) }
        assertTrue("版本 2" in e.message!!)
    }
}
