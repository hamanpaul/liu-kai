package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableBundle
import com.hamanpaul.liukai.core.table.TableImporter
import java.io.File

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

    /** 依匯入的來源檔建立字表並存檔，回傳匯入報告。失敗時丟出 TableImportException。 */
    fun import(context: Context, files: List<NamedBytes>): ImportResult {
        val result = TableImporter.import(files)
        val target = file(context)
        val tmp = File(target.parentFile, "$FILE_NAME.tmp")
        tmp.outputStream().use { result.bundle.write(it) }
        check(tmp.renameTo(target)) { "無法寫入字表檔" }
        invalidate()
        return result
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
