package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorCrashRecoveryTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `reopen 後完整但缺 LF 的尾端不得與下一筆接成一行`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val before = context.open()
        before.enable().success()
        val first = context.touchEvent(before, 1)
        before.append(first).success()
        val record = context.doctorRecordFiles().single()
        val original = Files.readAllBytes(record)
        assertEquals('\n'.code.toByte(), original.last())
        val withoutTerminator = original.copyOf(original.size - 1)
        Files.write(record, withoutTerminator)

        val reopened = context.open()
        val second = context.touchEvent(reopened, 2)
        reopened.append(second).success()

        assertTrue(withoutTerminator.contentEquals(Files.readAllBytes(record)))
        assertEquals(2, context.doctorRecordFiles().size)
        assertEquals(listOf(first, second), reopened.readRecords().success())
        assertEquals(listOf(first, second), context.open().readRecords().success())
    }

    @Test
    fun `clear 刪除崩潰留下的自有 raw staging 但不刪無關檔案`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val record = context.doctorRecordFiles().single()
        val staging = record.resolveSibling(".${record.fileName}.tmp")
        Files.copy(record, staging)
        val uniqueStaging = record.resolveSibling(
            ".${record.fileName}.01234567-89ab-4cde-8fab-0123456789ab.tmp",
        )
        Files.copy(record, uniqueStaging)
        val controlStaging = context.root.resolve(".doctor-control-v1.tmp")
        Files.copy(context.root.resolve("doctor-control-v1"), controlStaging)
        val uniqueControlStaging = context.root.resolve(
            ".doctor-control-v1.01234567-89ab-4cde-8fab-0123456789ab.tmp",
        )
        Files.copy(context.root.resolve("doctor-control-v1"), uniqueControlStaging)
        val unrelated = record.resolveSibling("unrelated.txt")
        Files.write(unrelated, byteArrayOf(7, 8, 9))

        store.clear().success()

        assertFalse(Files.exists(staging), "old-epoch events remain in owned staging file")
        assertFalse(Files.exists(uniqueStaging), "unique raw staging remains after clear")
        assertFalse(Files.exists(controlStaging), "stale control staging remains after clear")
        assertFalse(Files.exists(uniqueControlStaging), "unique control staging remains after clear")
        assertTrue(store.readRecords().success().isEmpty())
        assertTrue(Files.exists(unrelated), "clear must stay inside its owned file allowlist")
    }

    @Test
    fun `reopen 清除崩潰留下的 raw staging 並保留完整正式記錄`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val before = context.open()
        before.enable().success()
        val event = context.touchEvent(before, 1)
        before.append(event).success()
        val record = context.doctorRecordFiles().single()
        val staging = record.resolveSibling(".${record.fileName}.tmp")
        Files.copy(record, staging)

        val reopened = context.open()

        assertFalse(Files.exists(staging), "incomplete replacement must not survive recovery")
        assertEquals(listOf(event), reopened.readRecords().success())
    }

    @Test
    fun `reopen staging 刪除故障可見且下次 reopen 續清並保留 symlink`() {
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(
            temporaryFolder.newFolder().toPath(),
            faults = faults,
        )
        val before = context.open()
        before.enable().success()
        val event = context.touchEvent(before, 1)
        before.append(event).success()
        val record = context.doctorRecordFiles().single()
        val staging = record.resolveSibling(".${record.fileName}.tmp")
        Files.copy(record, staging)
        val symlink = record.resolveSibling(".doctor-record-99999999.jsonl.tmp")
        Files.createSymbolicLink(symlink, record.fileName)
        faults.failNext(DoctorIoOperation.DELETE_OLD_RECORD, DoctorStoreFailure.PERMISSION_DENIED)

        val failed = context.open()

        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, failed.status().failure)
        assertTrue(Files.exists(staging))
        assertTrue(Files.isSymbolicLink(symlink))

        val resumed = context.open()
        assertFalse(Files.exists(staging))
        assertTrue(Files.isSymbolicLink(symlink))
        assertEquals(listOf(event), resumed.readRecords().success())
    }

    @Test
    fun `clear 的 staging 刪除失敗保持 pendingClear 且 reopen 才完成`() {
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(
            temporaryFolder.newFolder().toPath(),
            faults = faults,
        )
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val record = context.doctorRecordFiles().single()
        val staging = record.resolveSibling(".${record.fileName}.tmp")
        Files.copy(record, staging)
        faults.failNext(DoctorIoOperation.DELETE_OLD_RECORD, DoctorStoreFailure.PERMISSION_DENIED)

        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.clear().failure())
        assertTrue(store.status().pendingClear)
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, store.status().failure)
        assertTrue(Files.exists(staging))

        val resumed = context.open()
        assertFalse(resumed.status().pendingClear)
        assertTrue(resumed.readRecords().success().isEmpty())
        assertFalse(Files.exists(staging))
    }
}
