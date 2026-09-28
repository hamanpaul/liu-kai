package com.hamanpaul.liukai.core

import com.hamanpaul.liukai.core.engine.LiuEngine
import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable
import com.hamanpaul.liukai.core.table.NamedBytes
import com.hamanpaul.liukai.core.table.SectionKind
import com.hamanpaul.liukai.core.table.TableImporter

/** 合成測試字表（非官方資料）。 */
object Fixtures {
    fun bytes(name: String): ByteArray =
        requireNotNull(Fixtures::class.java.getResourceAsStream("/fixtures/$name")) { "missing fixture $name" }.readBytes()

    fun text(name: String): String = bytes(name).toString(Charsets.UTF_8)

    val importResult by lazy {
        TableImporter.import(
            listOf(
                NamedBytes("synthetic-ibus.txt", bytes("synthetic-ibus.txt")),
                NamedBytes("synthetic-lime.txt", bytes("synthetic-lime.txt")),
            ),
        )
    }

    val traditional: CompiledTable by lazy {
        CompiledTable.build(importResult.bundle.section(SectionKind.TRADITIONAL)!!.entries)
    }

    val japanese: CompiledTable by lazy {
        CompiledTable.build(importResult.bundle.section(SectionKind.JAPANESE)!!.entries)
    }

    val readings: Readings by lazy { Readings.parse(text("readings.tsv").lineSequence()) }

    fun engine(): LiuEngine = LiuEngine(traditional, japanese, readings)
}
