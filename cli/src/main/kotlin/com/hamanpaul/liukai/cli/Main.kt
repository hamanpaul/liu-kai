package com.hamanpaul.liukai.cli

import com.hamanpaul.liukai.core.reading.PinyinZhuyin
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.TableImportException
import com.hamanpaul.liukai.core.table.TableImporter
import java.io.File
import java.io.PrintStream

private const val USAGE = """liu-kai-cli <command> [options]

commands:
  stats   <file>...                         匯入字表並印出各區段統計（不寫檔）
  convert <file>... --out <tsv>             匯入字表並輸出中性 TSV（請寫到 repo 外）
  gen-readings --unihan <Unihan_Readings.txt> [--grades <Unihan_DictionaryLikeData.txt>] --out <readings.tsv>
                                            由 Unihan kMandarin 產生注音讀音表（可附 kGradeLevel 常用度）
"""

/**
 * 失敗時以未捕捉例外結束（JVM 結束碼 1），只印訊息不印 stack trace。
 * 不用 exitProcess：永不返回的呼叫會讓覆蓋率工具記錄不到該段程式。
 */
fun main(args: Array<String>) {
    Thread.setDefaultUncaughtExceptionHandler { _, e -> System.err.println(e.message) }
    val code = run(args.toList(), System.out, System.err)
    check(code == 0) { "liu-kai-cli 失敗（結束碼 $code）" }
}

fun run(args: List<String>, out: PrintStream, err: PrintStream): Int {
    val command = args.firstOrNull() ?: run { err.print(USAGE); return 2 }
    val rest = args.drop(1)
    return try {
        when (command) {
            "stats" -> { printStats(importFiles(positional(rest)), out); 0 }
            "convert" -> convert(rest, out)
            "gen-readings" -> genReadings(rest, out)
            "-h", "--help", "help" -> { out.print(USAGE); 0 }
            else -> { err.print(USAGE); 2 }
        }
    } catch (e: TableImportException) {
        err.println("匯入失敗：${e.message}")
        1
    } catch (e: IllegalArgumentException) {
        err.println("參數錯誤：${e.message}")
        2
    }
}

private fun option(args: List<String>, name: String): String? {
    val i = args.indexOf(name)
    return if (i >= 0) requireNotNull(args.getOrNull(i + 1)) { "$name 需要值" } else null
}

private fun positional(args: List<String>): List<String> {
    val out = ArrayList<String>()
    var i = 0
    while (i < args.size) {
        if (args[i].startsWith("--")) i += 2 else out += args[i++]
    }
    require(out.isNotEmpty()) { "需要至少一個字表檔" }
    return out
}

private fun importFiles(paths: List<String>): ImportResult =
    TableImporter.import(paths.map { File(it).let { f -> NamedBytes(f.name, f.readBytes()) } })

private fun printStats(result: ImportResult, out: PrintStream) {
    out.println("sources:")
    for (s in result.bundle.sources) out.println("  ${s.name}  sha256=${s.sha256}")
    out.println("sections:")
    result.allSections.forEachIndexed { i, s ->
        out.println(
            "  [$i] kind=${s.kind} name=${s.name ?: "-"} rawRows=${s.rawRows} uniquePairs=${s.uniquePairs} " +
                "uniqueCodes=${s.uniqueCodes} uniqueTexts=${s.uniqueTexts}",
        )
    }
}

private fun convert(args: List<String>, out: PrintStream): Int {
    val target = File(requireNotNull(option(args, "--out")) { "convert 需要 --out" })
    val result = importFiles(positional(args))
    target.absoluteFile.parentFile.mkdirs()
    target.writeText(TableImporter.writeNeutral(result.bundle.sections))
    printStats(result, out)
    out.println("wrote ${target.path}")
    return 0
}

/**
 * Unihan_Readings.txt 的 kMandarin 行：`U+4E2D<TAB>kMandarin<TAB>zhōng`（兩個值時第二個為台灣讀音）。
 * 輸出以台灣讀音為主要讀音。
 */
private fun genReadings(args: List<String>, out: PrintStream): Int {
    val source = File(requireNotNull(option(args, "--unihan")) { "gen-readings 需要 --unihan" })
    val target = File(requireNotNull(option(args, "--out")) { "gen-readings 需要 --out" })
    val grades = HashMap<String, String>()
    option(args, "--grades")?.let { path ->
        File(path).useLines { seq ->
            for (line in seq) {
                val cols = line.split('\t')
                if (cols.size >= 3 && cols[1] == "kGradeLevel") {
                    grades[String(Character.toChars(cols[0].removePrefix("U+").toInt(16)))] = cols[2].trim()
                }
            }
        }
    }
    val lines = ArrayList<String>()
    var skipped = 0
    source.useLines { seq ->
        for (line in seq) {
            val cols = line.split('\t')
            if (cols.size < 3 || cols[1] != "kMandarin") continue
            val ch = String(Character.toChars(cols[0].removePrefix("U+").toInt(16)))
            val values = cols[2].trim().split(' ').filter { it.isNotEmpty() }.reversed()
            val zhuyin = values.mapNotNull { PinyinZhuyin.convert(it) }.distinct()
            if (zhuyin.isEmpty()) {
                skipped++
            } else {
                lines += "$ch\t${zhuyin.joinToString(" ")}" + (grades[ch]?.let { "\t$it" } ?: "")
            }
        }
    }
    target.absoluteFile.parentFile.mkdirs()
    target.writeText(
        buildString {
            append("# 由 liu-kai-cli gen-readings 自 Unicode Unihan kMandarin／kGradeLevel 產生（Unicode License v3，見 THIRD_PARTY_NOTICES.md）\n")
            append("# 欄位：字<TAB>注音讀音（第一個為主要讀音，台灣讀音優先）[<TAB>kGradeLevel]\n")
            lines.forEach { append(it).append('\n') }
        },
    )
    out.println("readings=${lines.size} skipped=$skipped graded=${lines.count { it.count { c -> c == '\t' } == 2 }} wrote ${target.path}")
    return 0
}
