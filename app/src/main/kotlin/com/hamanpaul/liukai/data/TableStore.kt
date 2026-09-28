package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableBundle
import com.hamanpaul.liukai.core.table.TableImporter
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** 已載入並編譯好的字表，給 IME 使用。 */
class LoadedTables(
    val bundle: TableBundle,
    val traditional: CompiledTable,
    val japanese: CompiledTable?,
    val readings: Readings,
)

/**
 * 字表存放於 app 私有目錄 `files/table.liutb`（不備份、不外流）。
 * 以檔案修改時間判斷是否需要重新載入，讓設定頁匯入後 IME 下次啟動輸入即生效。
 */
object TableStore {
    private const val FILE_NAME = "table.liutb"

    @Volatile private var cached: LoadedTables? = null
    @Volatile private var cachedStamp: Long = -1
    private val readings: Readings by lazy { Readings.loadBundled() }

    private fun file(context: Context) = File(context.filesDir, FILE_NAME)

    fun hasTable(context: Context): Boolean = file(context).exists()

    /** 依匯入的來源檔建立字表並以原子搬移存檔，回傳匯入報告。失敗時丟出 TableImportException。 */
    fun import(context: Context, files: List<NamedBytes>): ImportResult {
        val result = TableImporter.import(files)
        writeAtomically(file(context), ByteArrayOutputStream().also { result.bundle.write(it) }.toByteArray())
        invalidate()
        return result
    }

    /**
     * 先寫到同目錄的暫存檔，再以原子搬移（ATOMIC_MOVE＋REPLACE_EXISTING）取代字表檔：寫入途中當機或被結束時
     * 原字表檔不受影響；搬移失敗時丟出例外（不像 renameTo 只回傳 false、各裝置覆蓋行為不一）。
     */
    private fun writeAtomically(target: File, bytes: ByteArray) {
        val tmp = File(target.parentFile, "$FILE_NAME.tmp")
        tmp.writeBytes(bytes)
        Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
    }

    fun clear(context: Context) {
        file(context).delete()
        invalidate()
    }

    @Synchronized
    fun load(context: Context): LoadedTables? {
        val f = file(context)
        if (!f.exists()) {
            cached = null
            cachedStamp = -1
            return null
        }
        val stamp = f.lastModified() xor f.length()
        cached?.let { if (stamp == cachedStamp) return it }
        val bundle = f.inputStream().use { TableBundle.read(it) }
        val trad = bundle.section(SectionKind.TRADITIONAL) ?: return null
        val loaded = LoadedTables(
            bundle = bundle,
            traditional = CompiledTable.build(trad.entries),
            japanese = bundle.section(SectionKind.JAPANESE)?.let { CompiledTable.build(it.entries) },
            readings = readings,
        )
        cached = loaded
        cachedStamp = stamp
        return loaded
    }

    @Synchronized
    private fun invalidate() {
        cached = null
        cachedStamp = -1
    }
}
