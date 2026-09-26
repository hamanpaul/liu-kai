package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableBundle
import com.hamanpaul.liukai.core.table.TableImporter
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** 已載入並編譯好的字表，給 IME 使用。 */
class LoadedTables(
    val bundle: TableBundle,
    val traditional: CompiledTable,
    val readings: Readings,
)

/** 讀完整個串流後關閉。 */
fun InputStream.readAllAndClose(): ByteArray {
    val bytes = readBytes()
    close()
    return bytes
}

/**
 * 字表存放於 app 私有目錄 `files/table.liutb`（不備份、不外流）。
 * 以檔案修改時間與大小判斷是否需要重新載入，讓設定頁匯入後 IME 下次啟動輸入即生效。
 */
object TableStore {
    private const val FILE_NAME = "table.liutb"

    @Volatile private var cached: LoadedTables? = null
    @Volatile private var cachedStamp: Long = -1
    private val readings: Readings by lazy { Readings.loadBundled() }

    private fun file(context: Context) = File(context.filesDir, FILE_NAME)

    /** 依匯入的來源檔建立字表並以原子搬移存檔，回傳匯入報告。失敗時丟出 TableImportException。 */
    fun import(context: Context, files: List<NamedBytes>): ImportResult {
        val result = TableImporter.import(files)
        val target = file(context)
        val tmp = File(target.parentFile, "$FILE_NAME.tmp")
        tmp.writeBytes(ByteArrayOutputStream().also { result.bundle.write(it) }.toByteArray())
        Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        invalidate()
        return result
    }

    fun clear(context: Context) {
        file(context).delete()
        invalidate()
    }

    /** 沒有字表時回傳 null；字表檔損毀或缺少繁中區段時丟出例外。 */
    @Synchronized
    fun load(context: Context): LoadedTables? {
        val f = file(context)
        if (!f.exists()) {
            invalidate()
            return null
        }
        val stamp = f.lastModified() xor f.length()
        val hit = cached
        if (hit != null && stamp == cachedStamp) return hit
        val bundle = TableBundle.read(ByteArrayInputStream(f.readBytes()))
        val loaded = LoadedTables(
            bundle = bundle,
            traditional = CompiledTable.build(bundle.section(SectionKind.TRADITIONAL)!!.entries),
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
