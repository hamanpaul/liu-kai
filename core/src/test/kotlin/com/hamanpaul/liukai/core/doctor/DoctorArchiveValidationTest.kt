package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import java.nio.file.Path
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorArchiveValidationTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `coherent but mismatched snapshot metadata is rejected before save`() {
        for ((field, supplied) in listOf("epoch" to "1", "createdAtMs" to "later",
            "expiresAtMs" to "later", "recordCount" to "1",
            "oldestRetainedMs" to "1", "newestRetainedMs" to "1")) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            val snapshot = snapshot(context, store)
            val value = when (field) {
                "createdAtMs" -> (snapshot.createdAtMs + 1).toString()
                "expiresAtMs" -> (snapshot.expiresAtMs + 1).toString()
                else -> supplied
            }
            rewrite(snapshot.archive) { entries ->
                val name = "doctor/manifest-v1.json"
                val text = entries.getValue(name).toString(Charsets.UTF_8)
                val coherent = if (field == "oldestRetainedMs")
                    text.replace("\"newestRetainedMs\":0", "\"newestRetainedMs\":1") else text
                entries[name] = Regex("\"$field\":[0-9]+").replace(coherent, "\"$field\":$value").toByteArray()
            }
            assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure(), field)
            val destination = context.root.resolve("rejected.zip")
            assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.saveExport(snapshot, destination).failure(), field)
            assertFalse(Files.exists(destination))
        }
    }

    @Test fun `missing renamed or additional ZIP entries cannot be previewed`() {
        for (entry in listOf("doctor/manifest-v1.json", "doctor/mark-v1.json", "doctor/events-v1.jsonl")) {
            for (rename in listOf(false, true)) {
                val context = DoctorStoreTestContext(temporary.newFolder().toPath())
                val store = context.open()
                val snapshot = snapshot(context, store)
                rewrite(snapshot.archive) { entries ->
                    val old = entries.remove(entry)!!
                    if (rename) entries["foreign.json"] = old
                }
                assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
            }
        }
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val snapshot = snapshot(context, store)
        rewrite(snapshot.archive) { it["foreign.json"] = byteArrayOf() }
        assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
    }

    @Test fun `invalid events and a valid event from another epoch are rejected`() {
        for (foreign in listOf(false, true)) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            val snapshot = snapshot(context, store)
            val events = if (foreign) {
                val event = context.touchEvent(store, 1).copy(epoch = store.status().epoch + 1)
                (DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, event) as DoctorEventEncode.Accepted).line + "\n"
            } else "{\"text\":\"must never be accepted\"}\n"
            rewrite(snapshot.archive) { it["doctor/events-v1.jsonl"] = events.toByteArray() }
            assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
        }
    }

    @Test fun `corrupt archive and changed snapshot location or identity fail closed`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val snapshot = snapshot(context, store)
        assertEquals(DoctorStoreFailure.EPOCH_MISMATCH,
            store.previewExport(snapshot.copy(epoch = snapshot.epoch + 1)).failure())
        assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT,
            store.previewExport(snapshot.copy(archive = context.root.resolve(snapshot.archive.fileName))).failure())
        assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT,
            store.previewExport(snapshot.copy(archive = snapshot.archive.resolveSibling("foreign.zip"))).failure())
        Files.writeString(snapshot.archive, "not a ZIP")
        assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
        Files.delete(snapshot.archive)
        assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
        Files.createDirectory(snapshot.archive)
        assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
    }

    @Test fun `failed real destination replacement removes only owned staging and preserves preview`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val snapshot = snapshot(context, store)
        val destination = Files.createDirectory(context.root.resolve("destination.zip"))
        val sentinel = destination.resolve("sentinel")
        Files.writeString(sentinel, "preserve")
        assertEquals(DoctorStoreFailure.IO_ERROR, store.saveExport(snapshot, destination).failure())
        assertEquals("preserve", Files.readString(sentinel))
        assertTrue(Files.isRegularFile(snapshot.archive))
        Files.list(context.root).use { paths ->
            assertFalse(paths.anyMatch { it.fileName.toString().startsWith(".doctor-save-") })
        }
        assertEquals(0, store.previewExport(snapshot).success().recordCount)
        Files.delete(snapshot.archive)
        store.cancelExport(snapshot).success()
    }

    @Test fun `startup removes only invalid expired duplicate and nonfile exports`() {
        for (kind in listOf("corrupt", "missing-entry", "renamed-entry", "bad-manifest-crc",
            "expired", "wrong-epoch", "directory", "duplicate")) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            val snapshot = snapshot(context, store)
            when (kind) {
                "corrupt" -> Files.writeString(snapshot.archive, "broken")
                "missing-entry" -> rewrite(snapshot.archive) { it.remove("doctor/manifest-v1.json") }
                "renamed-entry" -> rewrite(snapshot.archive) { it["foreign.json"] = it.remove("doctor/mark-v1.json")!! }
                "bad-manifest-crc" -> {
                    val bytes = Files.readAllBytes(snapshot.archive)
                    val offset = centralHeader(bytes, "doctor/manifest-v1.json")
                    val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
                    buffer.putInt(offset + 16, buffer.getInt(offset + 16) xor 1)
                    Files.write(snapshot.archive, bytes)
                }
                "expired" -> context.clock.setWallUtcMs(snapshot.expiresAtMs)
                "wrong-epoch" -> rewrite(snapshot.archive) { entries ->
                    val name = "doctor/manifest-v1.json"
                    entries[name] = entries.getValue(name).toString(Charsets.UTF_8)
                        .replace("\"epoch\":0", "\"epoch\":1").toByteArray()
                }
                "directory" -> { Files.delete(snapshot.archive); Files.createDirectory(snapshot.archive) }
                "duplicate" -> Files.copy(snapshot.archive,
                    snapshot.archive.resolveSibling("doctor-export-11111111-1111-1111-1111-111111111111.zip"))
            }
            context.open()
            if (kind == "directory") {
                assertTrue(Files.isDirectory(snapshot.archive))
            } else if (kind == "duplicate") {
                Files.list(snapshot.archive.parent).use { assertEquals(1L, it.count()) }
            } else assertFalse(Files.exists(snapshot.archive), kind)
        }
    }

    @Test fun `each fixed entry rejects incorrect CRC mismatched sizes oversized and truncated payloads`() {
        for (name in listOf("doctor/manifest-v1.json", "doctor/mark-v1.json", "doctor/events-v1.jsonl")) {
            for (kind in listOf("crc", "different-size", "oversized", "short-body")) {
                val context = DoctorStoreTestContext(temporary.newFolder().toPath())
                val store = context.open()
                val snapshot = snapshot(context, store)
                val bytes = Files.readAllBytes(snapshot.archive)
                val header = centralHeader(bytes, name)
                val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
                when (kind) {
                    "crc" -> buffer.putInt(header + 16, buffer.getInt(header + 16) xor 1)
                    "different-size" -> buffer.putInt(header + 20, buffer.getInt(header + 20) + 1)
                    else -> {
                        val size = if (kind == "oversized") 32 * 1024 * 1024 + 1 else 65_536
                        buffer.putInt(header + 20, size)
                        buffer.putInt(header + 24, size)
                    }
                }
                Files.write(snapshot.archive, bytes)
                assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure(), "$name $kind")
            }
        }
    }

    @Test fun `record counts outside signed int range and root destinations are rejected`() {
        for (value in listOf("-2147483649", "2147483648")) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            val snapshot = snapshot(context, store)
            rewrite(snapshot.archive) { entries ->
                val name = "doctor/manifest-v1.json"
                entries[name] = entries.getValue(name).toString(Charsets.UTF_8)
                    .replace("\"recordCount\":0", "\"recordCount\":$value").toByteArray()
            }
            assertEquals(DoctorStoreFailure.INVALID_SNAPSHOT, store.previewExport(snapshot).failure())
        }
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val snapshot = snapshot(context, store)
        assertEquals(DoctorStoreFailure.INVALID_REQUEST,
            store.saveExport(snapshot, context.root.root).failure())
    }

    private fun centralHeader(bytes: ByteArray, name: String): Int {
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        for (offset in 0..bytes.size - 46) {
            if (buffer.getInt(offset) != 0x02014b50) continue
            val length = buffer.getShort(offset + 28).toInt() and 0xffff
            if (offset + 46 + length <= bytes.size &&
                bytes.copyOfRange(offset + 46, offset + 46 + length).toString(Charsets.UTF_8) == name) return offset
        }
        error("Missing central directory entry")
    }

    private fun snapshot(context: DoctorStoreTestContext, store: DoctorStore): DoctorExportSnapshot =
        store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()

    private fun rewrite(path: Path, change: (MutableMap<String, ByteArray>) -> Unit) {
        val entries = ZipFile(path.toFile()).use { zip ->
            zip.entries().asSequence().associate { entry ->
                entry.name to zip.getInputStream(entry).use { it.readBytes() }
            }.toMutableMap()
        }
        change(entries)
        ZipOutputStream(Files.newOutputStream(path)).use { zip ->
            for ((name, bytes) in entries) {
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
    }
}
