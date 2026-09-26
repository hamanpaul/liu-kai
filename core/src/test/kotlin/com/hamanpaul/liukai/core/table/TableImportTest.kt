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
        assertEquals(42, entries.size)
        assertEquals(TableEntry("a", "甲", 90), entries.first())
        assertEquals(TableEntry("aa", "寸", 50), entries.last())
    }

    @Test
    fun `CIN 多區段解析：無標頭首段、gen_inp 切段、略過 keyname`() {
        val sections = CinParser.parse(Fixtures.text("synthetic-lime.txt").lineSequence())
        assertEquals(listOf(31, 2, 7), sections.map { it.entries.size })
        assertEquals(listOf(0, 1, 1), sections.map { it.keynames.size })
        assertNull(sections[0].cname)
        assertEquals("合成簡體", sections[1].cname)
        assertEquals("合成日文", sections[2].cname)
    }

    @Test
    fun `以 CIN 邊界切分 IBus 並判定區段用途`() {
        val split = SectionSplitter.split(
            IbusTableParser.parse(Fixtures.text("synthetic-ibus.txt").lineSequence()),
            CinParser.parse(Fixtures.text("synthetic-lime.txt").lineSequence()),
        )
        assertEquals(listOf(SectionKind.TRADITIONAL, SectionKind.OTHER, SectionKind.JAPANESE), split.map { it.kind })
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
    fun `匯入：只保留繁中與日文，字碼小寫、去重並取最大頻率`() {
        val result = Fixtures.importResult
        assertEquals(listOf(SectionKind.TRADITIONAL, SectionKind.JAPANESE), result.bundle.sections.map { it.kind })
        assertEquals(3, result.allSections.size)
        val trad = result.bundle.section(SectionKind.TRADITIONAL)!!
        val tian = trad.entries.filter { it.text == "天" }
        assertEquals(listOf(TableEntry("ab", "天", 120)), tian)
        val stats = result.bundle.stats.first { it.kind == SectionKind.TRADITIONAL }
        assertEquals(31, stats.rawRows)
        assertEquals(30, stats.uniquePairs)
        assertEquals(2, result.bundle.sources.size)
        assertTrue(result.bundle.sources.all { it.sha256.length == 64 })
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
    fun `二進位字表檔往返`() {
        val bundle = Fixtures.importResult.bundle
        val out = ByteArrayOutputStream()
        bundle.write(out)
        val read = TableBundle.read(ByteArrayInputStream(out.toByteArray()))
        assertEquals(bundle, read)
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
}
