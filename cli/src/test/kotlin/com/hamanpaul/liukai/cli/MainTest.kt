package com.hamanpaul.liukai.cli

import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableBundle
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.PrintStream
import kotlin.io.path.createTempDirectory
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
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
        assertTrue("[2] kind=JAPANESE name=合成日文 rawRows=8" in out, out)
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
        assertTrue("TRADITIONAL\tka,\tか\t100" in lines)
        assertTrue(lines.none { it.startsWith("JAPANESE") })
    }

    @Test
    fun `bundle 輸出 app 直接載入的二進位字表`() {
        val dir = createTempDirectory().toFile()
        val bin = File(dir, "out/bundled.liutable")
        val (code, out, _) = exec("bundle", "$fixtures/synthetic-ibus.txt", "$fixtures/synthetic-lime.txt", "--out", bin.path)
        assertEquals(0, code)
        assertTrue("wrote ${bin.path}" in out, out)
        val read = FileInputStream(bin).let { TableBundle.read(it).also { _ -> it.close() } }
        assertEquals(listOf(SectionKind.TRADITIONAL), read.sections.map { it.kind })
        assertTrue(read.sections.single().entries.any { it.code == "ka," && it.text == "か" })
        assertEquals(listOf("synthetic-ibus.txt", "synthetic-lime.txt"), read.sources.map { it.name })
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
        val grades = File(dir, "Unihan_DictionaryLikeData.txt").apply {
            writeText("U+4E2D\tkGradeLevel\t1\nU+4E2D\tkFenn\t9A\n")
        }
        val out = File(dir, "readings.tsv")
        val (code, stdout, _) = exec("gen-readings", "--unihan", unihan.path, "--grades", grades.path, "--out", out.path)
        assertEquals(0, code)
        assertTrue("readings=2 skipped=1 graded=1" in stdout, stdout)
        val lines = out.readLines().filterNot { it.startsWith("#") }
        assertEquals(listOf("中\tㄓㄨㄥ\t1", "誰\tㄕㄨㄟˊ ㄕㄟˊ"), lines)
    }

    @Test
    fun `未知指令印出用法`() {
        val (code, _, err) = exec("nope")
        assertEquals(2, code)
        assertTrue("liu-kai-cli" in err)
    }

    @Test
    fun `說明與參數錯誤的回報`() {
        assertEquals(2, exec().first)
        for (flag in listOf("-h", "--help", "help")) {
            val (code, out, _) = exec(flag)
            assertEquals(0, code)
            assertTrue("gen-readings" in out)
        }
        val cases = listOf(
            listOf("stats") to "需要至少一個字表檔",
            listOf("convert", "$fixtures/synthetic-lime.txt") to "convert 需要 --out",
            listOf("convert", "$fixtures/synthetic-lime.txt", "--out") to "--out 需要值",
            listOf("bundle", "$fixtures/synthetic-lime.txt") to "bundle 需要 --out",
            listOf("gen-readings", "--out", "x.tsv") to "gen-readings 需要 --unihan",
            listOf("gen-readings", "--unihan", "u.txt") to "gen-readings 需要 --out",
        )
        for ((args, message) in cases) {
            val (code, _, err) = exec(*args.toTypedArray())
            assertEquals(2, code, args.toString())
            assertTrue(message in err, err)
        }
    }

    @Test
    fun `gen-readings 不帶年級檔、並容忍多餘空白與不完整的列`() {
        val dir = createTempDirectory().toFile()
        val unihan = File(dir, "u.txt").apply { writeText("U+4E00\tkMandarin\tyī  yí\nshort line\n") }
        val grades = File(dir, "g.txt").apply { writeText("# comment\nU+4E00\tkGradeLevel\t1\n") }
        val out = File(dir, "r.tsv")
        assertEquals(0, exec("gen-readings", "--unihan", unihan.path, "--out", out.path).first)
        assertEquals(listOf("一\tㄧˊ ㄧ"), out.readLines().filterNot { it.startsWith("#") })
        assertEquals(0, exec("gen-readings", "--unihan", unihan.path, "--grades", grades.path, "--out", out.path).first)
        assertEquals(listOf("一\tㄧˊ ㄧ\t1"), out.readLines().filterNot { it.startsWith("#") })
    }

    @Test
    fun `main 成功時正常返回、失敗時以例外結束並只印訊息`() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        val originalOut = System.out
        val originalErr = System.err
        val err = ByteArrayOutputStream()
        try {
            System.setOut(PrintStream(ByteArrayOutputStream(), true, "UTF-8"))
            System.setErr(PrintStream(err, true, "UTF-8"))
            main(arrayOf("--help"))
            val e = assertFailsWith<IllegalStateException> { main(arrayOf("nope")) }
            assertEquals("liu-kai-cli 失敗（結束碼 2）", e.message)
            Thread.getDefaultUncaughtExceptionHandler()!!.uncaughtException(Thread.currentThread(), e)
            assertTrue(err.toString("UTF-8").endsWith("liu-kai-cli 失敗（結束碼 2）\n"))
        } finally {
            System.setOut(originalOut)
            System.setErr(originalErr)
            Thread.setDefaultUncaughtExceptionHandler(previous)
        }
    }
}
