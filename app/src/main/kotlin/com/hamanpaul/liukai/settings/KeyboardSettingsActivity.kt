package com.hamanpaul.liukai.settings

import android.app.Activity
import android.app.AlertDialog
import android.os.Bundle
import android.view.WindowManager
import android.widget.EditText
import com.hamanpaul.liukai.data.FontSize
import com.hamanpaul.liukai.data.ImePrefs
import com.hamanpaul.liukai.data.KeyHeight
import com.hamanpaul.liukai.data.KeyboardTheme
import com.hamanpaul.liukai.data.VoiceMode

/**
 * 嘸蝦米鍵盤設定：項目、順序與文字照使用者手機上的官方非 PRO「嘸蝦米輸入法」2.6.8 的設定頁。
 * 英文的拼字建議、自動完成、輕觸修正，中文的關聯字詞不實作（使用者 2026-09-27、09-28 裁決）；HTC Desire Z 切換鍵是
 * 舊機型實體鍵的支援，不適用；
 * 官方 PRO 的主題與閒置常用標點列保留為選用設定（使用者 2026-09-28 裁決），放在「其他設定」。
 */
class KeyboardSettingsActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val s = ImePrefs.settings(this)
        val ui = SettingsUi(this)
        ui.check("按鍵時震動", null, ImePrefs.VIBRATE, s.vibrate)
        ui.check("按鍵時播放音效", null, ImePrefs.SOUND, s.sound)
        ui.check("按鍵時顯示彈出式視窗", null, ImePrefs.KEY_PREVIEW, s.keyPreview)
        ui.check("自動返回字元輸入模式", "在符號模式，輸入空白或換行之後，自動返回字元鍵盤", ImePrefs.AUTO_RETURN, s.autoReturn)
        ui.choiceDialog("顯示退出鍵盤鍵", ImePrefs.DONE_KEY, listOf("永遠顯示", "永遠隱藏"), listOf("true", "false"), s.doneKey.toString())
        val voices = VoiceMode.entries
        ui.choiceDialog("語音輸入", ImePrefs.VOICE, voices.map { it.label }, voices.map { it.name }, s.voice.name, voices.map { it.summary })
        ui.section("英文輸入設定")
        ui.check("自動大寫", null, ImePrefs.AUTO_CAP, s.autoCap)
        ui.section("中文輸入設定")
        ui.check("智慧鍵盤", null, ImePrefs.SMART_KEYBOARD, s.smartKeyboard)
        ui.section("鍵盤配置設定")
        val heights = KeyHeight.entries
        val fonts = FontSize.entries
        fun height(title: String, key: String, v: KeyHeight) =
            ui.choiceDialog(title, key, heights.map { it.label }, heights.map { it.name }, v.name)
        fun font(title: String, key: String, v: FontSize) =
            ui.choiceDialog(title, key, fonts.map { it.label }, fonts.map { it.name }, v.name)
        height("直式鍵盤高度", ImePrefs.PORTRAIT_HEIGHT, s.portraitHeight)
        font("直式鍵大小", ImePrefs.PORTRAIT_FONT, s.portraitFont)
        height("橫式鍵盤高度", ImePrefs.LANDSCAPE_HEIGHT, s.landscapeHeight)
        font("橫式鍵大小", ImePrefs.LANDSCAPE_FONT, s.landscapeFont)
        ui.link("檢視鍵盤", "點擊可檢視鍵盤外觀") { viewKeyboard() }
        ui.section("其他設定")
        val themes = KeyboardTheme.entries
        ui.choiceDialog("鍵盤主題", ImePrefs.THEME, themes.map { it.label }, themes.map { it.name }, s.theme.name)
        ui.check("顯示按鍵", null, ImePrefs.SHOW_KEYS, s.showKeys)
        ui.check("閒置時顯示常用標點", "沒有組字時，候選列顯示常用標點與麥克風", ImePrefs.IDLE_STRIP, s.idleStrip)
        ui.section("關於")
        ui.info("liu-kai v${packageManager.getPackageInfo(packageName, 0).versionName}", null)
        ui.show()
    }

    /** 官方「檢視鍵盤」：對話框內的輸入欄取得焦點並叫出鍵盤，套用目前的設定。 */
    private fun viewKeyboard() {
        val field = EditText(this)
        val dialog = AlertDialog.Builder(this)
            .setTitle("檢視鍵盤")
            .setView(field)
            .setNegativeButton("取消", null)
            .setPositiveButton("確定", null)
            .create()
        dialog.window!!.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        dialog.show()
        field.requestFocus()
    }
}
