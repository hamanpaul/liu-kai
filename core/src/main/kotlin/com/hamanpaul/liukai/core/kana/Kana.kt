package com.hamanpaul.liukai.core.kana

/** 假名判定：Unicode 平假名區塊（U+3041 起）與片假名區塊（至 U+30FF）。 */
object Kana {
    fun isKana(c: Char): Boolean = c.code in 0x3041..0x30FF
}
