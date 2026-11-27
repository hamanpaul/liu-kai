package com.hamanpaul.liukai.core.doctor

import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import java.util.zip.CRC32
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorZipContractTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `STORE payload repack remains readable before metadata is changed`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val store = context.open()
        val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
        val contents = ZipFile(snapshot.archive.toFile()).use { zip ->
            zip.entries().asSequence().associate { entry ->
                entry.name to zip.getInputStream(entry).use { it.readBytes() }
            }
        }
        ZipOutputStream(Files.newOutputStream(snapshot.archive)).use { zip ->
            for ((name, bytes) in contents) {
                zip.putNextEntry(ZipEntry(name).apply {
                    method = ZipEntry.STORED
                    size = bytes.size.toLong()
                    compressedSize = size
                    crc = CRC32().apply { update(bytes) }.value
                })
                zip.write(bytes)
                zip.closeEntry()
            }
        }
        assertEquals(0, store.previewExport(snapshot).success().recordCount)
    }

    @Test
    fun `archive metadata requires each fixed field with its exact type and nonnegative range`() {
        val fields = mapOf(
            "doctor/manifest-v1.json" to listOf("schemaVersion", "version", "sourceCommit", "epoch",
                "createdAtMs", "expiresAtMs", "recordCount", "oldestRetainedMs", "newestRetainedMs",
                "ignoredLines", "sequenceGap"),
            "doctor/mark-v1.json" to listOf("schemaVersion", "epoch", "actionTimeMs", "selectedIssueTimeMs",
                "beforeMs", "afterMs"),
        )
        for ((entry, names) in fields) for (field in names) {
            for (invalid in listOf("\"bad value!\"", "true", "null", "-1", "9223372036854775808", "0.25")) {
                val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
                val store = context.open()
                val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
                val contents = ZipFile(snapshot.archive.toFile()).use { zip ->
                    zip.entries().asSequence().associate { e -> e.name to zip.getInputStream(e).use { it.readBytes() } }.toMutableMap()
                }
                val original = contents.getValue(entry).toString(Charsets.UTF_8)
                val mutated = Regex("\"${Regex.escape(field)}\":[^,}]+").replace(original) { "\"$field\":$invalid" }
                contents[entry] = mutated.toByteArray(Charsets.UTF_8)
                ZipOutputStream(Files.newOutputStream(snapshot.archive)).use { zip ->
                    for ((name, bytes) in contents) {
                        zip.putNextEntry(ZipEntry(name).apply {
                            method = ZipEntry.STORED
                            size = bytes.size.toLong(); compressedSize = size
                            crc = CRC32().apply { update(bytes) }.value
                        })
                        zip.write(bytes); zip.closeEntry()
                    }
                }
                assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure(), "$entry $field $invalid")
            }
        }
    }

    @Test
    fun `空快照的固定欄位與 STORE entries 供兩端同一解析器讀取`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val snapshot = store.prepareExport(
            store.markIssue(DoctorStoreTestContext.START_WALL_MS).success(),
        ).success()
        ZipFile(snapshot.archive.toFile()).use { zip ->
            val entries = zip.entries().asSequence().toList()
            assertEquals(
                setOf("doctor/manifest-v1.json", "doctor/mark-v1.json", "doctor/events-v1.jsonl"),
                entries.map { it.name }.toSet(),
            )
            entries.forEach { assertEquals(ZipEntry.STORED, it.method, it.name) }
            fun text(name: String) = zip.getInputStream(zip.getEntry(name)).use {
                it.readBytes().toString(Charsets.UTF_8)
            }
            assertEquals(doctorFixture("export-manifest.json").trimEnd('\n'), text("doctor/manifest-v1.json"))
            assertEquals(doctorFixture("export-mark.json").trimEnd('\n'), text("doctor/mark-v1.json"))
            assertEquals("", text("doctor/events-v1.jsonl"))
        }
        assertEquals(0, store.previewExport(snapshot).success().recordCount)
    }

    @Test
    fun `時鐘倒退不延長快照期限且重新啟動同樣清理`() {
        for (reopen in listOf(false, true)) {
            val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
            val store = context.open()
            store.enable().success()
            val snapshot = store.prepareExport(
                store.markIssue(DoctorStoreTestContext.START_WALL_MS).success(),
            ).success()
            context.clock.setWallUtcMs(DoctorStoreTestContext.START_WALL_MS - 1)
            if (reopen) {
                context.open()
                assertFalse(Files.exists(snapshot.archive), "startup retained snapshot after clock rollback")
            } else {
                assertEquals(DoctorStoreFailure.EXPORT_EXPIRED, store.previewExport(snapshot).failure())
                assertFalse(Files.exists(snapshot.archive))
            }
        }
    }

    @Test
    fun `共同 ZIP 可讀但額外文字欄位不可被預覽或存檔`() {
        for (name in listOf("export-contract.zip", "export-unknown-field.zip", "export-bad-crc.zip", "export-deflated.zip")) {
            val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
            val store = context.open()
            store.enable().success()
            val snapshot = store.prepareExport(
                store.markIssue(DoctorStoreTestContext.START_WALL_MS).success(),
            ).success()
            val fixture = requireNotNull(javaClass.getResourceAsStream("/doctor-v1/$name"))
                .use { it.readBytes() }
            Files.write(snapshot.archive, fixture)
            if (name == "export-contract.zip") {
                assertEquals(0, store.previewExport(snapshot).success().recordCount)
            } else {
                assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
                val destination = context.root.resolve("rejected.zip")
                assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.saveExport(snapshot, destination).failure())
                assertFalse(Files.exists(destination))
            }
        }
    }
}
