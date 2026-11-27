package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorFilesystemRecoveryTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `missing root and a crash-created empty segment recover without losing the first event`() {
        val root = temporary.newFolder().toPath().resolve("not-created")
        assertFalse(Files.exists(root))
        val context = DoctorStoreTestContext(root)
        val store = context.open()
        assertTrue(Files.isDirectory(root))
        store.enable().success()
        val records = Files.createDirectories(root.resolve("records"))
        Files.createFile(records.resolve("doctor-record-00000000.jsonl"))
        val recovered = context.open()
        val event = context.touchEvent(recovered, 1)
        recovered.append(event).success()
        assertEquals(listOf(event), recovered.readRecords().success())
        assertEquals(1, context.doctorRecordFiles().size)
        Files.delete(context.doctorRecordFiles().single())
        assertTrue(recovered.readRecords().success().isEmpty())
        assertEquals(0L, recovered.status().rawBytes)
        assertNull(recovered.status().oldestRetainedMs)
    }

    @Test fun `records or export directory replaced by a file is rejected without deleting the replacement`() {
        for (name in listOf("records", "temp")) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            store.enable().success()
            store.append(context.touchEvent(store, 1)).success()
            val folder = context.root.resolve(name)
            Files.createDirectories(folder)
            val backup = folder.resolveSibling("preserved-$name")
            Files.move(folder, backup)
            Files.writeString(folder, "foreign replacement")
            val failed = if (name == "records") store.readRecords().failure() else
                store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).failure()
            assertEquals(DoctorStoreFailure.IO_ERROR, failed)
            assertEquals("foreign replacement", Files.readString(folder))
            assertTrue(Files.isDirectory(backup))
            if (name == "temp") assertEquals(DoctorStoreFailure.IO_ERROR, store.status().failure)
        }
    }

    @Test fun `failed atomic control replacement cleans its staging and preserves the prior control`() {
        val root = temporary.newFolder().toPath()
        val control = root.resolve("doctor-control-v1")
        val backup = root.resolve("preserved-control")
        var armed = false
        val faults = object : ScriptedDoctorIoFaults() {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
                if (armed && operation == DoctorIoOperation.PERSIST_CONTROL) {
                    armed = false
                    Files.move(control, backup)
                    Files.createDirectory(control)
                    Files.writeString(control.resolve("sentinel"), "preserve")
                }
                return null
            }
        }
        val context = DoctorStoreTestContext(root, faults = faults)
        val store = context.open()
        store.enable().success()
        val original = Files.readAllBytes(control)
        armed = true
        assertEquals(DoctorStoreFailure.IO_ERROR, store.disable().failure())
        assertTrue(original.contentEquals(Files.readAllBytes(backup)))
        assertEquals("preserve", Files.readString(control.resolve("sentinel")))
        Files.list(root).use { paths ->
            assertFalse(paths.anyMatch { it.fileName.toString().startsWith(".doctor-control-") })
        }
        Files.delete(control.resolve("sentinel"))
        Files.delete(control)
        Files.move(backup, control)
        assertTrue(context.open().status().enabled)
    }

    @Test fun `partially expired records revalidate a changed tail and retain only valid current events`() {
        for (replacement in listOf("bad-tail", "all-old", "all-current")) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath(),
                limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 1_000))
            val store = context.open()
            store.enable().success()
            val start = context.clock.wallUtcMs()
            val old = context.touchEvent(store, 1, wallUtcMs = start)
            val fresh = context.touchEvent(store, 2, wallUtcMs = start + 1_000)
            store.append(old).success()
            store.append(fresh).success()
            val record = context.doctorRecordFiles().single()
            fun line(event: DoctorEventRecord) =
                (DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, event) as DoctorEventEncode.Accepted).line + "\n"
            Files.writeString(record, when (replacement) {
                "bad-tail" -> line(old) + line(fresh) + "{bad}\n"
                "all-old" -> line(old)
                else -> line(fresh)
            })
            context.clock.setWallUtcMs(start + 1_500)
            val retained = store.readRecords().success()
            assertEquals(if (replacement == "all-old") emptyList() else listOf(fresh), retained)
            if (replacement == "bad-tail") {
                assertEquals(DoctorStoreFailure.INVALID_SCHEMA, store.status().failure)
                assertEquals(1L, store.status().ignoredLines)
            }
        }
    }

    @Test fun `restricted correlation and count failures preserve the same public privacy error`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val health = context.healthEvent(store, 1, DoctorInputScope.UNKNOWN)
        for (invalid in listOf(health.copy(operationId = 1), health.copy(dropCount = 1))) {
            assertEquals(DoctorStoreFailure.PRIVACY_REJECTED, store.append(invalid).failure())
        }
        assertTrue(store.readRecords().success().isEmpty())
        assertEquals(0L, store.status().rawBytes)
    }

    @Test fun `retention refuses a segment replaced by a link without deleting its target`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath(),
            limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 1_000))
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val record = context.doctorRecordFiles().single()
        Files.move(record, context.root.resolve("preserved-record"))
        val target = context.root.resolve("foreign-file")
        Files.writeString(target, "preserve")
        Files.createSymbolicLink(record, target)
        context.clock.advance(1_001)
        assertEquals(DoctorStoreFailure.IO_ERROR, store.readRecords().failure())
        assertEquals("preserve", Files.readString(target))
        assertTrue(Files.isSymbolicLink(record))
    }
}
