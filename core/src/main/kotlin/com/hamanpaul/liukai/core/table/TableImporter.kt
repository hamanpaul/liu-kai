package com.hamanpaul.liukai.core.table

import com.hamanpaul.liukai.core.kana.Kana

/** 匯入時的一個來源檔（名稱只用於報告與格式判斷）。 */
class NamedBytes(val name: String, val bytes: ByteArray)

class TableImportException(message: String) : IllegalArgumentException(message)

/** 匯入結果：可存檔的字表（繁中區段已併入假名字碼，另保留簡、台簡、日三段供語言模式），以及給使用者看的全區段報告。 */
data class ImportResult(val bundle: TableBundle, val allSections: List<SectionStats>)

enum class TableFormat { IBUS, CIN, NEUTRAL_TSV, UNKNOWN }

/**
 * 支援的組合：
 * - IBus（liu_ibus_final.txt）＋ CIN／LIME 多區段檔（lime_liu7.txt）：以 CIN 邊界切分，沿用 IBus 頻率。
 * - 單一 CIN／LIME 檔：依檔內區段切分，頻率為 0。
 * - 單一 liu-kai 中性 TSV（由 cli 產生）。
 * 只有 IBus 檔時無法可靠切分區段，拒絕匯入。
 */
object TableImporter {
    const val NEUTRAL_HEADER = "# liu-kai-tsv v1"

    /**
     * IBus 的 BEGIN_TABLE 在檔頭；CIN／LIME 的 % 指令可能在很後面
     * （lime_liu7.txt 首段無標頭，第一個指令在第 28,817 行），所以掃描全文。
     */
    fun detect(text: String): TableFormat {
        val head = text.take(64 * 1024)
        return when {
            head.trimStart('﻿').startsWith(NEUTRAL_HEADER) -> TableFormat.NEUTRAL_TSV
            CIN_OR_IBUS_LINE.find(head)?.value?.trim() == "BEGIN_TABLE" -> TableFormat.IBUS
            CIN_DIRECTIVE.containsMatchIn(text) -> TableFormat.CIN
            else -> TableFormat.UNKNOWN
        }
    }

    private val CIN_OR_IBUS_LINE = Regex("(?m)^(BEGIN_TABLE|%chardef|%gen_inp|%cname)\\b")
    private val CIN_DIRECTIVE = Regex("(?m)^%(chardef|gen_inp|cname)\\b")

    fun import(files: List<NamedBytes>): ImportResult {
        val decoded = files.map { it to it.bytes.toString(Charsets.UTF_8) }
        val byFormat = decoded.groupBy { (_, text) -> detect(text) }
        byFormat[TableFormat.UNKNOWN]?.let { unknown ->
            throw TableImportException("無法辨識的字表格式：${unknown.joinToString { it.first.name }}")
        }
        val ibus = byFormat[TableFormat.IBUS].orEmpty()
        val cin = byFormat[TableFormat.CIN].orEmpty()
        val tsv = byFormat[TableFormat.NEUTRAL_TSV].orEmpty()

        val rawSections: List<TableSection> = when (Triple(ibus.size, cin.size, tsv.size)) {
            Triple(1, 1, 0) -> try {
                SectionSplitter.split(
                    IbusTableParser.parse(ibus.single().second.lineSequence()),
                    CinParser.parse(cin.single().second.lineSequence()),
                )
            } catch (e: SectionMismatchException) {
                throw TableImportException("IBus 與 CIN 區段對不上：${e.message}")
            }
            Triple(0, 1, 0) -> CinParser.parse(cin.single().second.lineSequence()).mapIndexed { i, s ->
                TableSection(SectionSplitter.guessKind(i, s.cname, s.ename, s.entries), s.cname, s.entries)
            }
            Triple(0, 0, 1) -> parseNeutral(tsv.single().second)
            Triple(1, 0, 0) -> throw TableImportException("只有 IBus 表無法切分區段，請同時選擇 lime_liu7.txt（CIN／LIME 格式）")
            else -> throw TableImportException(
                "不支援的檔案組合（IBus ${ibus.size}、CIN ${cin.size}、TSV ${tsv.size}）：" +
                    "請選 IBus＋CIN、單一 CIN，或單一中性 TSV",
            )
        }

        val normalized = rawSections.map { it.copy(entries = TableNormalizer.normalize(it.entries)) }
        val allStats = normalized.mapIndexed { i, s -> TableNormalizer.stats(s, rawSections[i].entries.size) }
        val tradIdx = normalized.indexOfFirst { it.kind == SectionKind.TRADITIONAL }
        if (tradIdx < 0) throw TableImportException("找不到繁中區段")
        // 嘸蝦米在一般模式以「羅馬拼音＋,」輸入平假名、「＋.」輸入片假名：把日文區段的假名字碼併入繁中查詢；
        // 日文區段的其他內容（日文漢字）不併入。
        val kana = normalized.firstOrNull { it.kind == SectionKind.JAPANESE }?.entries.orEmpty().filter(::isKanaCode)
        val main = normalized[tradIdx].let { it.copy(entries = TableNormalizer.normalize(it.entries + kana)) }
        // 語言模式（无／台／日）各用自己的區段，照原樣保留（每種各取第一段）
        val modes = LANGUAGE_KINDS.mapNotNull { kind -> normalized.indexOfFirst { it.kind == kind }.takeIf { it >= 0 } }
        val bundle = TableBundle(
            sections = listOf(main) + modes.map { normalized[it] },
            sources = files.map { SourceFile(it.name, sha256Hex(it.bytes)) },
            stats = listOf(TableNormalizer.stats(main, rawSections[tradIdx].entries.size + kana.size)) + modes.map { allStats[it] },
        )
        return ImportResult(bundle, allStats)
    }

    private fun isKanaCode(e: TableEntry): Boolean = e.code.last() in KANA_CODE_SUFFIXES && e.text.all(Kana::isKana)

    private const val KANA_CODE_SUFFIXES = ",."
    private val LANGUAGE_KINDS = listOf(SectionKind.SIMPLIFIED, SectionKind.TW_SIMPLIFIED, SectionKind.JAPANESE)

    fun writeNeutral(sections: List<TableSection>): String = buildString {
        append(NEUTRAL_HEADER).append('\n')
        for (s in sections) {
            for (e in s.entries) {
                append(s.kind.name).append('\t').append(e.code).append('\t').append(e.text).append('\t').append(e.freq).append('\n')
            }
        }
    }

    private fun parseNeutral(text: String): List<TableSection> {
        val grouped = LinkedHashMap<SectionKind, MutableList<TableEntry>>()
        text.lineSequence().drop(1).filter { it.isNotBlank() }.forEach { line ->
            val cols = line.split('\t')
            if (cols.size < 3) throw TableImportException("中性 TSV 欄位不足：$line")
            val kind = runCatching { SectionKind.valueOf(cols[0]) }.getOrElse {
                throw TableImportException("未知的區段類型：${cols[0]}")
            }
            grouped.getOrPut(kind) { ArrayList() } += TableEntry(cols[1], cols[2], cols.getOrNull(3)?.toLongOrNull() ?: 0)
        }
        return grouped.map { (kind, entries) -> TableSection(kind, null, entries) }
    }
}
