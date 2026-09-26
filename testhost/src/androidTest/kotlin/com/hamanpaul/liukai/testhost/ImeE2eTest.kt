package com.hamanpaul.liukai.testhost

import android.content.Intent
import android.view.KeyEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.UiObject2
import androidx.test.uiautomator.Until
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * liu-kai 端對端測試（模擬器、合成字表）。前置條件由 scripts/emulator-e2e.sh 完成：
 * 安裝 app 與 testhost、啟用並切換到 liu-kai、以 DEBUG_IMPORT 匯入 demo 合成表。
 */
@RunWith(AndroidJUnit4::class)
class ImeE2eTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val device = UiDevice.getInstance(instrumentation)

    private fun shell(cmd: String) {
        instrumentation.uiAutomation.executeShellCommand(cmd).close()
    }

    private fun launch() {
        val ctx = instrumentation.context
        val intent = ctx.packageManager.getLaunchIntentForPackage(PKG)!!
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        ctx.startActivity(intent)
        assertNotNull("testhost 未啟動", device.wait(Until.findObject(By.desc("plain")), LAUNCH_TIMEOUT))
        field("plain").click()
        assertNotNull("liu-kai 輸入畫面未出現", device.wait(Until.findObject(By.desc("mode")), TIMEOUT))
    }

    @Before
    fun setUp() {
        device.wakeUp()
        shell("wm dismiss-keyguard")
        shell("settings put secure show_ime_with_hard_keyboard 0")
        launch()
        ensureChinese()
    }

    @After
    fun tearDown() {
        shell("settings put secure show_ime_with_hard_keyboard 0")
        device.pressKeyCode(KeyEvent.KEYCODE_ESCAPE)
    }

    private fun field(desc: String): UiObject2 = device.wait(Until.findObject(By.desc(desc)), TIMEOUT)!!

    private fun modeText(): String = device.wait(Until.findObject(By.desc("mode")), TIMEOUT)!!.text

    private fun ensureChinese() {
        if (modeText() == "英") tapShift()
        if (modeText() == "日") ctrl(KeyEvent.KEYCODE_J)
        device.wait(Until.hasObject(By.desc("mode").text("中")), TIMEOUT)
        assertEquals("中", modeText())
    }

    private fun tapShift() {
        device.pressKeyCode(KeyEvent.KEYCODE_SHIFT_LEFT)
        device.waitForIdle()
    }

    private fun ctrl(keyCode: Int) {
        device.pressKeyCode(keyCode, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_LEFT_ON)
        device.waitForIdle()
    }

    private fun type(s: String) {
        for (c in s) {
            when (c) {
                in 'a'..'z' -> device.pressKeyCode(KeyEvent.KEYCODE_A + (c - 'a'))
                in '0'..'9' -> device.pressKeyCode(KeyEvent.KEYCODE_0 + (c - '0'))
                ',' -> device.pressKeyCode(KeyEvent.KEYCODE_COMMA)
                '?' -> device.pressKeyCode(KeyEvent.KEYCODE_SLASH, KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON)
                '!' -> device.pressKeyCode(KeyEvent.KEYCODE_1, KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON)
                '`' -> device.pressKeyCode(KeyEvent.KEYCODE_GRAVE)
                ' ' -> device.pressKeyCode(KeyEvent.KEYCODE_SPACE)
                else -> error("unsupported $c")
            }
        }
        device.waitForIdle()
    }

    private fun candidate(index: Int, text: String): UiObject2 =
        device.wait(Until.findObject(By.desc("cand:$index:$text")), TIMEOUT)
            ?: error("找不到候選 cand:$index:$text")

    private fun assertFieldText(expected: String, desc: String = "plain") {
        device.wait(Until.hasObject(By.desc(desc).text(expected)), TIMEOUT)
        assertEquals(expected, field(desc).text)
    }

    @Test
    fun 空白上屏首選() {
        type("ba")
        candidate(0, "日")
        type(" ")
        assertFieldText("日")
    }

    @Test
    fun VRSF選第二候選() {
        type("bav")
        assertFieldText("月")
    }

    @Test
    fun VRSF衝突時當字根() {
        type("abv ")
        assertFieldText("地")
    }

    @Test
    fun 點選候選上屏且可連續點選() {
        type("c")
        candidate(1, "火").click()
        assertFieldText("火")
        type("c")
        candidate(2, "土").click()
        assertFieldText("火土")
    }

    @Test
    fun 數字鍵選字() {
        type("c3")
        assertFieldText("土")
    }

    @Test
    fun 翻頁後數字鍵選字() {
        type("a")
        candidate(0, "甲")
        device.pressKeyCode(KeyEvent.KEYCODE_PAGE_DOWN)
        type("1")
        assertFieldText("子")
    }

    @Test
    fun 萬用字元查字() {
        type("a?")
        candidate(0, "天").click()
        assertFieldText("天")
    }

    @Test
    fun 反引號查同音字() {
        type("q`")
        candidate(1, "忠").click()
        assertFieldText("忠")
    }

    @Test
    fun 長按候選查同音字() {
        type("q")
        candidate(0, "中").longClick()
        candidate(2, "鐘").click()
        assertFieldText("鐘")
    }

    @Test
    fun 日文模式片假名() {
        ctrl(KeyEvent.KEYCODE_J)
        device.wait(Until.hasObject(By.desc("mode").text("日")), TIMEOUT)
        type("ka")
        candidate(1, "カ").click()
        assertFieldText("カ")
        ctrl(KeyEvent.KEYCODE_J)
    }

    @Test
    fun 單按Shift切換英文() {
        tapShift()
        device.wait(Until.hasObject(By.desc("mode").text("英")), TIMEOUT)
        type("ab")
        assertFieldText("ab")
        tapShift()
    }

    @Test
    fun 組字中按標點先上屏首選() {
        type("ba!")
        assertFieldText("日!")
    }

    @Test
    fun Enter送出原始字碼_Esc清除組字() {
        type("ab")
        device.pressKeyCode(KeyEvent.KEYCODE_ENTER)
        assertFieldText("ab")
        type("c")
        device.pressKeyCode(KeyEvent.KEYCODE_ESCAPE)
        device.waitForIdle()
        assertFieldText("ab")
        type("b")
        device.pressKeyCode(KeyEvent.KEYCODE_DEL)
        device.waitForIdle()
        assertFieldText("ab")
        device.pressKeyCode(KeyEvent.KEYCODE_DEL)
        assertFieldText("a")
    }

    @Test
    fun 密碼欄英數直出() {
        field("password").click()
        device.waitForIdle()
        type("ba")
        device.waitForIdle()
        assertNull("密碼欄不應出現候選", device.findObject(By.descStartsWith("cand:")))
        assertEquals(2, field("password").text.length)
    }

    @Test
    fun 軟鍵盤打字與點選候選() {
        shell("settings put secure show_ime_with_hard_keyboard 1")
        launch()
        for (k in listOf("b", "a")) {
            device.wait(Until.findObject(By.desc("key:$k")), TIMEOUT)!!.click()
        }
        candidate(1, "月").click()
        assertFieldText("月")
        device.wait(Until.findObject(By.desc("key:space")), TIMEOUT)!!.click()
        assertFieldText("月 ")
    }

    companion object {
        private const val PKG = "com.hamanpaul.liukai.testhost"
        private const val TIMEOUT = 5_000L
        private const val LAUNCH_TIMEOUT = 15_000L
    }
}
