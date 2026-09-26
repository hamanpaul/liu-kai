package com.hamanpaul.liukai.core.table

import com.hamanpaul.liukai.core.kana.Kana

class SectionMismatchException(message: String) : IllegalArgumentException(message)

/**
 * 以 CIN／LIME 的區段邊界切分 IBus 資料流：
 * 各區段 (code,text) 依序串接後必須與 IBus 資料逐筆相同，否則拒絕（不猜測邊界）。
 * 切分結果沿用 IBus 的頻率欄。
 */
object SectionSplitter {
    fun split(ibus: List<TableEntry>, sections: List<CinSection>): List<TableSection> {
        val total = sections.sumOf { it.entries.size }
        if (total != ibus.size) {
            throw SectionMismatchException("區段筆數合計 $total 與 IBus 筆數 ${ibus.size} 不符")
        }
        val out = ArrayList<TableSection>(sections.size)
        var cursor = 0
        sections.forEachIndexed { index, section ->
            val merged = section.entries.map { cin ->
                val ib = ibus[cursor]
                if (!cin.code.equals(ib.code, ignoreCase = true) || cin.text != ib.text) {
                    throw SectionMismatchException(
                        "第 ${cursor + 1} 筆不一致：CIN(${cin.code},${cin.text}) vs IBus(${ib.code},${ib.text})",
                    )
                }
                cursor++
                ib
            }
            out += TableSection(guessKind(index, section.cname, section.ename, merged), section.cname, merged)
        }
        return out
    }

    /** 第一段視為繁中；名稱含「日」／jp，或輸出字多為假名者視為日文；其餘為 OTHER。 */
    fun guessKind(index: Int, cname: String?, ename: String?, entries: List<TableEntry>): SectionKind {
        val names = listOfNotNull(cname, ename).joinToString(" ").lowercase()
        if ("日" in names || "jp" in names || "japan" in names) return SectionKind.JAPANESE
        if (entries.isNotEmpty()) {
            val kana = entries.count { e -> e.text.any { Kana.isKana(it) } }
            if (kana * 3 >= entries.size) return SectionKind.JAPANESE
        }
        return if (index == 0) SectionKind.TRADITIONAL else SectionKind.OTHER
    }
}

/** 每區段的統計，寫入匯入報告與 manifest。 */
data class SectionStats(
    val kind: SectionKind,
    val name: String?,
    val rawRows: Int,
    val uniquePairs: Int,
    val uniqueCodes: Int,
    val uniqueTexts: Int,
)

object TableNormalizer {
    /**
     * 字碼轉小寫；同一 (code,text) 去重並保留首次出現的位置（即候選順位），頻率取最大值。
     */
    fun normalize(entries: List<TableEntry>): List<TableEntry> {
        val index = LinkedHashMap<Pair<String, String>, TableEntry>()
        for (e in entries) {
            val key = e.code.lowercase() to e.text
            val prev = index[key]
            index[key] = if (prev == null) e.copy(code = key.first) else prev.copy(freq = maxOf(prev.freq, e.freq))
        }
        return index.values.toList()
    }

    /** section 須為已正規化的區段；rawRows 為正規化前的原始列數。 */
    fun stats(section: TableSection, rawRows: Int): SectionStats {
        val norm = section.entries
        return SectionStats(
            kind = section.kind,
            name = section.name,
            rawRows = rawRows,
            uniquePairs = norm.size,
            uniqueCodes = norm.map { it.code }.toSet().size,
            uniqueTexts = norm.map { it.text }.toSet().size,
        )
    }
}
