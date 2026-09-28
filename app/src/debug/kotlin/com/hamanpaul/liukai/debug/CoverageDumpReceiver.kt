package com.hamanpaul.liukai.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import java.io.File

/**
 * 僅 debug 版：把 JaCoCo（offline instrumentation）目前的執行資料寫到 `files/coverage.ec`。
 * 由 TestPilot plugin 在案例全部跑完後以 broadcast 觸發兩次（第二次才包含本方法自身的執行紀錄），
 * 再以 `run-as` 取回，與 JVM 單元測試資料合併產生 app 覆蓋率報表。
 */
class CoverageDumpReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val agent = Class.forName("org.jacoco.agent.rt.RT").getMethod("getAgent").invoke(null)!!
        val data = agent.javaClass.getMethod("getExecutionData", Boolean::class.javaPrimitiveType).invoke(agent, false) as ByteArray
        File(context.filesDir, "coverage.ec").writeBytes(data)
        resultCode = 1
        resultData = "OK ${data.size}"
    }
}
