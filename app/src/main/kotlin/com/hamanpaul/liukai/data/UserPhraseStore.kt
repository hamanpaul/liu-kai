package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.table.UserPhrase
import com.hamanpaul.liukai.core.table.UserPhrases
import java.io.File

/** 加字加詞的自訂字詞：存成 app 私有的 TSV（拆碼、字詞），載入字表時併入繁中查詢。 */
object UserPhraseStore {
    private const val FILE_NAME = "user_phrases.tsv"

    private fun file(context: Context) = File(context.filesDir, FILE_NAME)

    fun load(context: Context): List<UserPhrase> {
        val f = file(context)
        return if (f.exists()) UserPhrases.parse(f.readText()) else emptyList()
    }

    fun save(context: Context, phrases: List<UserPhrase>) {
        val f = file(context)
        val tmp = File(f.parentFile, "$FILE_NAME.tmp")
        tmp.writeText(UserPhrases.serialize(phrases))
        tmp.renameTo(f)
    }

    /** 檔案的變更戳記（字表快取用）；沒有檔案時為 0。 */
    fun stamp(context: Context): Long = file(context).let { it.lastModified() xor it.length() }
}
