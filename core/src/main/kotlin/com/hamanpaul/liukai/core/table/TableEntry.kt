package com.hamanpaul.liukai.core.table

/** 一筆「字碼 → 輸出字」對應；freq 為來源表的頻率欄，沒有時為 0。 */
data class TableEntry(val code: String, val text: String, val freq: Long = 0)

/** 字表區段的用途。v0.1 只使用繁中與日文兩段，其餘保留但不載入引擎。 */
enum class SectionKind { TRADITIONAL, JAPANESE, OTHER }

/** 切分後的一個區段。name 取自 CIN 的 %cname（沒有時為 null）。 */
data class TableSection(val kind: SectionKind, val name: String?, val entries: List<TableEntry>)
