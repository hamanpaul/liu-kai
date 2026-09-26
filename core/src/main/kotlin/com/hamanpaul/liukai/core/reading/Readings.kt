package com.hamanpaul.liukai.core.reading

import com.hamanpaul.liukai.core.table.CompiledTable

/**
 * 字 → 注音讀音（第一個為主要讀音）。資料格式為每行「字<TAB>讀音1 讀音2…」。
 * 同音字以「主要讀音相同」判定，範圍限於字表中存在的單字，依字表頻率排序。
 */
class Readings(private val map: Map<String, List<String>>) {
    val size: Int get() = map.size

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
                val primary = readings.firstOrNull() ?: continue
                if (table.containsText(ch)) grouped.getOrPut(primary) { ArrayList() } += ch
            }
            index = grouped.mapValues { (_, chars) ->
                chars.sortedWith(compareByDescending<String> { table.freqOf(it) }.thenBy { table.codesOf(it).firstOrNull() ?: "" })
            }
            indexedFor = table
        }
        return index
    }

    companion object {
        val EMPTY = Readings(emptyMap())

        fun parse(lines: Sequence<String>): Readings {
            val map = HashMap<String, List<String>>()
            for (line in lines) {
                if (line.isBlank() || line.startsWith("#")) continue
                val tab = line.indexOf('\t')
                if (tab <= 0) continue
                val readings = line.substring(tab + 1).trim().split(' ').filter { it.isNotEmpty() }
                if (readings.isNotEmpty()) map[line.substring(0, tab)] = readings
            }
            return Readings(map)
        }

        /** 讀取打包在 core 內的 `/readings.tsv`（由 cli gen-readings 產生）；不存在時回傳空資料。 */
        fun loadBundled(): Readings {
            val stream = Readings::class.java.getResourceAsStream("/readings.tsv") ?: return EMPTY
            return stream.bufferedReader(Charsets.UTF_8).use { parse(it.lineSequence()) }
        }
    }
}
