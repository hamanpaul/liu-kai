package com.hamanpaul.liukai.core.reading

/** A Zhuyin symbol produced by a physical key on the standard Taiwanese layout. */
data class ZhuyinKey(val physicalKey: Char, val glyph: String, internal val category: Int)

/** Physical-key mapping and reading composition shared by input routing and the soft keyboard. */
object ZhuyinKeyboard {
    private const val PHYSICAL_SYMBOL_KEYS = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-"
    private val symbols = listOf(
        "ㄅ", "ㄆ", "ㄇ", "ㄈ", "ㄉ", "ㄊ", "ㄋ", "ㄌ", "ㄍ", "ㄎ", "ㄏ", "ㄐ",
        "ㄑ", "ㄒ", "ㄓ", "ㄔ", "ㄕ", "ㄖ", "ㄗ", "ㄘ", "ㄙ", "ㄧ", "ㄨ", "ㄩ",
        "ㄚ", "ㄛ", "ㄜ", "ㄝ", "ㄞ", "ㄟ", "ㄠ", "ㄡ", "ㄢ", "ㄣ", "ㄤ", "ㄥ", "ㄦ",
    )
    private val symbolLayout = PHYSICAL_SYMBOL_KEYS.toList().zip(symbols).mapIndexed { index, (key, glyph) ->
        ZhuyinKey(key, glyph, categoryAt(index))
    }
    private val symbolByKey = symbolLayout.associate { it.physicalKey to it.glyph }
    private val categoryByKey = symbolLayout.associate { it.physicalKey to it.category }
    private val toneGlyphs = mapOf('6' to "ˊ", '3' to "ˇ", '4' to "ˋ", '7' to "˙")
    private val toneLayout = toneGlyphs.map { (key, glyph) -> ZhuyinKey(key, glyph, 3) }

    /** All physical symbols and tone keys from the pinned Windows layout. */
    val layout: List<ZhuyinKey> = symbolLayout + toneLayout

    fun glyphForKey(key: Char): String? {
        val normalized = normalize(key)
        return when {
            normalized == ' ' -> "ˉ"
            normalized in toneGlyphs -> toneGlyphs[normalized]
            else -> symbolByKey[normalized]
        }
    }

    fun isTone(key: Char): Boolean = normalize(key) in toneGlyphs

    fun isSymbol(key: Char): Boolean = normalize(key) in symbolByKey

    /** Add a physical key while keeping one symbol per category in initial/medial/final order. */
    fun appendCanonical(input: String, key: Char): String {
        val normalized = normalize(key)
        if (isTone(normalized)) {
            val existingTone = input.indexOfFirst(::isTone)
            return if (existingTone < 0) input + normalized else input.replaceRange(existingTone, existingTone + 1, normalized.toString())
        }
        if (!isSymbol(normalized)) return input

        val target = category(normalized)
        val existing = input.indexOfFirst { !isTone(it) && category(it) == target }
        if (existing >= 0) return input.replaceRange(existing, existing + 1, normalized.toString())

        val insertion = input.indexOfFirst { isTone(it) || category(it) > target }.let { if (it < 0) input.length else it }
        return input.substring(0, insertion) + normalized + input.substring(insertion)
    }

    /** Render canonical physical keys as Zhuyin glyphs. Neutral tone displays after the syllable. */
    fun display(input: String): String = input.toList()
        .sortedBy { if (isTone(it)) 3 else categoryByKey[normalize(it)] ?: 3 }
        .joinToString("") { glyphForKey(it).orEmpty() }

    /** Convert a canonical physical-key sequence to its normalized reading index key. */
    fun reading(input: String): String? {
        var hasTone = false
        var lastCategory = -1
        var tone = ""
        val body = StringBuilder()

        for (rawKey in input) {
            val key = normalize(rawKey)
            if (isTone(key)) {
                if (hasTone) return null
                hasTone = true
                tone = toneGlyphs.getValue(key)
                continue
            }
            if (hasTone || !isSymbol(key)) return null

            val category = categoryByKey.getValue(key)
            if (category <= lastCategory) return null
            lastCategory = category
            body.append(symbolByKey.getValue(key))
        }

        if (body.isEmpty()) return null
        val syllable = body.toString()
        return if (tone == "˙") tone + syllable else syllable + tone
    }

    private fun category(key: Char): Int = categoryByKey.getValue(normalize(key))

    // In physical-key order the 21 initials come first, then three medials,
    // twelve finals, and ㄦ in its editable initial-class slot at index 36.
    private fun categoryAt(index: Int): Int = when {
        index <= 20 || index == 36 -> 0
        index <= 23 -> 1
        else -> 2
    }

    private fun normalize(key: Char): Char = if (key in 'A'..'Z') key + ('a' - 'A') else key
}
