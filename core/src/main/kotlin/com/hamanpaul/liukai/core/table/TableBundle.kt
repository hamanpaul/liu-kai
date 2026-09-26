package com.hamanpaul.liukai.core.table

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest

/** 匯入來源檔的識別資訊（只記檔名與雜湊，不記內容）。 */
data class SourceFile(val name: String, val sha256: String)

/** 匯入後的字表：正規化後各區段 + 來源與統計，存成 app 私有的二進位檔。 */
data class TableBundle(
    val sections: List<TableSection>,
    val sources: List<SourceFile>,
    val stats: List<SectionStats>,
) {
    fun section(kind: SectionKind): TableSection? = sections.firstOrNull { it.kind == kind }

    fun write(out: OutputStream) {
        val d = DataOutputStream(BufferedOutputStream(out))
        d.write(MAGIC)
        d.writeInt(VERSION)
        d.writeInt(sources.size)
        for (s in sources) {
            d.writeUTF(s.name)
            d.writeUTF(s.sha256)
        }
        d.writeInt(sections.size)
        sections.forEachIndexed { i, sec ->
            d.writeUTF(sec.kind.name)
            d.writeUTF(sec.name ?: "")
            val st = stats[i]
            d.writeInt(st.rawRows)
            d.writeInt(sec.entries.size)
            for (e in sec.entries) {
                d.writeUTF(e.code)
                d.writeUTF(e.text)
                d.writeLong(e.freq)
            }
        }
        d.flush()
    }

    companion object {
        private val MAGIC = "LIUKAITB".toByteArray(Charsets.US_ASCII)
        private const val VERSION = 1

        fun read(input: InputStream): TableBundle {
            val d = DataInputStream(BufferedInputStream(input))
            val magic = ByteArray(MAGIC.size)
            d.readFully(magic)
            require(magic.contentEquals(MAGIC)) { "不是 liu-kai 字表檔" }
            val version = d.readInt()
            require(version == VERSION) { "不支援的字表檔版本 $version" }
            val sources = List(d.readInt()) { SourceFile(d.readUTF(), d.readUTF()) }
            val sections = ArrayList<TableSection>()
            val stats = ArrayList<SectionStats>()
            repeat(d.readInt()) {
                val kind = SectionKind.valueOf(d.readUTF())
                val name = d.readUTF().ifEmpty { null }
                val rawRows = d.readInt()
                val entries = List(d.readInt()) { TableEntry(d.readUTF(), d.readUTF(), d.readLong()) }
                val sec = TableSection(kind, name, entries)
                sections += sec
                stats += TableNormalizer.stats(sec, rawRows)
            }
            return TableBundle(sections, sources, stats)
        }
    }
}

fun sha256Hex(bytes: ByteArray): String =
    MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
