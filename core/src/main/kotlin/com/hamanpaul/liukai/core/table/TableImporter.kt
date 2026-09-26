package com.hamanpaul.liukai.core.table

/** 匯入時的一個來源檔（名稱只用於報告與格式判斷）。 */
class NamedBytes(val name: String, val bytes: ByteArray)

class TableImportException(message: String) : IllegalArgumentException(message)

/** 匯入結果：可存檔的字表，以及給使用者看的全區段報告。 */
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
        if (files.isEmpty()) throw TableImportException("沒有選擇任何檔案")
        val decoded = files.map { it to it.bytes.toString(Charsets.UTF_8) }
        val byFormat = decoded.groupBy { (_, text) -> detect(text) }
        byFormat[TableFormat.UNKNOWN]?.let { unknown ->
            throw TableImportException("無法辨識的字表格式：${unknown.joinToString { it.first.name }}")
        }
        val ibus = byFormat[TableFormat.IBUS].orEmpty()
        val cin = byFormat[TableFormat.CIN].orEmpty()
        val tsv = byFormat[TableFormat.NEUTRAL_TSV].orEmpty()
        if (ibus.size > 1 || cin.size > 1 || tsv.size > 1) throw TableImportException("同一種格式只能選一個檔案")

        val rawSections: List<TableSection> = when {
            tsv.isNotEmpty() && ibus.isEmpty() && cin.isEmpty() -> parseNeutral(tsv.single().second)
            ibus.isNotEmpty() && cin.isNotEmpty() && tsv.isEmpty() -> try {
                SectionSplitter.split(
                    IbusTableParser.parse(ibus.single().second.lineSequence()),
                    CinParser.parse(cin.single().second.lineSequence()),
                )
            } catch (e: SectionMismatchException) {
                throw TableImportException("IBus 與 CIN 區段對不上：${e.message}")
            }
            cin.isNotEmpty() && ibus.isEmpty() && tsv.isEmpty() ->
                CinParser.parse(cin.single().second.lineSequence()).mapIndexed { i, s ->
                    TableSection(SectionSplitter.guessKind(i, s.cname, s.ename, s.entries), s.cname, s.entries)
                }
            ibus.isNotEmpty() && cin.isEmpty() && tsv.isEmpty() ->
                throw TableImportException("只有 IBus 表無法切分區段，請同時選擇 lime_liu7.txt（CIN／LIME 格式）")
            else -> throw TableImportException("不支援的檔案組合")
        }

        val normalized = rawSections.map { it.copy(entries = TableNormalizer.normalize(it.entries)) }
        val allStats = normalized.mapIndexed { i, s -> TableNormalizer.stats(s, rawSections[i].entries.size) }
        val keepIdx = listOf(SectionKind.TRADITIONAL, SectionKind.JAPANESE).mapNotNull { kind ->
            normalized.indexOfFirst { it.kind == kind }.takeIf { it >= 0 }
        }
        if (keepIdx.none { normalized[it].kind == SectionKind.TRADITIONAL }) {
            throw TableImportException("找不到繁中區段")
        }
        val bundle = TableBundle(
            sections = keepIdx.map { normalized[it] },
            sources = files.map { SourceFile(it.name, sha256Hex(it.bytes)) },
            stats = keepIdx.map { allStats[it] },
        )
        return ImportResult(bundle, allStats)
    }

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
