package com.hamanpaul.liukai.core.table

/** 使用者自訂的字詞（官方「加字加詞」）：拆碼與字詞，同一拆碼可對應多個字詞。 */
data class UserPhrase(val code: String, val text: String)

/** 自訂字詞的規則、檔案格式（TSV：拆碼、字詞）與清單操作。 */
object UserPhrases {
    /** 拆碼照官方：可用英文字母、數字及「,」「.」，第一碼只能是英文字母或「,」「.」。 */
    fun validCode(code: String): Boolean =
        code.isNotEmpty() && code.all { it.isAsciiLetter() || it.isDigit() || it == ',' || it == '.' } && !code[0].isDigit()

    private fun Char.isAsciiLetter() = this in 'a'..'z' || this in 'A'..'Z'

    /** 解析 TSV（拆碼轉小寫；空行與 # 開頭的註解略過）；格式錯誤時回報列號。 */
    fun parse(text: String): List<UserPhrase> =
        text.lines().mapIndexedNotNull { i, line ->
            if (line.isBlank() || line.startsWith("#")) return@mapIndexedNotNull null
            val code = line.substringBefore('\t')
            val phrase = line.substringAfter('\t', "")
            require(validCode(code)) { "第 ${i + 1} 列：拆碼不合法（$code）" }
            require(phrase.isNotEmpty()) { "第 ${i + 1} 列：缺少字詞" }
            UserPhrase(code.lowercase(), phrase)
        }

    fun serialize(list: List<UserPhrase>): String = list.joinToString("") { "${it.code}\t${it.text}\n" }

    fun moveUp(list: List<UserPhrase>, index: Int): List<UserPhrase> =
        if (index <= 0) list else list.toMutableList().apply { add(index - 1, removeAt(index)) }

    fun moveDown(list: List<UserPhrase>, index: Int): List<UserPhrase> =
        if (index >= list.size - 1) list else list.toMutableList().apply { add(index + 1, removeAt(index)) }

    /** 與字表合併：自訂字詞排在前面，同碼時候選順序在字表之前。 */
    fun merge(entries: List<TableEntry>, phrases: List<UserPhrase>): List<TableEntry> =
        phrases.map { TableEntry(it.code, it.text) } + entries
}
