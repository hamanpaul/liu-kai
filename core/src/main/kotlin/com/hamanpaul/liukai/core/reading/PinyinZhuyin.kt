package com.hamanpaul.liukai.core.reading

/** 帶聲調符號的漢語拼音（Unihan kMandarin 格式）轉注音符號；無法轉換時回傳 null。 */
object PinyinZhuyin {
    private val TONED = mapOf(
        'ā' to ('a' to 1), 'á' to ('a' to 2), 'ǎ' to ('a' to 3), 'à' to ('a' to 4),
        'ē' to ('e' to 1), 'é' to ('e' to 2), 'ě' to ('e' to 3), 'è' to ('e' to 4),
        'ī' to ('i' to 1), 'í' to ('i' to 2), 'ǐ' to ('i' to 3), 'ì' to ('i' to 4),
        'ō' to ('o' to 1), 'ó' to ('o' to 2), 'ǒ' to ('o' to 3), 'ò' to ('o' to 4),
        'ū' to ('u' to 1), 'ú' to ('u' to 2), 'ǔ' to ('u' to 3), 'ù' to ('u' to 4),
        'ǖ' to ('ü' to 1), 'ǘ' to ('ü' to 2), 'ǚ' to ('ü' to 3), 'ǜ' to ('ü' to 4),
        'ń' to ('n' to 2), 'ň' to ('n' to 3), 'ǹ' to ('n' to 4), 'ḿ' to ('m' to 2),
    )

    private val INITIALS = listOf(
        "zh" to "ㄓ", "ch" to "ㄔ", "sh" to "ㄕ",
        "b" to "ㄅ", "p" to "ㄆ", "m" to "ㄇ", "f" to "ㄈ", "d" to "ㄉ", "t" to "ㄊ", "n" to "ㄋ", "l" to "ㄌ",
        "g" to "ㄍ", "k" to "ㄎ", "h" to "ㄏ", "j" to "ㄐ", "q" to "ㄑ", "x" to "ㄒ", "r" to "ㄖ",
        "z" to "ㄗ", "c" to "ㄘ", "s" to "ㄙ",
    )

    private val FINALS = mapOf(
        "a" to "ㄚ", "o" to "ㄛ", "e" to "ㄜ", "ê" to "ㄝ", "ai" to "ㄞ", "ei" to "ㄟ", "ao" to "ㄠ", "ou" to "ㄡ",
        "an" to "ㄢ", "en" to "ㄣ", "ang" to "ㄤ", "eng" to "ㄥ", "er" to "ㄦ", "ong" to "ㄨㄥ",
        "i" to "ㄧ", "ia" to "ㄧㄚ", "io" to "ㄧㄛ", "ie" to "ㄧㄝ", "iai" to "ㄧㄞ", "iao" to "ㄧㄠ", "iu" to "ㄧㄡ",
        "ian" to "ㄧㄢ", "in" to "ㄧㄣ", "iang" to "ㄧㄤ", "ing" to "ㄧㄥ", "iong" to "ㄩㄥ",
        "u" to "ㄨ", "ua" to "ㄨㄚ", "uo" to "ㄨㄛ", "uai" to "ㄨㄞ", "ui" to "ㄨㄟ", "uan" to "ㄨㄢ", "un" to "ㄨㄣ",
        "uang" to "ㄨㄤ", "ueng" to "ㄨㄥ",
        "ü" to "ㄩ", "üe" to "ㄩㄝ", "üan" to "ㄩㄢ", "ün" to "ㄩㄣ",
    )

    private val Y_W = mapOf(
        "yi" to "i", "ya" to "ia", "yo" to "io", "ye" to "ie", "yai" to "iai", "yao" to "iao", "you" to "iu",
        "yan" to "ian", "yin" to "in", "yang" to "iang", "ying" to "ing", "yong" to "iong",
        "yu" to "ü", "yue" to "üe", "yuan" to "üan", "yun" to "ün",
        "wu" to "u", "wa" to "ua", "wo" to "uo", "wai" to "uai", "wei" to "ui", "wan" to "uan", "wen" to "un",
        "wang" to "uang", "weng" to "ueng",
    )

    private val TONE_MARK = mapOf(1 to "", 2 to "ˊ", 3 to "ˇ", 4 to "ˋ")

    fun convert(pinyin: String): String? {
        var tone = 5
        val base = buildString {
            for (c in pinyin.lowercase().trim()) {
                val t = TONED[c]
                if (t != null) {
                    append(t.first)
                    tone = t.second
                } else {
                    append(c)
                }
            }
        }.replace("u:", "ü").replace('v', 'ü')
        if (base.isEmpty() || base.any { it !in 'a'..'z' && it != 'ü' && it != 'ê' }) return null

        val body = when (base) {
            "n" -> "ㄣ"
            "m" -> "ㄇ"
            "ng" -> "ㄫ"
            else -> syllable(base) ?: return null
        }
        return if (tone == 5) "˙$body" else body + TONE_MARK.getValue(tone)
    }

    private fun syllable(base: String): String? {
        Y_W[base]?.let { return FINALS[it] }
        val initial = INITIALS.firstOrNull { base.startsWith(it.first) }
        if (initial == null) return FINALS[base]
        var rest = base.substring(initial.first.length)
        if (rest == "i" && initial.first in setOf("zh", "ch", "sh", "r", "z", "c", "s")) return initial.second
        if (initial.first in setOf("j", "q", "x") && rest.startsWith("u")) rest = "ü" + rest.substring(1)
        val fin = FINALS[rest] ?: return null
        return initial.second + fin
    }
}
