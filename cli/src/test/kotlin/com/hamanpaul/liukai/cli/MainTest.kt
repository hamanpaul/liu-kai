package com.hamanpaul.liukai.cli

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.PrintStream
import kotlin.io.path.createTempDirectory
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class MainTest {
    private val fixtures = File("../core/src/test/resources/fixtures")

    private fun exec(vararg args: String): Triple<Int, String, String> {
        val out = ByteArrayOutputStream()
        val err = ByteArrayOutputStream()
        val code = run(args.toList(), PrintStream(out, true, "UTF-8"), PrintStream(err, true, "UTF-8"))
        return Triple(code, out.toString("UTF-8"), err.toString("UTF-8"))
    }

    @Test
    fun `stats 印出全區段統計`() {
        val (code, out, _) = exec("stats", "$fixtures/synthetic-ibus.txt", "$fixtures/synthetic-lime.txt")
        assertEquals(0, code)
        assertTrue("[0] kind=TRADITIONAL name=- rawRows=31 uniquePairs=30" in out, out)
        assertTrue("[2] kind=JAPANESE name=合成日文 rawRows=6" in out, out)
    }

    @Test
    fun `convert 輸出中性 TSV`() {
        val dir = createTempDirectory().toFile()
        val tsv = File(dir, "out/liu.tsv")
        val (code, _, _) = exec("convert", "$fixtures/synthetic-ibus.txt", "$fixtures/synthetic-lime.txt", "--out", tsv.path)
        assertEquals(0, code)
        val lines = tsv.readLines()
        assertEquals("# liu-kai-tsv v1", lines.first())
        assertTrue("TRADITIONAL\tab\t天\t120" in lines)
        assertTrue("JAPANESE\tka\tか\t100" in lines)
    }

    @Test
    fun `只給 IBus 檔時回報匯入失敗`() {
        val (code, _, err) = exec("stats", "$fixtures/synthetic-ibus.txt")
        assertEquals(1, code)
        assertTrue("lime_liu7" in err)
    }

    @Test
    fun `gen-readings 以台灣讀音為主`() {
        val dir = createTempDirectory().toFile()
        val unihan = File(dir, "Unihan_Readings.txt").apply {
            writeText("# comment\nU+4E2D\tkMandarin\tzhōng\nU+4E2D\tkDefinition\tcentral\nU+8AB0\tkMandarin\tshéi shuí\nU+20000\tkMandarin\thm\n")
        }
        val out = File(dir, "readings.tsv")
        val (code, stdout, _) = exec("gen-readings", "--unihan", unihan.path, "--out", out.path)
        assertEquals(0, code)
        assertTrue("readings=2 skipped=1" in stdout, stdout)
        val lines = out.readLines().filterNot { it.startsWith("#") }
        assertEquals(listOf("中\tㄓㄨㄥ", "誰\tㄕㄨㄟˊ ㄕㄟˊ"), lines)
    }

    @Test
    fun `未知指令印出用法`() {
        val (code, _, err) = exec("nope")
        assertEquals(2, code)
        assertTrue("liu-kai-cli" in err)
    }
}
