package com.hamanpaul.liukai.settings

import android.app.Activity
import android.os.Bundle
import com.hamanpaul.liukai.data.FontSize
import com.hamanpaul.liukai.data.ImePrefs
import com.hamanpaul.liukai.data.KeyHeight
import com.hamanpaul.liukai.data.KeyboardTheme
import com.hamanpaul.liukai.settings.SettingsUi.SpinnerItem

/** 設定主題及配置：照官方的「按鍵高度設定」「鍵盤配置」「鍵盤主題」三區。 */
class ThemeSettingsActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val s = ImePrefs.settings(this)
        val ui = SettingsUi(this)
        val heights = KeyHeight.entries
        val fonts = FontSize.entries
        fun height(title: String, key: String, v: KeyHeight) =
            SpinnerItem(title, key, heights.map { it.label }, heights.map { it.name }, v.name)
        fun font(title: String, key: String, v: FontSize) =
            SpinnerItem(title, key, fonts.map { it.label }, fonts.map { it.name }, v.name)
        ui.section("按鍵高度設定")
        ui.spinners(height("直式按鍵", ImePrefs.PORTRAIT_HEIGHT, s.portraitHeight), font("直式字體", ImePrefs.PORTRAIT_FONT, s.portraitFont))
        ui.spinners(height("橫式按鍵", ImePrefs.LANDSCAPE_HEIGHT, s.landscapeHeight), font("橫式字體", ImePrefs.LANDSCAPE_FONT, s.landscapeFont))
        ui.section("鍵盤配置")
        ui.check("顯示數字鍵", null, ImePrefs.NUMBER_ROW, s.numberRow)
        ui.section("鍵盤主題")
        val themes = KeyboardTheme.entries
        ui.spinners(SpinnerItem("主題樣式", ImePrefs.THEME, themes.map { it.label }, themes.map { it.name }, s.theme.name))
        ui.check("顯示按鍵", null, ImePrefs.SHOW_KEYS, s.showKeys)
        ui.show()
    }
}
