package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermission
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorStorageEdgeRegressionTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `export retains callback order and real bounds when wall clock moves backwards`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val base = context.clock.wallUtcMs()
        val events = listOf(2_000L, 1_000L, 3_000L).mapIndexed { index, delta ->
            context.touchEvent(store, index + 1L, base + delta)
        }
        events.forEach { store.append(it).success() }
        val snapshot = store.prepareExport(store.markIssue(0,
            DoctorExportWindow(beforeMs = 1, afterMs = Long.MAX_VALUE)).success()).success()
        val preview = store.previewExport(snapshot).success()
        assertEquals(3, preview.recordCount)
        assertEquals(0, preview.windowStartMs)
        assertEquals(Long.MAX_VALUE, preview.windowEndMs)
        assertEquals(base + 1_000, preview.oldestRetainedMs)
        assertEquals(base + 3_000, preview.newestRetainedMs)
        assertEquals(events, store.readRecords().success())
        store.saveExport(snapshot, context.root.resolve("saved.zip")).success()
    }

    @Test fun `full raw and temporary budget rejects export without creating an archive`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath(),
            limits = DoctorStoreLimits.DEFAULT.copy(maxRawAndTempBytes = 1))
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        assertEquals(DoctorStoreFailure.TOTAL_LIMIT,
            store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).failure())
        assertEquals(0, store.status().tempBytes)
        assertEquals(1, store.readRecords().success().size)
    }

    @Test fun `clock becoming invalid after a valid mark rejects archive creation`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val mark = store.markIssue(context.clock.wallUtcMs()).success()
        context.clock.setWallUtcMs(-1)
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.prepareExport(mark).failure())
        assertEquals(0, store.status().tempBytes)
    }

    @Test fun `raw capacity rotates a smaller active segment before evicting its older events`() {
        val root = temporary.newFolder().toPath()
        val seed = DoctorStoreTestContext(root)
        val event = seed.touchEvent(seed.open(), 1)
        val bytes = (line(event)).toByteArray().size.toLong()
        val context = DoctorStoreTestContext(root,
            limits = DoctorStoreLimits.DEFAULT.copy(maxRawBytes = bytes * 2 + bytes / 2))
        val store = context.open()
        store.enable().success()
        for (sequence in 1L..3L) store.append(context.touchEvent(store, sequence)).success()
        assertEquals(listOf(3L), store.readRecords().success().map { it.sequence })
        assertTrue(store.status().rawBytes <= context.limits.maxRawBytes)
        assertEquals(2, store.status().sequenceGap)
    }

    @Test fun `multiple damaged partially expired segments retain each valid current suffix`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath(),
            limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 1_000))
        val store = context.open()
        store.enable().success()
        val base = context.clock.wallUtcMs()
        val records = Files.createDirectories(context.root.resolve("records"))
        val fresh = (1L..2L).map { n -> context.touchEvent(store, n + 2, base + 1_000) }
        for (n in 1L..2L) Files.writeString(records.resolve("doctor-record-0000000$n.jsonl"),
            line(context.touchEvent(store, n, base)) + line(fresh[(n - 1).toInt()]) + "{bad}\n")
        context.clock.setWallUtcMs(base + 1_500)
        val reopened = context.open()
        assertEquals(fresh, reopened.readRecords().success())
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA, reopened.status().failure)
        assertEquals(2, reopened.status().ignoredLines)
        assertEquals(2, reopened.status().sequenceGap)
    }

    @Test fun `foreign epoch segments are removed at startup and filtered if they appear later`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val current = context.touchEvent(store, 1)
        store.append(current).success()
        val foreign = current.copy(epoch = current.epoch + 1)
        val records = context.root.resolve("records")
        val stale = records.resolve("doctor-record-00000001.jsonl")
        Files.writeString(stale, line(foreign))
        val reopened = context.open()
        assertFalse(Files.exists(stale))
        assertEquals(listOf(current), reopened.readRecords().success())
        Files.writeString(stale, line(foreign))
        assertEquals(listOf(current), reopened.readRecords().success())
    }

    @Test fun `a segment containing only an interrupted first line recovers before the next append`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val records = Files.createDirectories(context.root.resolve("records"))
        val path = records.resolve("doctor-record-00000000.jsonl")
        Files.writeString(path, "{")
        val reopened = context.open()
        assertEquals(0, Files.size(path))
        val event = context.touchEvent(reopened, 1)
        reopened.append(event).success()
        assertEquals(listOf(event), reopened.readRecords().success())
    }

    @Test fun `empty control values fail closed rather than interpreting disabled consent`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val control = context.root.resolve("doctor-control-v1")
        Files.writeString(control, Files.readString(control).replace("enabled=1", "enabled="))
        val reopened = context.open()
        assertFalse(reopened.status().collecting)
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA, reopened.status().failure)
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA, reopened.readRecords().failure())
    }

    @Test fun `epoch and incomplete-clear operation faults interrupt collection without changing consent`() {
        for (reason in listOf(DoctorStoreFailure.EPOCH_COMMIT_FAILED, DoctorStoreFailure.CLEAR_INCOMPLETE)) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            store.enable().success()
            context.faults.failNext(DoctorIoOperation.PERSIST_CONTROL, reason)
            assertEquals(reason, store.disable().failure())
            assertTrue(store.status().enabled)
            assertFalse(store.status().collecting)
            if (reason == DoctorStoreFailure.EPOCH_COMMIT_FAILED) {
                assertEquals(reason, store.readRecords().failure())
                assertEquals(reason, store.append(context.touchEvent(store, 1)).failure())
            }
        }
    }

    @Test fun `staging replaced by a symlink during cleanup does not delete its target`() {
        val root = temporary.newFolder().toPath()
        val staging = root.resolve(".doctor-control-v1.tmp")
        val target = root.resolve("foreign.keep")
        Files.writeString(target, "preserve")
        Files.writeString(staging, "interrupted")
        var replaced = false
        val faults = object : ScriptedDoctorIoFaults() {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
                if (!replaced && operation == DoctorIoOperation.DELETE_OLD_RECORD) {
                    replaced = true
                    Files.delete(staging)
                    Files.createSymbolicLink(staging, target)
                }
                return null
            }
        }
        DoctorStoreTestContext(root, faults = faults).open()
        assertTrue(replaced)
        assertTrue(Files.isSymbolicLink(staging))
        assertEquals("preserve", Files.readString(target))
    }

    @Test fun `multiple concurrently damaged record files each contribute one ignored line`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val records = Files.createDirectories(context.root.resolve("records"))
        for (n in 1..2) Files.writeString(records.resolve("doctor-record-0000000$n.jsonl"), "{bad}\n")
        assertTrue(store.readRecords().success().isEmpty())
        assertEquals(2, store.status().ignoredLines)
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA, store.status().failure)
    }

    @Test fun `three retained segments advance ordinals without overwriting earlier files`() {
        val root = temporary.newFolder().toPath()
        val seed = DoctorStoreTestContext(root)
        val bytes = line(seed.touchEvent(seed.open(), 1)).toByteArray().size.toLong()
        val context = DoctorStoreTestContext(root,
            limits = DoctorStoreLimits.DEFAULT.copy(maxRecordBytes = bytes + 8))
        val store = context.open()
        store.enable().success()
        for (n in 1L..3L) store.append(context.touchEvent(store, n)).success()
        assertEquals(3, context.doctorRecordFiles().size)
        assertEquals(listOf(1L, 2L, 3L), context.open().readRecords().success().map { it.sequence })
        Files.writeString(root.resolve("records/doctor-record-999999999999999999999999.jsonl"), "")
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA, context.open().status().failure)
    }

    @Test fun `real directory write or synchronization denial cleans only created staging`() {
        for (permissions in listOf(setOf(PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_EXECUTE),
            setOf(PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE))) {
            val root = temporary.newFolder().toPath()
            val context = DoctorStoreTestContext(root)
            val store = context.open()
            val original = Files.getPosixFilePermissions(root)
            try {
                Files.setPosixFilePermissions(root, permissions)
                assertEquals(DoctorStoreFailure.PERMISSION_DENIED, store.enable().failure())
            } finally {
                Files.setPosixFilePermissions(root, original)
            }
            Files.list(root).use { files ->
                assertFalse(files.anyMatch { it.fileName.toString().startsWith(".doctor-control-") })
            }
        }
    }

    @Test fun `a records directory substituted during retention cannot redirect the next append`() {
        val root = temporary.newFolder().toPath()
        val foreign = Files.createDirectory(root.resolve("foreign"))
        val sentinel = foreign.resolve("sentinel")
        Files.writeString(sentinel, "preserve")
        var armed = false
        val faults = object : ScriptedDoctorIoFaults() {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
                if (armed && operation == DoctorIoOperation.PERSIST_CONTROL) {
                    armed = false
                    val records = root.resolve("records")
                    Files.move(records, root.resolve("preserved-records"))
                    Files.createSymbolicLink(records, foreign)
                }
                return null
            }
        }
        val context = DoctorStoreTestContext(root, faults = faults,
            limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 0))
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        context.clock.advance(1)
        armed = true
        assertEquals(DoctorStoreFailure.IO_ERROR, store.append(context.touchEvent(store, 2)).failure())
        assertFalse(armed)
        assertEquals("preserve", Files.readString(sentinel))
        Files.list(foreign).use { assertEquals(1L, it.count()) }
    }

    @Test fun `an export substituted after enumeration is not deleted as an invalid owned ZIP`() {
        val root = temporary.newFolder().toPath()
        var snapshot: DoctorExportSnapshot? = null
        var armed = false
        val clock = object : DoctorClock {
            override fun wallUtcMs(): Long {
                if (armed) {
                    armed = false
                    val path = snapshot!!.archive
                    Files.move(path, root.resolve("preserved.zip"))
                    Files.createDirectory(path)
                }
                return DoctorStoreTestContext.START_WALL_MS
            }
            override fun monotonicNs() = 1L
        }
        val store = DoctorStore.open(root, DOCTOR_TEST_BUILD, clock)
        snapshot = store.prepareExport(store.markIssue(clock.wallUtcMs()).success()).success()
        armed = true
        assertEquals(DoctorStoreFailure.IO_ERROR, store.readRecords().failure())
        assertFalse(armed)
        assertTrue(Files.isDirectory(snapshot.archive))
        assertTrue(Files.isRegularFile(root.resolve("preserved.zip")))
    }

    private fun line(event: DoctorEventRecord) =
        (DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, event) as DoctorEventEncode.Accepted).line + "\n"
}
