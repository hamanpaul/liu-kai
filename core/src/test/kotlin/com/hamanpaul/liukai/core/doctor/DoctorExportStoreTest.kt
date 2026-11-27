package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import java.util.zip.ZipFile
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.fail
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorExportStoreTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `mark 分離 action 與 selected time 預設及自訂 window 都由同一 ZIP 預覽儲存`() {
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root)
        val store = context.open()
        store.enable().success()
        val selected = DoctorStoreTestContext.START_WALL_MS
        val events = listOf(
            context.touchEvent(store, 1, wallUtcMs = selected - 300_001),
            context.touchEvent(store, 2, wallUtcMs = selected - 300_000),
            context.touchEvent(store, 3, wallUtcMs = selected),
            context.touchEvent(store, 4, wallUtcMs = selected + 30_000),
            context.touchEvent(store, 5, wallUtcMs = selected + 30_001),
        )
        events.forEach { store.append(it).success() }
        Files.writeString(root.resolve("unrelated.txt"), "not doctor data")
        Files.writeString(
            root.resolve("records").resolve("foreign.jsonl"),
            encodeDoctorLine(context.touchEvent(store, 99, wallUtcMs = selected)) + "\n",
        )
        context.clock.setWallUtcMs(selected + 60_000)

        val mark = store.markIssue(selected).success()
        assertEquals(selected + 60_000, mark.actionTimeMs)
        assertEquals(selected, mark.selectedIssueTimeMs)
        assertEquals(300_000, mark.window.beforeMs)
        assertEquals(30_000, mark.window.afterMs)
        val snapshot = store.prepareExport(mark).success()
        val preview = store.previewExport(snapshot).success()
        assertEquals(store.status().epoch, preview.epoch)
        assertEquals(3, preview.recordCount)
        assertEquals(
            setOf(
                "doctor/manifest-v1.json",
                "doctor/mark-v1.json",
                "doctor/events-v1.jsonl",
            ),
            zipEntryNames(snapshot.archive),
        )
        val exported = DoctorEventCodec.decodeJsonl(zipText(snapshot.archive, "doctor/events-v1.jsonl"))
        assertEquals(null, exported.failure)
        assertEquals(
            listOf(selected - 300_000, selected, selected + 30_000),
            exported.events.map { it.event.wallUtcMs },
        )
        val exportedMark = zipText(snapshot.archive, "doctor/mark-v1.json")
        assertTrue(exportedMark.contains("\"actionTimeMs\":${mark.actionTimeMs}"))
        assertTrue(exportedMark.contains("\"selectedIssueTimeMs\":$selected"))
        assertEquals(5, store.readRecords().success().size)
        assertFalse(zipEntryNames(snapshot.archive).any { "foreign" in it || "unrelated" in it })

        val fixedZip = Files.readAllBytes(snapshot.archive)
        val destination = root.resolve("saved-doctor.zip")
        store.saveExport(snapshot, destination).success()
        assertTrue(Files.exists(destination))
        assertTrue(fixedZip.contentEquals(Files.readAllBytes(destination)))
        assertFalse(Files.exists(snapshot.archive))

        val customMark = store.markIssue(
            selectedIssueTimeMs = selected,
            window = DoctorExportWindow(beforeMs = 1_000, afterMs = 1_000),
        ).success()
        val customSnapshot = store.prepareExport(customMark).success()
        assertEquals(1, store.previewExport(customSnapshot).success().recordCount)
        store.cancelExport(customSnapshot).success()
        assertFalse(Files.exists(customSnapshot.archive))
    }

    @Test
    fun `同時只保留一份固定 snapshot 並在取消或到期後刪暫存`() {
        val limits = DoctorStoreLimits.DEFAULT.copy(exportTtlMs = 1_000)
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root, limits = limits)
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val mark = store.markIssue(context.clock.wallUtcMs()).success()
        val first = store.prepareExport(mark).success()
        val firstBytes = Files.readAllBytes(first.archive)

        assertEquals(
            DoctorStoreFailure.EXPORT_EXISTS,
            store.prepareExport(mark).failure(),
        )
        store.previewExport(first).success()
        assertTrue(firstBytes.contentEquals(Files.readAllBytes(first.archive)))
        store.cancelExport(first).success()
        assertFalse(Files.exists(first.archive))
        assertEquals(0, store.status().tempBytes)

        val second = store.prepareExport(mark).success()
        assertTrue(first.archive != second.archive)
        context.clock.advance(1_001)
        assertEquals(
            DoctorStoreFailure.EXPORT_EXPIRED,
            store.previewExport(second).failure(),
        )
        assertFalse(Files.exists(second.archive))
        assertEquals(0, store.status().tempBytes)
        assertTrue(context.tempExportFiles().isEmpty())
    }

    @Test
    fun `save 失敗不假完成而 ready snapshot 在同 epoch stop 後仍可儲存`() {
        val root = temporaryFolder.newFolder().toPath()
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(root, faults = faults)
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
        val epoch = snapshot.epoch
        val destination = root.resolve("picker-target.zip")
        faults.failNext(DoctorIoOperation.SAVE_EXPORT, DoctorStoreFailure.PERMISSION_DENIED)

        store.disable().success()
        val stopped = context.open()
        assertEquals(
            DoctorStoreFailure.PERMISSION_DENIED,
            stopped.saveExport(snapshot, destination).failure(),
        )
        assertTrue(Files.exists(snapshot.archive))
        assertFalse(Files.exists(destination))

        stopped.saveExport(snapshot, destination).success()
        assertEquals(epoch, stopped.status().epoch)
        assertFalse(Files.exists(snapshot.archive))
        assertTrue(Files.exists(destination))
        assertEquals(0, stopped.status().tempBytes)
        assertEquals(1, stopped.readRecords().success().size)
    }

    @Test
    fun `預設容量值為 32 MiB 7 天 32 MiB ZIP 64 MiB 總量並實際守小型 limits`() {
        assertEquals(32L * 1024 * 1024, DoctorStoreLimits.DEFAULT.maxRawBytes)
        assertEquals(7L * 24 * 60 * 60 * 1000, DoctorStoreLimits.DEFAULT.maxAgeMs)
        assertEquals(32L * 1024 * 1024, DoctorStoreLimits.DEFAULT.maxExportBytes)
        assertEquals(64L * 1024 * 1024, DoctorStoreLimits.DEFAULT.maxRawAndTempBytes)

        val exportLimitRoot = temporaryFolder.newFolder().toPath()
        val exportLimited = DoctorStoreTestContext(
            exportLimitRoot,
            limits = DoctorStoreLimits.DEFAULT.copy(maxExportBytes = 128),
        )
        val exportStore = exportLimited.open()
        exportStore.enable().success()
        (1L..3L).forEach { exportStore.append(exportLimited.touchEvent(exportStore, it)).success() }
        val exportMark = exportStore.markIssue(exportLimited.clock.wallUtcMs()).success()

        assertEquals(
            DoctorStoreFailure.EXPORT_LIMIT,
            exportStore.prepareExport(exportMark).failure(),
        )
        assertEquals(0, exportStore.status().tempBytes)
        assertTrue(exportLimited.tempExportFiles().isEmpty())

        val totalLimitRoot = temporaryFolder.newFolder().toPath()
        val unrestricted = DoctorStoreTestContext(totalLimitRoot)
        val unrestrictedStore = unrestricted.open()
        unrestrictedStore.enable().success()
        (1L..2L).forEach {
            unrestrictedStore.append(unrestricted.touchEvent(unrestrictedStore, it)).success()
        }
        val rawBytes = unrestrictedStore.status().rawBytes
        val constrained = DoctorStoreTestContext(
            totalLimitRoot,
            clock = unrestricted.clock,
            limits = DoctorStoreLimits.DEFAULT.copy(maxRawAndTempBytes = rawBytes + 1),
        ).open()
        val totalMark = constrained.markIssue(unrestricted.clock.wallUtcMs()).success()

        assertEquals(
            DoctorStoreFailure.TOTAL_LIMIT,
            constrained.prepareExport(totalMark).failure(),
        )
        assertEquals(0, constrained.status().tempBytes)
        assertTrue(unrestricted.tempExportFiles().isEmpty())
    }

    @Test
    fun `export 首次讀取會排除清理過期 raw 而不是把舊事件放入 ZIP`() {
        val limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 1_000)
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root, limits = limits)
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        context.clock.advance(1_001)
        val mark = store.markIssue(DoctorStoreTestContext.START_WALL_MS).success()

        val snapshot = store.prepareExport(mark).success()
        assertEquals(0, store.previewExport(snapshot).success().recordCount)
        assertEquals(0, store.status().rawBytes)
        assertTrue(context.doctorRecordFiles().isEmpty())
        assertTrue(zipText(snapshot.archive, "doctor/events-v1.jsonl").isEmpty())
        store.cancelExport(snapshot).success()
        assertFalse(Files.exists(snapshot.archive))
    }

    @Test
    fun `ZIP capacity counts actual bytes once and accepts the exact limit`() {
        val baseline = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val baselineStore = baseline.open()
        val snapshot = baselineStore.prepareExport(
            baselineStore.markIssue(baseline.clock.wallUtcMs()).success(),
        ).success()
        val exactBytes = Files.size(snapshot.archive)
        for (delta in listOf(-1L, 0L, 1L)) {
            val context = DoctorStoreTestContext(
                temporaryFolder.newFolder().toPath(),
                limits = DoctorStoreLimits.DEFAULT.copy(maxExportBytes = exactBytes + delta),
            )
            val store = context.open()
            val result = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success())
            if (delta < 0) {
                assertEquals(DoctorStoreFailure.EXPORT_LIMIT, result.failure())
                assertEquals(0L, store.status().tempBytes)
            } else {
                val prepared = result.success()
                assertEquals(exactBytes, Files.size(prepared.archive))
                assertEquals(exactBytes, store.status().tempBytes)
                assertEquals(0, store.previewExport(prepared).success().recordCount)
            }
        }
    }

    private fun zipEntryNames(path: java.nio.file.Path): Set<String> =
        ZipFile(path.toFile()).use { zip ->
            zip.entries().asSequence().map { it.name }.toSet()
        }

    private fun zipText(path: java.nio.file.Path, entryName: String): String =
        ZipFile(path.toFile()).use { zip ->
            val entry = zip.getEntry(entryName) ?: fail("missing ZIP entry $entryName")
            zip.getInputStream(entry).use { input ->
                input.readBytes().toString(Charsets.UTF_8)
            }
        }

    private fun encodeDoctorLine(event: DoctorEventRecord): String =
        when (val encoded = DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, event)) {
            is DoctorEventEncode.Accepted -> encoded.line
            is DoctorEventEncode.Rejected -> fail("test event rejected: ${encoded.reason}")
        }
}

internal fun DoctorStoreTestContext.tempExportFiles(): List<java.nio.file.Path> {
    val temp = root.resolve("temp")
    if (!Files.exists(temp)) return emptyList()
    return Files.list(temp).use { paths ->
        paths.filter { it.fileName.toString().matches(Regex("doctor-export-.*\\.zip")) }
            .toList()
            .sortedBy { it.fileName.toString() }
    }
}
