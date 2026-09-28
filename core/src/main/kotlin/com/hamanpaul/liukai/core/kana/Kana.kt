package com.hamanpaul.liukai.core.kana

/** 平假名與片假名的判定與轉換（Unicode 平假名區塊與片假名區塊相差 0x60）。 */
object Kana {
    private const val HIRAGANA_START = 0x3041
    private const val HIRAGANA_END = 0x3096
    private const val OFFSET = 0x60

    fun isHiragana(c: Char): Boolean = c.code in HIRAGANA_START..HIRAGANA_END || c == 'ゝ' || c == 'ゞ'

    fun isKatakana(c: Char): Boolean = c.code in 0x30A1..0x30FA || c == 'ヽ' || c == 'ヾ' || c == 'ー'

    fun isKana(c: Char): Boolean = isHiragana(c) || isKatakana(c)

    /** 整串都是平假名（長音符「ー」也允許）。 */
    fun isAllHiragana(s: String): Boolean = s.isNotEmpty() && s.all { isHiragana(it) || it == 'ー' } && s.any { isHiragana(it) }

    fun toKatakana(s: String): String = buildString(s.length) {
        for (c in s) {
            append(
                when {
                    c.code in HIRAGANA_START..HIRAGANA_END -> (c.code + OFFSET).toChar()
                    c == 'ゝ' -> 'ヽ'
                    c == 'ゞ' -> 'ヾ'
                    else -> c
                },
            )
        }
    }
}
