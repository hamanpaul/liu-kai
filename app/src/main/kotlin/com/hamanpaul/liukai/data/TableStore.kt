package com.hamanpaul.liukai.data

import android.content.Context
import com.hamanpaul.liukai.core.engine.Language
import com.hamanpaul.liukai.core.engine.languageTables
import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable
import com.hamanpaul.liukai.core.table.ImportResult
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableBundle
import com.hamanpaul.liukai.core.table.TableImporter
import com.hamanpaul.liukai.core.table.UserPhrases
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
    /** 繁中以外的語言模式字表（无／台／日）。 */
    val others: Map<Language, CompiledTable>,
    val readings: Readings,
    /** 目前字表來自 APK 內建（尚未匯入其他字表）。 */
    val bundled: Boolean,
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
 *
 * 內建字表：本機建置時若有使用者自建字表，會編譯成 `assets/bundled.liutable` 內建進 APK。
 * 第一次載入且還沒有字表時複製為目前字表（裝好就能打中文）；清除字表後下次載入改回內建字表。
 * 建置時沒有字表（例如 CI）就沒有內建字表，複製失敗即略過，需在設定頁匯入。
 */
object TableStore {
    private const val FILE_NAME = "table.liutb"
    private const val BUNDLED_ASSET = "bundled.liutable"
    /** 已套用過內建字表的標記：有這個標記就不再自動套用（清除字表時一併刪除）。 */
    private const val SEEDED_MARKER = "table.seeded"
    /** 目前字表來自內建的標記（匯入其他字表時刪除）。 */
    private const val BUNDLED_FLAG = "table.bundled"

    @Volatile private var cached: LoadedTables? = null
    @Volatile private var cachedStamp: Long = -1
    private val readings: Readings by lazy { Readings.loadBundled() }

    private fun file(context: Context) = File(context.filesDir, FILE_NAME)

    /** 依匯入的來源檔建立字表並以原子搬移存檔，回傳匯入報告。失敗時丟出 TableImportException。 */
    fun import(context: Context, files: List<NamedBytes>): ImportResult {
        val result = TableImporter.import(files)
        writeAtomically(file(context), ByteArrayOutputStream().also { result.bundle.write(it) }.toByteArray())
        File(context.filesDir, BUNDLED_FLAG).delete()
        invalidate()
        return result
    }

    /** 清除匯入的字表；下次載入時改回內建字表。 */
    fun clear(context: Context) {
        file(context).delete()
        File(context.filesDir, SEEDED_MARKER).delete()
        File(context.filesDir, BUNDLED_FLAG).delete()
        invalidate()
    }

    private fun writeAtomically(target: File, bytes: ByteArray) {
        val tmp = File(target.parentFile, "$FILE_NAME.tmp")
        tmp.writeBytes(bytes)
        Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
    }

    /** 還沒有字表、也還沒套用過內建字表時，把 APK 內建的字表複製為目前字表。 */
    private fun seedBundled(context: Context, target: File) {
        val marker = File(context.filesDir, SEEDED_MARKER)
        if (target.exists() || marker.exists()) return
        marker.writeText("")
        runCatching {
            writeAtomically(target, context.assets.open(BUNDLED_ASSET).readAllAndClose())
            File(context.filesDir, BUNDLED_FLAG).writeText("")
        }
    }

    /** 沒有字表時回傳 null；字表檔損毀或缺少繁中區段時丟出例外。 */
    @Synchronized
    fun load(context: Context): LoadedTables? {
        val f = file(context)
        seedBundled(context, f)
        if (!f.exists()) {
            invalidate()
            return null
        }
        // 字表檔或自訂字詞（加字加詞）有變更時重新編譯
        val stamp = (f.lastModified() xor f.length()) * 31 + UserPhraseStore.stamp(context)
        val hit = cached
        if (hit != null && stamp == cachedStamp) return hit
        val bundle = TableBundle.read(ByteArrayInputStream(f.readBytes()))
        val loaded = LoadedTables(
            bundle = bundle,
            // 自訂字詞排在同碼候選前面
            traditional = CompiledTable.build(
                UserPhrases.merge(bundle.section(SectionKind.TRADITIONAL)!!.entries, UserPhraseStore.load(context)),
            ),
            others = bundle.languageTables(),
            readings = readings,
            bundled = File(context.filesDir, BUNDLED_FLAG).exists(),
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
