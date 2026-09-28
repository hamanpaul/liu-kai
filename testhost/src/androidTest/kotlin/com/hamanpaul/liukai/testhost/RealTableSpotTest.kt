package com.hamanpaul.liukai.testhost

import android.content.Intent
import android.view.KeyEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.Direction
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.UiObject2
import androidx.test.uiautomator.Until
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.util.regex.Pattern

/**
 * 真實字表（使用者自建的 liu_ibus_final.txt＋lime_liu7.txt）抽測。
 * 只在 `scripts/emulator-e2e.sh --real <dir>` 匯入真實字表後、以 `-e realTable true` 執行。
 */
@RunWith(AndroidJUnit4::class)
class RealTableSpotTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val device = UiDevice.getInstance(instrumentation)

    @Before
    fun setUp() {
        assumeTrue(InstrumentationRegistry.getArguments().getString("realTable") == "true")
        device.wakeUp()
        instrumentation.uiAutomation.executeShellCommand("wm dismiss-keyguard").close()
        instrumentation.uiAutomation.executeShellCommand("settings put secure show_ime_with_hard_keyboard 0").close()
        val ctx = instrumentation.context
        ctx.startActivity(
            ctx.packageManager.getLaunchIntentForPackage(PKG)!!
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK),
        )
        assertNotNull(device.wait(Until.findObject(By.desc("plain")), 15_000))
        field().click()
        assertNotNull(device.wait(Until.findObject(By.desc("mode")), TIMEOUT))
        if (mode() == "英") device.pressKeyCode(KeyEvent.KEYCODE_SHIFT_LEFT)
        if (mode() == "日") ctrlJ()
        device.wait(Until.hasObject(By.desc("mode").text("中")), TIMEOUT)
    }

    private fun field(): UiObject2 = device.wait(Until.findObject(By.desc("plain")), TIMEOUT)!!

    private fun mode(): String = device.wait(Until.findObject(By.desc("mode")), TIMEOUT)!!.text

    private fun ctrlJ() {
        device.pressKeyCode(KeyEvent.KEYCODE_J, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_LEFT_ON)
        device.waitForIdle()
    }

    private fun type(s: String) {
        for (c in s) {
            when (c) {
                in 'a'..'z' -> device.pressKeyCode(KeyEvent.KEYCODE_A + (c - 'a'))
                ',' -> device.pressKeyCode(KeyEvent.KEYCODE_COMMA)
                '.' -> device.pressKeyCode(KeyEvent.KEYCODE_PERIOD)
                '?' -> device.pressKeyCode(KeyEvent.KEYCODE_SLASH, KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON)
                '`' -> device.pressKeyCode(KeyEvent.KEYCODE_GRAVE)
                ' ' -> device.pressKeyCode(KeyEvent.KEYCODE_SPACE)
                else -> error("unsupported $c")
            }
        }
        device.waitForIdle()
    }

    private fun assertText(expected: String) {
        device.wait(Until.hasObject(By.desc("plain").text(expected)), TIMEOUT)
        assertEquals(expected, field().text)
    }

    /** 候選可能在候選列畫面外（萬用字元、同音清單很長），找不到時往右捲動候選列。 */
    private fun candidateEndingWith(text: String): UiObject2 {
        val selector = By.desc(Pattern.compile("cand:\\d+:" + Pattern.quote(text)))
        device.wait(Until.findObject(selector), TIMEOUT)?.let { return it }
        val row = device.wait(Until.findObject(By.desc("candidates")), TIMEOUT) ?: error("找不到候選列")
        return row.scrollUntil(Direction.RIGHT, Until.findObject(selector)) ?: error("候選中找不到 $text")
    }

    @Test
    fun 真實字表_一般上屏與標點() {
        type("ci ")
        type(", ")
        assertText("中，")
    }

    @Test
    fun 真實字表_VRSF選字與衝突() {
        type("aav")
        assertText("丶")
        type("aev ")
        assertText("丶鏙")
    }

    @Test
    fun 真實字表_點選候選() {
        type("aa")
        candidateEndingWith("寸").click()
        assertText("寸")
    }

    @Test
    fun 真實字表_萬用字元() {
        type("c?")
        candidateEndingWith("中").click()
        assertText("中")
    }

    @Test
    fun 真實字表_同音字() {
        type("ci`")
        candidateEndingWith("忠").click()
        assertText("忠")
    }

    @Test
    fun 真實字表_日文假名() {
        ctrlJ()
        device.wait(Until.hasObject(By.desc("mode").text("日")), TIMEOUT)
        type("ka, ka. ")
        assertText("かカ")
        ctrlJ()
    }

    companion object {
        private const val PKG = "com.hamanpaul.liukai.testhost"
        private const val TIMEOUT = 5_000L
    }
}
