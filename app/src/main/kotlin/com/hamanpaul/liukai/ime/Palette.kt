package com.hamanpaul.liukai.ime

import android.graphics.Color
import com.hamanpaul.liukai.data.KeyboardTheme

/**
 * 鍵盤主題色。數值取自官方 PRO 3.0.9 各主題在「顯示按鍵」開與關時的模擬器截圖（420dpi，英文字母層）。
 * cells＝按鍵獨立分格（顯示按鍵，經典灰一律分格）；不分格時按鍵扁平、空白鍵為膠囊、Enter 為圓形。
 */
data class Palette(
    val cells: Boolean,
    val bg: Int,
    val bar: Int,
    val keyTop: Int,
    val keyBottom: Int,
    val fnTop: Int,
    val fnBottom: Int,
    val pressedTop: Int,
    val pressedBottom: Int,
    val text: Int,
    val hint: Int,
    /** 單字元標籤用粗體（經典灰）；其餘主題單字元為一般字重，多字元標籤（?123、Next…）一律粗體。 */
    val boldLetters: Boolean,
    /** 可長按彈出的鍵右下角「…」。 */
    val popupMark: Int,
    /** Shift、Backspace 等圖示。 */
    val icon: Int,
    /** 候選列的常用標點。 */
    val strip: Int,
    val mic: Int,
    /** 分格時 Enter 鍵的底色。 */
    val enter: Int,
    /** 不分格時 Enter 的圓形底色。 */
    val enterCircle: Int,
    val enterText: Int,
    /** 不分格時的空白鍵膠囊。 */
    val spacePill: Int,
    /** Shift／ALT 指示：經典灰為右上角圓點，其餘主題為圖示下方的橫線。 */
    val indicatorDot: Boolean,
    val indicatorOff: Int,
    val indicatorOn: Int,
    val indicatorLock: Int,
    val popupBg: Int,
    val popupKeyTop: Int,
    val popupKeyBottom: Int,
    /** 彈出面板底部的線（經典灰為橘色，其餘透明）。 */
    val popupLine: Int,
    val popupDim: Int,
    val previewBg: Int,
    /** 預覽在按鍵上方獨立的黑框（經典灰）；其餘主題為蓋住按鍵往上延伸的半透明長條。 */
    val previewAbove: Boolean,
)

object Palettes {
    private fun c(hex: String) = Color.parseColor(hex)

    /** 兩色混合：t＝0 為 a，1 為 b。 */
    private fun mix(a: Int, b: Int, t: Float) = Color.rgb(
        (Color.red(a) + (Color.red(b) - Color.red(a)) * t).toInt(),
        (Color.green(a) + (Color.green(b) - Color.green(a)) * t).toInt(),
        (Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t).toInt(),
    )

    private fun alpha(color: Int, a: Int) = Color.argb(a, Color.red(color), Color.green(color), Color.blue(color))

    /** 經典灰（使用者手機的設定）：黑底、上淺下深漸層按鍵、黃綠指示點、深色彈出面板與橘線、按鍵上方黑框預覽。 */
    private val GRAY = Palette(
        cells = true, boldLetters = true,
        bg = Color.BLACK, bar = Color.BLACK,
        keyTop = c("#8A8A8A"), keyBottom = c("#6F6F6F"), fnTop = c("#4B4C4C"), fnBottom = c("#323232"),
        pressedTop = c("#DBDBDB"), pressedBottom = c("#C1C1C1"),
        text = Color.WHITE, hint = c("#C4C4C4"), popupMark = c("#767676"), icon = Color.WHITE, strip = c("#FCAE00"), mic = c("#A6ADB0"),
        enter = c("#4B4C4C"), enterCircle = c("#4B4C4C"), enterText = Color.WHITE, spacePill = c("#343535"),
        indicatorDot = true, indicatorOff = c("#3A3B3B"), indicatorOn = c("#D0DD27"), indicatorLock = c("#F58A1F"),
        popupBg = c("#141414"), popupKeyTop = c("#686868"), popupKeyBottom = c("#4B4B4B"), popupLine = c("#C37629"),
        popupDim = Color.argb(0x8F, 0, 0, 0), previewBg = c("#0B0B0B"), previewAbove = true,
    )

    /** 其他主題的量測值：bar、bg、按鍵、功能鍵、字、數字提示、圖示、標點、麥克風、Enter 格、Enter 圓、Enter 字、空白膠囊。 */
    private class Measured(
        val bar: String, val bg: String, val key: String, val fn: String, val text: String, val hint: String, val icon: String,
        val strip: String, val mic: String, val enter: String, val enterCircle: String, val enterText: String, val spacePill: String,
    )

    private val MEASURED = mapOf(
        KeyboardTheme.WHITE to Measured("#DDE1E4", "#ECEFF1", "#FDFDFE", "#F4F5F7", "#37474F", "#9AA2A6", "#7F8B8F", "#4A555D", "#58666D", "#F4F5F7", "#4DB6AC", "#37474F", "#D1D5D9"),
        KeyboardTheme.BLACK to Measured("#191D21", "#263238", "#404B4F", "#313C40", "#D9DBDC", "#8C9395", "#8B969A", "#CFCFD0", "#ABB3B7", "#235E73", "#80CBC4", "#D3DFE3", "#3C474C"),
        KeyboardTheme.RED to Measured("#B71C1C", "#C62828", "#D04E4E", "#CA3838", "#F6DBDB", "#E39595", "#CB9294", "#CFCFD0", "#CBB3B6", "#F44336", "#F44336", "#FDD9D6", "#D66363"),
        KeyboardTheme.GREEN to Measured("#0B4E51", "#0B5749", "#176C5C", "#207D6B", "#D0E1DE", "#74A79D", "#80A5A1", "#CFCFD0", "#A8BDC0", "#0A4144", "#207D6B", "#CED9D9", "#4E857B"),
        KeyboardTheme.BLUE to Measured("#0D47A1", "#1565C0", "#3E80CB", "#457BD5", "#D8E6F5", "#8BB3E0", "#84AAD1", "#CFCFD0", "#A9BBD0", "#2196F3", "#2196F3", "#D2EAFD", "#558FD1"),
        KeyboardTheme.PURPLE to Measured("#73003D", "#9B0046", "#A8099F", "#872F97", "#EECDEC", "#CB6BC5", "#BA82A0", "#CFCFD0", "#BDADBC", "#BB1A93", "#BB1A93", "#F1D1E9", "#B64679"),
        KeyboardTheme.PINK to Measured("#F191B1", "#F6BCCF", "#F3CDD9", "#FDDBE7", "#37474F", "#958A94", "#837682", "#4A555D", "#5C5662", "#E2779B", "#E2779B", "#37464F", "#F8CEDC"),
        KeyboardTheme.YELLOW to Measured("#FDD286", "#FFE9A4", "#FED66B", "#F9BC7B", "#37474E", "#9A8E5D", "#878871", "#4A555D", "#5F635A", "#E6A763", "#F9BC7B", "#37474E", "#FFEFBD"),
        KeyboardTheme.COCOA to Measured("#3E2723", "#4E342E", "#6D5853", "#5B433E", "#E2DDDC", "#A79B98", "#9B9796", "#CFCFD0", "#B2B5B7", "#8D6E63", "#8D6E63", "#E8E2E0", "#7F6C67"),
    )

    fun of(theme: KeyboardTheme, showKeys: Boolean): Palette {
        if (theme == KeyboardTheme.GRAY) return GRAY
        val m = MEASURED.getValue(theme)
        val bg = c(m.bg)
        val text = c(m.text)
        val key = if (showKeys) c(m.key) else bg
        val fn = if (showKeys) c(m.fn) else bg
        val pressed = mix(key, text, 0.25f)
        return Palette(
            cells = showKeys, boldLetters = false,
            bg = bg, bar = c(m.bar),
            keyTop = key, keyBottom = key, fnTop = fn, fnBottom = fn, pressedTop = pressed, pressedBottom = pressed,
            text = text, hint = c(m.hint), popupMark = c(m.hint), icon = c(m.icon), strip = c(m.strip), mic = c(m.mic),
            enter = c(m.enter), enterCircle = c(m.enterCircle), enterText = c(m.enterText), spacePill = c(m.spacePill),
            indicatorDot = false, indicatorOff = c(m.hint), indicatorOn = c(m.enterCircle), indicatorLock = c(m.enterCircle),
            popupBg = bg, popupKeyTop = key, popupKeyBottom = key, popupLine = Color.TRANSPARENT,
            popupDim = Color.argb(0x66, 0, 0, 0), previewBg = alpha(text, 0x40), previewAbove = false,
        )
    }
}
