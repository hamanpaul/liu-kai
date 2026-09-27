package com.hamanpaul.liukai.core.engine

import com.hamanpaul.liukai.core.table.CompiledTable
import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableBundle

/** 語言模式（官方長按「同音」的選單）：嘸（繁中）、无（簡體）、台（打繁出簡）、日（日文漢字與假名）。 */
enum class Language(val kind: SectionKind) {
    TRADITIONAL(SectionKind.TRADITIONAL),
    SIMPLIFIED(SectionKind.SIMPLIFIED),
    TW_SIMPLIFIED(SectionKind.TW_SIMPLIFIED),
    JAPANESE(SectionKind.JAPANESE),
}

/** 字表中繁中以外的語言模式字表（繁中另外編譯，並已併入假名字碼）。 */
fun TableBundle.languageTables(): Map<Language, CompiledTable> =
    Language.entries.drop(1).mapNotNull { lang -> section(lang.kind)?.let { lang to CompiledTable.build(it.entries) } }.toMap()
