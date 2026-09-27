package com.hamanpaul.liukai.core.table

/**
 * 查詢用的字表：字碼排序後以二分搜尋查候選、查前綴；另建字→碼反查與字頻。
 * 候選順序即來源順位（TableNormalizer 已保留首次出現順序）。
 */
class CompiledTable private constructor(
    private val codes: Array<String>,
    private val candidates: Array<Array<String>>,
    private val textFreq: Map<String, Long>,
    private val textCodes: Map<String, List<String>>,
) {
    val maxCodeLength: Int = codes.fold(0) { longest, code -> maxOf(longest, code.length) }
    val alphabet: Set<Char> = codes.flatMapTo(HashSet()) { it.toList() }

    private fun indexOf(code: String): Int = codes.binarySearch(code)

    fun isCode(code: String): Boolean = indexOf(code) >= 0

    fun candidates(code: String): List<String> {
        val i = indexOf(code)
        return if (i >= 0) candidates[i].asList() else emptyList()
    }

    /** 是否有任何字碼以 prefix 開頭（含完全相等）。 */
    fun hasPrefix(prefix: String): Boolean {
        val i = indexOf(prefix)
        if (i >= 0) return true
        val insertion = -i - 1
        return insertion < codes.size && codes[insertion].startsWith(prefix)
    }

    /** 以 prefix 為前綴的較長字碼的下一個字根（智慧鍵盤用）。 */
    fun nextChars(prefix: String): Set<Char> {
        val i = indexOf(prefix)
        var at = if (i >= 0) i + 1 else -i - 1
        val out = HashSet<Char>()
        while (at < codes.size && codes[at].startsWith(prefix)) {
            out += codes[at][prefix.length]
            at++
        }
        return out
    }

    fun containsText(text: String): Boolean = text in textFreq

    fun freqOf(text: String): Long = textFreq[text] ?: 0

    /** 字的所有字碼，短碼優先。 */
    fun codesOf(text: String): List<String> = textCodes[text] ?: emptyList()

    /**
     * 萬用字元查字：`*`（many）比對零到多個字根。
     * 回傳 (輸出字, 字碼)，同字只留第一次（短碼優先），最多 limit 筆。
     */
    fun wildcard(pattern: String, limit: Int, many: Char = '*'): List<Pair<String, String>> {
        val literalPrefix = pattern.takeWhile { it != many }
        val regex = Regex(pattern.map { if (it == many) ".*" else Regex.escape(it.toString()) }.joinToString(""))
        var start = indexOf(literalPrefix).let { if (it >= 0) it else -it - 1 }
        if (literalPrefix.isEmpty()) start = 0
        val matched = ArrayList<String>()
        var i = start
        while (i < codes.size && codes[i].startsWith(literalPrefix)) {
            if (regex.matches(codes[i])) matched += codes[i]
            i++
        }
        matched.sortWith(compareBy<String> { it.length }.thenBy { it })
        val seen = HashSet<String>()
        val out = ArrayList<Pair<String, String>>()
        for (code in matched) {
            for (text in candidates(code)) {
                if (seen.add(text)) {
                    out += text to code
                    if (out.size >= limit) return out
                }
            }
        }
        return out
    }

    companion object {
        fun build(entries: List<TableEntry>): CompiledTable {
            val byCode = LinkedHashMap<String, MutableList<String>>()
            val freq = HashMap<String, Long>()
            val reverse = HashMap<String, MutableList<String>>()
            for (e in entries) {
                val list = byCode.getOrPut(e.code) { ArrayList() }
                if (e.text !in list) list += e.text
                freq[e.text] = maxOf(freq[e.text] ?: 0, e.freq)
                val codes = reverse.getOrPut(e.text) { ArrayList() }
                if (e.code !in codes) codes += e.code
            }
            val sortedCodes = byCode.keys.sorted()
            return CompiledTable(
                codes = sortedCodes.toTypedArray(),
                candidates = Array(sortedCodes.size) { byCode.getValue(sortedCodes[it]).toTypedArray() },
                textFreq = freq,
                textCodes = reverse.mapValues { (_, v) -> v.sortedWith(compareBy<String> { it.length }.thenBy { it }) },
            )
        }
    }
}
