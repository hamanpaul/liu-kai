package com.hamanpaul.liukai.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.hamanpaul.liukai.data.ImePrefs

/**
 * 僅 debug 版：供 adb 自動化設定偏好（與設定頁寫入的是同一份 SharedPreferences）。
 * - `--es key <鍵> --es value <值>`：設定一項。
 * - `--es reset true`：清除所有偏好，回到預設值。
 * 輸入法在下一次顯示鍵盤時讀取設定。
 */
class DebugPrefReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.getStringExtra("reset") == "true") {
            ImePrefs.reset(context)
            resultData = "OK reset"
        } else {
            val key = intent.getStringExtra("key")!!
            val value = intent.getStringExtra("value")!!
            ImePrefs.set(context, key, value)
            resultData = "OK $key=$value"
        }
        resultCode = 1
    }
}
