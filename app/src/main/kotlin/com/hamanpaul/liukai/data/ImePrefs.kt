package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.engine.Language

/** 按鍵高度（官方「按鍵高度設定」五段）。 */
enum class KeyHeight(val label: String) { HIGH("高"), SLIGHTLY_HIGH("稍高"), MEDIUM("適中"), SLIGHTLY_LOW("稍低"), LOW("低") }

/** 按鍵字體大小（官方五段）。 */
enum class FontSize(val label: String) { LARGE("大"), SLIGHTLY_LARGE("稍大"), MEDIUM("適中"), SLIGHTLY_SMALL("稍小"), SMALL("小") }

/** 語音輸入（官方「語音輸入」）：麥克風放在主鍵盤、符號鍵盤或關閉；summary 為設定頁說明列。 */
enum class VoiceMode(val label: String, val summary: String) {
    MAIN("於主鍵盤", "主鍵盤上的麥克風"), SYMBOLS("於符號鍵盤", "符號鍵盤上的麥克風"), OFF("關閉", "已停用語音輸入"),
}

/** 鍵盤主題（官方 PRO 十種；非 PRO 只有經典灰，使用者 2026-09-28 決定保留為選用設定）。 */
enum class KeyboardTheme(val label: String) {
    WHITE("質感白"), BLACK("沉勁黑"), GRAY("經典灰"), RED("魅力紅"), GREEN("奇想綠"),
    BLUE("星空藍"), PURPLE("甜蜜紫"), PINK("櫻花粉"), YELLOW("靜謐黃"), COCOA("濃可可"),
}

/**
 * 鍵盤設定（照使用者手機上的官方非 PRO「嘸蝦米輸入法」2.6.8 的「嘸蝦米鍵盤設定」）。預設值：外觀照使用者手機上的
 * 官方設定（經典灰、顯示按鍵、直式按鍵高），其餘照官方預設。主題與閒置常用標點列是官方 PRO 的功能，保留為選用設定。
 */
data class KeyboardSettings(
    val vibrate: Boolean = false,
    val sound: Boolean = false,
    /** 按鍵時顯示彈出式視窗（放大預覽）。 */
    val keyPreview: Boolean = true,
    /** 在符號模式輸入符號後按空白或換行，自動返回字元鍵盤。 */
    val autoReturn: Boolean = true,
    /** 顯示退出鍵盤鍵（官方「永遠顯示／永遠隱藏」）。 */
    val doneKey: Boolean = false,
    /** 英文自動大寫。 */
    val autoCap: Boolean = true,
    /** 智慧鍵盤：組字中不可能的下一碼留白。 */
    val smartKeyboard: Boolean = true,
    /** 語音輸入的麥克風位置。 */
    val voice: VoiceMode = VoiceMode.MAIN,
    val portraitHeight: KeyHeight = KeyHeight.HIGH,
    val portraitFont: FontSize = FontSize.MEDIUM,
    val landscapeHeight: KeyHeight = KeyHeight.MEDIUM,
    val landscapeFont: FontSize = FontSize.MEDIUM,
    val theme: KeyboardTheme = KeyboardTheme.GRAY,
    /** 顯示按鍵（按鍵獨立分格）。 */
    val showKeys: Boolean = true,
    /** 閒置時候選列顯示常用標點與麥克風（官方 PRO）。 */
    val idleStrip: Boolean = false,
)

/** 輸入法的使用者偏好（app 私有 SharedPreferences，值一律存成字串）。 */
object ImePrefs {
    private const val FILE = "liukai"
    const val LANGUAGE = "language"
    const val VIBRATE = "vibrate"
    const val SOUND = "sound"
    const val KEY_PREVIEW = "key_preview"
    const val AUTO_RETURN = "auto_return"
    const val DONE_KEY = "done_key"
    const val AUTO_CAP = "auto_cap"
    const val SMART_KEYBOARD = "smart_keyboard"
    const val PORTRAIT_HEIGHT = "portrait_height"
    const val PORTRAIT_FONT = "portrait_font"
    const val LANDSCAPE_HEIGHT = "landscape_height"
    const val LANDSCAPE_FONT = "landscape_font"
    const val VOICE = "voice"
    const val THEME = "theme"
    const val SHOW_KEYS = "show_keys"
    const val IDLE_STRIP = "idle_strip"

    private fun prefs(context: Context) = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    private fun get(context: Context, key: String, default: Any): String =
        prefs(context).getString(key, default.toString())!!

    /** 語言模式（中文模式左右滑動空白鍵選的 嘸／无／台／日），預設繁中。 */
    fun language(context: Context): Language = Language.valueOf(get(context, LANGUAGE, Language.TRADITIONAL))

    fun setLanguage(context: Context, language: Language) = set(context, LANGUAGE, language.name)

    fun settings(context: Context): KeyboardSettings {
        val d = KeyboardSettings()
        fun bool(key: String, default: Boolean) = get(context, key, default).toBoolean()
        return KeyboardSettings(
            vibrate = bool(VIBRATE, d.vibrate),
            sound = bool(SOUND, d.sound),
            keyPreview = bool(KEY_PREVIEW, d.keyPreview),
            autoReturn = bool(AUTO_RETURN, d.autoReturn),
            doneKey = bool(DONE_KEY, d.doneKey),
            autoCap = bool(AUTO_CAP, d.autoCap),
            smartKeyboard = bool(SMART_KEYBOARD, d.smartKeyboard),
            voice = VoiceMode.valueOf(get(context, VOICE, d.voice)),
            portraitHeight = KeyHeight.valueOf(get(context, PORTRAIT_HEIGHT, d.portraitHeight)),
            portraitFont = FontSize.valueOf(get(context, PORTRAIT_FONT, d.portraitFont)),
            landscapeHeight = KeyHeight.valueOf(get(context, LANDSCAPE_HEIGHT, d.landscapeHeight)),
            landscapeFont = FontSize.valueOf(get(context, LANDSCAPE_FONT, d.landscapeFont)),
            theme = KeyboardTheme.valueOf(get(context, THEME, d.theme)),
            showKeys = bool(SHOW_KEYS, d.showKeys),
            idleStrip = bool(IDLE_STRIP, d.idleStrip),
        )
    }

    fun set(context: Context, key: String, value: String) {
        prefs(context).edit().putString(key, value).apply()
    }

    /** 清除所有偏好，回到預設值。 */
    fun reset(context: Context) {
        prefs(context).edit().clear().apply()
    }
}
