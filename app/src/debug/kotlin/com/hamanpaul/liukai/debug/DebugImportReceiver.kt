package com.hamanpaul.liukai.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.data.TableStore
import com.hamanpaul.liukai.data.readAllAndClose
import java.io.File

/**
 * 僅 debug 版：供 adb 自動化匯入字表。
 * - `--es source demo`：匯入 APK 內建的合成字表（assets/synthetic-*.txt）。
 * - `--es source files`：匯入 app 私有目錄 `files/import/` 內的所有檔案
 *   （以 `adb exec-in run-as <pkg> sh -c 'cat > files/import/<name>'` 寫入；匯入後刪除）。
 * 結果寫入 logcat（tag LiuKaiDebugImport）與 result data。
 */
class DebugImportReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        Thread {
            val message = runCatching {
                val files = when (val source = intent.getStringExtra("source")!!) {
                    "demo" -> listOf("synthetic-ibus.txt", "synthetic-lime.txt").map { name ->
                        NamedBytes(name, context.assets.open(name).readAllAndClose())
                    }
                    "files" -> {
                        val dir = File(context.filesDir, "import")
                        val list = dir.listFiles().orEmpty().sortedBy { it.name }
                        require(list.isNotEmpty()) { "${dir.path} 沒有檔案" }
                        list.map { NamedBytes(it.name, it.readBytes()) }.also { dir.deleteRecursively() }
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
