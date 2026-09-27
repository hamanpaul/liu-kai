package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.engine.Language

/** 輸入法的使用者偏好（app 私有 SharedPreferences）。 */
object ImePrefs {
    private const val FILE = "liukai"
    private const val LANGUAGE = "language"

    private fun prefs(context: Context) = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    /** 語言模式（長按「同音」選的 嘸／无／台／日），預設繁中。 */
    fun language(context: Context): Language =
        Language.valueOf(prefs(context).getString(LANGUAGE, Language.TRADITIONAL.name)!!)

    fun setLanguage(context: Context, language: Language) {
        prefs(context).edit().putString(LANGUAGE, language.name).apply()
    }
}
