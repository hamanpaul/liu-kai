package com.hamanpaul.liukai.core.table

private val WHITESPACE = Regex("\\s+")

private fun String.stripBom(): String = removePrefix("﻿")

/** 以 tab 為主、空白為輔切出「字碼 輸出字 [頻率]」三欄。 */
private fun splitColumns(line: String): List<String> =
    if ('\t' in line) line.split('\t').map { it.trim() } else line.trim().split(WHITESPACE)

/**
 * SCIM／IBus table 文字格式：只讀 BEGIN_TABLE 與 END_TABLE 之間的資料列，
 * 欄位為 code、phrase、freq（freq 可省略）。
 */
object IbusTableParser {
    fun parse(lines: Sequence<String>): List<TableEntry> {
        var inTable = false
        val out = ArrayList<TableEntry>()
        for (raw in lines) {
            val line = raw.stripBom().trimEnd('\r')
            when {
                line.trim() == "BEGIN_TABLE" -> inTable = true
                line.trim() == "END_TABLE" -> inTable = false
                !inTable || line.isBlank() || line.startsWith("###") -> Unit
                else -> {
                    val cols = splitColumns(line)
                    if (cols.size >= 2 && cols[0].isNotEmpty() && cols[1].isNotEmpty()) {
                        out += TableEntry(cols[0], cols[1], cols.getOrNull(2)?.toLongOrNull() ?: 0)
                    }
                }
            }
        }
        return out
    }
}

/**
 * CIN／LIME 文字格式中的一個區段（尚未判定用途）。
 * keynames 為 `%keyname begin…end` 內的按鍵顯示名稱（例如 a → Ａ），不是字碼。
 */
data class CinSection(
    val cname: String?,
    val ename: String?,
    val entries: List<TableEntry>,
    val keynames: List<TableEntry> = emptyList(),
)

/**
 * CIN／LIME 文字格式，可含多個串接區段：
 * - `%gen_inp` 開啟新區段（目前區段已有內容時）；檔案開頭可以沒有標頭。
 * - `%cname`／`%ename` 設定區段名稱；`%keyname begin…end` 內的行收進 keynames（不當字碼）。
 * - 其餘非 `%`、非 `#` 的非空行都視為「字碼 輸出字」資料列（`%chardef begin/end` 只是標記）。
 */
object CinParser {
    fun parse(lines: Sequence<String>): List<CinSection> {
        val sections = ArrayList<CinSection>()
        var cname: String? = null
        var ename: String? = null
        var entries = ArrayList<TableEntry>()
        var keynames = ArrayList<TableEntry>()
        var inKeyname = false

        fun flush() {
            if (entries.isNotEmpty() || cname != null || ename != null) {
                sections += CinSection(cname, ename, entries, keynames)
            }
            cname = null
            ename = null
            entries = ArrayList()
            keynames = ArrayList()
        }

        for (raw in lines) {
            val line = raw.stripBom().trimEnd('\r')
            val trimmed = line.trim()
            if (trimmed.isEmpty() || trimmed.startsWith("#")) continue
            if (inKeyname) {
                // 區塊內任何 %keyname 指令（正常為 %keyname end）都結束區塊
                if (trimmed.startsWith("%keyname")) {
                    inKeyname = false
                } else {
                    val cols = splitColumns(line)
                    if (cols.size >= 2) keynames += TableEntry(cols[0], cols[1])
                }
                continue
            }
            if (trimmed.startsWith("%")) {
                val directive = trimmed.substringBefore(' ').substringBefore('\t')
                val arg = trimmed.substring(directive.length).trim()
                when (directive) {
                    "%gen_inp" -> if (entries.isNotEmpty()) flush()
                    "%cname" -> cname = arg
                    "%ename" -> ename = arg
                    "%keyname" -> if (arg == "begin") inKeyname = true
                    else -> Unit
                }
                continue
            }
            val cols = splitColumns(line)
            if (cols.size >= 2 && cols[0].isNotEmpty() && cols[1].isNotEmpty()) {
                entries += TableEntry(cols[0], cols[1])
            }
        }
        flush()
        return sections
    }
}
