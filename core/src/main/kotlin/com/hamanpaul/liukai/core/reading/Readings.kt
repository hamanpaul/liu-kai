package com.hamanpaul.liukai.core.reading

import com.hamanpaul.liukai.core.table.CompiledTable

/**
 * 字 → 注音讀音（第一個為主要讀音）。資料格式為每行「字<TAB>讀音1 讀音2…[<TAB>年級]」，
 * 年級取自 Unihan kGradeLevel（香港小學學習年級，1 最常用；缺漏表示非基礎常用字）。
 * 同音字以「主要讀音相同」判定，範圍限於字表中存在的單字，依常用度排序：
 * 年級 → 最短字碼長度（嘸蝦米常用字多有短碼）→ 字表頻率 → 字碼。
 * 字表的頻率欄不一定是使用頻率（自建表只記同碼內順位），所以不作為主要依據。
 */
class Readings private constructor(private val map: Map<String, List<String>>, private val grades: Map<String, Int>) {
    fun of(ch: String): List<String> = map[ch].orEmpty()

    private var indexedFor: CompiledTable? = null
    private var index: Map<String, List<String>> = emptyMap()

    fun homophones(ch: String, table: CompiledTable): List<String> {
        val primary = of(ch).firstOrNull() ?: return emptyList()
        return indexFor(table)[primary].orEmpty()
    }

    @Synchronized
    private fun indexFor(table: CompiledTable): Map<String, List<String>> {
        if (indexedFor !== table) {
            val grouped = HashMap<String, MutableList<String>>()
            for ((ch, readings) in map) {
                // parse 保證每個字至少有一個讀音
                if (table.containsText(ch)) grouped.getOrPut(readings.first()) { ArrayList() } += ch
            }
            index = grouped.mapValues { (_, chars) ->
                chars.sortedWith(
                    compareBy<String> { grades[it] ?: Int.MAX_VALUE }
                        .thenBy { table.codesOf(it).firstOrNull()?.length ?: Int.MAX_VALUE }
                        .thenByDescending { table.freqOf(it) }
                        .thenBy { table.codesOf(it).firstOrNull() ?: "" },
                )
            }
            indexedFor = table
        }
        return index
    }

    companion object {
        val EMPTY = Readings(emptyMap(), emptyMap())

        fun parse(lines: Sequence<String>): Readings {
            val map = HashMap<String, List<String>>()
            val grades = HashMap<String, Int>()
            for (line in lines) {
                if (line.isBlank() || line.startsWith("#")) continue
                val cols = line.split('\t')
                if (cols.size < 2 || cols[0].isEmpty()) continue
                val readings = cols[1].trim().split(' ').filter { it.isNotEmpty() }
                if (readings.isEmpty()) continue
                map[cols[0]] = readings
                cols.getOrNull(2)?.trim()?.toIntOrNull()?.let { grades[cols[0]] = it }
            }
            return Readings(map, grades)
        }

        /** 讀取打包在 core 內的 `/readings.tsv`（由 cli gen-readings 產生，隨 core 一起發佈）。 */
        fun loadBundled(): Readings =
            parse(Readings::class.java.getResource("/readings.tsv")!!.readText(Charsets.UTF_8).lineSequence())
    }
}
