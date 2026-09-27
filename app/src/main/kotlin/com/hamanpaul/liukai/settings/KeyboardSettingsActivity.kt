package com.hamanpaul.liukai.settings

import android.app.Activity
import android.content.Intent
import com.hamanpaul.liukai.data.ImePrefs

/**
 * 嘸蝦米鍵盤設定：項目與順序照官方 PRO 3.0.9 的設定頁。英文的拼字建議、自動完成、輕觸修正，
 * 中文的關聯字詞，以及授權相關項目不實作（使用者 2026-09-27 裁決）。
 */
class KeyboardSettingsActivity : Activity() {
    override fun onResume() {
        super.onResume()
        // 回到本頁時重建，反映子頁的變更
        val s = ImePrefs.settings(this)
        val ui = SettingsUi(this)
        ui.link("設定主題及配置", "可自定主題及配置") { startActivity(Intent(this, ThemeSettingsActivity::class.java)) }
        ui.check("按鍵時震動", null, ImePrefs.VIBRATE, s.vibrate)
        ui.check("按鍵時播放音效", null, ImePrefs.SOUND, s.sound)
        ui.check("按鍵時顯示彈出式視窗", null, ImePrefs.KEY_PREVIEW, s.keyPreview)
        ui.check("自動返回字元輸入模式", "在符號模式輸入空白或換行後，自動返回字元鍵盤", ImePrefs.AUTO_RETURN, s.autoReturn)
        ui.choiceDialog("顯示退出鍵盤鍵", ImePrefs.DONE_KEY, listOf("永遠顯示", "永遠隱藏"), listOf("true", "false"), s.doneKey.toString())
        ui.section("英文輸入設定")
        ui.check("自動大寫", "句號後第一個字、欄位第一個字自動大寫", ImePrefs.AUTO_CAP, s.autoCap)
        ui.section("中文輸入設定")
        ui.check("智慧鍵盤", "輸入時淡化不可能的下一碼", ImePrefs.SMART_KEYBOARD, s.smartKeyboard)
        ui.show()
    }
}
