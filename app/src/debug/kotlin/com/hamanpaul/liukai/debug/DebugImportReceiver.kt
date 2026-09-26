package com.hamanpaul.liukai.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.data.TableStore
import java.io.File

/**
 * 僅 debug 版：供 adb 自動化匯入字表。
 * - `--es source demo`：匯入 APK 內建的合成字表（assets/synthetic-*.txt）。
 * - `--es source files`：匯入 app 專屬外部目錄 `files/import/` 內的所有檔案（以 adb push 放入）。
 * 結果寫入 logcat（tag LiuKaiDebugImport）與 result data。
 */
class DebugImportReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        Thread {
            val source = intent.getStringExtra("source") ?: "demo"
            val message = runCatching {
                val files = when (source) {
                    "demo" -> listOf("synthetic-ibus.txt", "synthetic-lime.txt").map { name ->
                        NamedBytes(name, context.assets.open(name).use { it.readBytes() })
                    }
                    "files" -> {
                        val dir = File(context.getExternalFilesDir(null), "import")
                        val list = dir.listFiles()?.filter { it.isFile }?.sortedBy { it.name }.orEmpty()
                        require(list.isNotEmpty()) { "${dir.path} 沒有檔案" }
                        list.map { NamedBytes(it.name, it.readBytes()) }
                    }
                    else -> error("未知的 source：$source")
                }
                val result = TableStore.import(context, files)
                "OK " + result.bundle.stats.joinToString("; ") {
                    "${it.kind} raw=${it.rawRows} pairs=${it.uniquePairs} codes=${it.uniqueCodes} texts=${it.uniqueTexts}"
                }
            }.getOrElse { "FAIL ${it.message}" }
            Log.i(TAG, message)
            pending.resultData = message
            pending.resultCode = if (message.startsWith("OK")) 1 else 0
            pending.finish()
        }.start()
    }

    companion object {
        private const val TAG = "LiuKaiDebugImport"
    }
}
