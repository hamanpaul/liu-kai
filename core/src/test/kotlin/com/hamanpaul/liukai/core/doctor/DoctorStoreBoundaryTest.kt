package com.hamanpaul.liukai.core.doctor

import java.io.IOException
import java.nio.file.AccessDeniedException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.ReadOnlyFileSystemException
import java.nio.file.StandardOpenOption
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorStoreBoundaryTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `limits 拒絕零 負數與不允許的負 age`() {
        val invalid = listOf(
            { DoctorStoreLimits.DEFAULT.copy(maxRawBytes = 0) },
            { DoctorStoreLimits.DEFAULT.copy(maxRawBytes = -1) },
            { DoctorStoreLimits.DEFAULT.copy(maxAgeMs = -1) },
            { DoctorStoreLimits.DEFAULT.copy(maxRecordBytes = 0) },
            { DoctorStoreLimits.DEFAULT.copy(maxExportBytes = 0) },
            { DoctorStoreLimits.DEFAULT.copy(maxRawAndTempBytes = 0) },
            { DoctorStoreLimits.DEFAULT.copy(exportTtlMs = 0) },
        )
        invalid.forEach { build ->
            assertFailsWith<IllegalArgumentException> { build() }
        }
        assertEquals(0, DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 0).maxAgeMs)
    }

    @Test
    fun `無故障 seam 的預設實作可啟用並持久化 epoch 零與 sequence 零`() {
        val root = temporaryFolder.newFolder().toPath()
        val clock = ManualDoctorClock()
        val store = DoctorStore.open(root, DOCTOR_TEST_BUILD, clock)
        assertEquals(0, store.status().epoch)
        store.enable().success()
        val event = DoctorStoreTestContext(root, clock).touchEvent(store, 0)
        store.append(event).success()

        val reopened = DoctorStore.open(root, DOCTOR_TEST_BUILD, clock)
        assertEquals(listOf(event), reopened.readRecords().success())
        assertEquals(0, reopened.status().sequenceGap)
    }

    @Test
    fun `typed IO security readonly 故障只投影固定 reason 且 programming error 外傳`() {
        val cases = listOf(
            AccessDeniedException("private-path") to DoctorStoreFailure.PERMISSION_DENIED,
            SecurityException("private-security") to DoctorStoreFailure.PERMISSION_DENIED,
            ReadOnlyFileSystemException() to DoctorStoreFailure.IO_ERROR,
            IOException("private-io") to DoctorStoreFailure.IO_ERROR,
        )
        cases.forEachIndexed { index, (failure, expected) ->
            val faults = ThrowingDoctorIoFaults()
            faults.throwNext(DoctorIoOperation.PERSIST_CONTROL, failure)
            val store = DoctorStoreTestContext(
                temporaryFolder.newFolder("typed-$index").toPath(),
                faults = faults,
            ).open()
            assertEquals(expected, store.enable().failure())
            assertEquals(expected, store.status().failure)
            assertFalse(store.status().enabled)
        }

        val programmingFault = IllegalStateException("private-programming-error")
        val faults = ThrowingDoctorIoFaults()
        faults.throwNext(DoctorIoOperation.PERSIST_CONTROL, programmingFault)
        val store = DoctorStoreTestContext(
            temporaryFolder.newFolder("programming").toPath(),
            faults = faults,
        ).open()
        assertFailsWith<IllegalStateException> { store.enable() }
    }

    @Test
    fun `append 與 export 邊界 fault 採用相同固定投影且保留已落盤資料`() {
        val context = DoctorStoreTestContext(
            temporaryFolder.newFolder().toPath(),
            faults = ThrowingDoctorIoFaults(),
        )
        val faults = context.faults as ThrowingDoctorIoFaults
        val store = context.open()
        store.enable().success()
        val first = context.touchEvent(store, 1)
        store.append(first).success()

        faults.throwNext(DoctorIoOperation.APPEND_RECORD, IOException("private-append"))
        assertEquals(
            DoctorStoreFailure.IO_ERROR,
            store.append(context.touchEvent(store, 2)).failure(),
        )
        assertEquals(listOf(first), store.readRecords().success())

        faults.throwNext(DoctorIoOperation.CREATE_EXPORT, AccessDeniedException("private-export"))
        assertEquals(
            DoctorStoreFailure.PERMISSION_DENIED,
            store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).failure(),
        )
        faults.throwNext(DoctorIoOperation.SAVE_EXPORT, ReadOnlyFileSystemException())
        val snapshot = store.prepareExport(
            store.markIssue(context.clock.wallUtcMs()).success(),
        ).success()
        assertEquals(
            DoctorStoreFailure.IO_ERROR,
            store.saveExport(snapshot, context.root.resolve("saved.zip")).failure(),
        )
        assertTrue(Files.exists(snapshot.archive))
    }

    @Test
    fun `initialize cleanup 的 typed 故障可重試且不得吞掉 Linux style security error`() {
        val faults = ThrowingDoctorIoFaults()
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root, faults = faults)
        val initial = context.open()
        initial.enable().success()
        initial.append(context.touchEvent(initial, 1)).success()
        val record = context.doctorRecordFiles().single()
        val staging = record.resolveSibling(".${record.fileName}.tmp")
        Files.copy(record, staging)

        faults.throwNext(
            DoctorIoOperation.DELETE_OLD_RECORD,
            AccessDeniedException(staging.toString()),
        )
        val failed = context.open()
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, failed.status().failure)
        assertTrue(Files.exists(staging))

        faults.throwNext(
            DoctorIoOperation.DELETE_OLD_RECORD,
            SecurityException("private-security"),
        )
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, context.open().status().failure)
        assertTrue(Files.exists(staging))

        val recovered = context.open()
        assertNull(recovered.status().failure)
        assertFalse(Files.exists(staging))
    }

    @Test
    fun `控制檔缺失與欄位合規值涵蓋 epoch gap false failure 的正常讀取`() {
        val root = temporaryFolder.newFolder().toPath()
        val defaultStore = DoctorStoreTestContext(root).open()
        assertFalse(defaultStore.status().enabled)
        assertEquals(0, defaultStore.status().epoch)
        assertEquals(0, defaultStore.status().sequenceGap)

        val validRoot = temporaryFolder.newFolder().toPath()
        Files.writeString(
            validRoot.resolve(CONTROL_FILE),
            controlText(
                enabled = "1",
                epoch = "0",
                pendingClear = "0",
                failure = "IO_ERROR",
                sequenceGap = Long.MAX_VALUE.toString(),
            ),
        )
        val valid = DoctorStoreTestContext(validRoot).open()
        assertTrue(valid.status().enabled)
        assertFalse(valid.status().collecting)
        assertEquals(DoctorStoreFailure.IO_ERROR, valid.status().failure)
        assertEquals(Long.MAX_VALUE, valid.status().sequenceGap)
    }

    @Test
    fun `控制檔每個語法與數值錯誤都降為 INVALID_SCHEMA`() {
        val canonical = controlText()
        val invalid = listOf(
            canonical.replace(CONTROL_MAGIC, "UNKNOWN"),
            canonical.lineSequence().take(5).joinToString("\n"),
            canonical.replace("enabled=0", "=0"),
            canonical.replace("enabled=0", "enabled"),
            canonical.replace("enabled=0", "enabled=0=extra"),
            canonical.replace("epoch=0", "enabled=1"),
            canonical.replace("sequenceGap=0", "unknown=0"),
            canonical.replace("epoch=0", "epoch=x"),
            canonical.replace("epoch=0", "epoch=-1"),
            canonical.replace("sequenceGap=0", "sequenceGap=x"),
            canonical.replace("sequenceGap=0", "sequenceGap=-1"),
            canonical.replace("enabled=0", "enabled=2"),
            canonical.replace("pendingClear=0", "pendingClear=2"),
            canonical.replace("failure=NONE", "failure=UNKNOWN"),
        )
        invalid.forEachIndexed { index, text ->
            val root = temporaryFolder.newFolder("control-$index").toPath()
            Files.writeString(root.resolve(CONTROL_FILE), text)
            assertEquals(
                DoctorStoreFailure.INVALID_SCHEMA,
                DoctorStoreTestContext(root).open().status().failure,
                "case $index",
            )
        }
    }

    @Test
    fun `root control records 與 temp 非預期檔案型態固定拒絕`() {
        val rootFile = temporaryFolder.newFile("root-file").toPath()
        Files.writeString(rootFile, "not a directory")
        assertEquals(
            DoctorStoreFailure.IO_ERROR,
            DoctorStoreTestContext(rootFile).open().status().failure,
        )

        listOf("control", "records", "temp").forEach { kind ->
            val root = temporaryFolder.newFolder(kind).toPath()
            val path = when (kind) {
                "control" -> root.resolve(CONTROL_FILE)
                else -> root.resolve(kind)
            }
            Files.createDirectories(path.takeIf { kind == "control" } ?: path)
            val expected = when (kind) {
                "control" -> DoctorStoreFailure.INVALID_SCHEMA
                else -> DoctorStoreFailure.IO_ERROR
            }
            if (kind == "control") {
                Files.writeString(path.resolve("child"), "x")
            } else {
                assertTrue(Files.deleteIfExists(path))
                Files.writeString(path, "not a directory")
            }
            assertEquals(expected, DoctorStoreTestContext(root).open().status().failure)
        }
    }

    @Test
    fun `commit 到 Long MAX 前就 fail closed 且 enable 不會解除 epoch failure`() {
        val root = temporaryFolder.newFolder().toPath()
        Files.writeString(
            root.resolve(CONTROL_FILE),
            controlText(
                enabled = "1",
                epoch = Long.MAX_VALUE.toString(),
            ),
        )
        val context = DoctorStoreTestContext(root)
        val store = context.open()

        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.clear().failure())
        assertEquals(Long.MAX_VALUE, store.status().epoch)
        assertFalse(store.status().enabled)
        assertFalse(store.status().pendingClear)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.status().failure)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.enable().failure())
        assertEquals(
            DoctorStoreFailure.DISABLED,
            store.append(context.touchEvent(store, 1)).failure(),
        )
    }

    @Test
    fun `clear 的 stop 與 pending failure 持久化都保持狀態與原因`() {
        val stopFaults = ScriptedDoctorIoFaults()
        val stopContext = DoctorStoreTestContext(
            temporaryFolder.newFolder("stop").toPath(),
            faults = stopFaults,
        )
        val stopStore = stopContext.open()
        stopStore.enable().success()
        stopFaults.failNext(
            DoctorIoOperation.PERSIST_CONTROL,
            DoctorStoreFailure.PERMISSION_DENIED,
        )
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, stopStore.clear().failure())
        assertTrue(stopStore.status().enabled)
        assertFalse(stopStore.status().collecting)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, stopStore.status().failure)
        assertFalse(stopStore.status().pendingClear)

        val pendingFaults = ThrowOnSecondControlWrite()
        val pendingContext = DoctorStoreTestContext(
            temporaryFolder.newFolder("pending").toPath(),
            faults = pendingFaults,
        )
        val pendingStore = pendingContext.open()
        pendingStore.enable().success()
        pendingStore.append(pendingContext.touchEvent(pendingStore, 1)).success()
        pendingFaults.arm(DoctorStoreFailure.DISK_FULL)
        assertEquals(
            DoctorStoreFailure.CLEAR_INCOMPLETE,
            pendingStore.clear().failure(),
        )
        assertTrue(pendingStore.status().pendingClear)
        assertEquals(DoctorStoreFailure.DISK_FULL, pendingStore.status().failure)
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, pendingStore.enable().failure())
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, pendingStore.disable().failure())
    }

    @Test
    fun `record byte limits 在 append 前固定拒絕且 rotation failure 不損舊段`() {
        val recordRoot = temporaryFolder.newFolder("record-limit").toPath()
        val recordContext = DoctorStoreTestContext(
            recordRoot,
            limits = DoctorStoreLimits.DEFAULT.copy(maxRecordBytes = 1),
        )
        val recordStore = recordContext.open()
        recordStore.enable().success()
        assertEquals(
            DoctorStoreFailure.RECORD_LIMIT,
            recordStore.append(recordContext.touchEvent(recordStore, 1)).failure(),
        )
        assertEquals(0, recordStore.status().rawBytes)

        val rawRoot = temporaryFolder.newFolder("raw-limit").toPath()
        val rawContext = DoctorStoreTestContext(
            rawRoot,
            limits = DoctorStoreLimits.DEFAULT.copy(
                maxRecordBytes = 10_000,
                maxRawBytes = 1,
            ),
        )
        val rawStore = rawContext.open()
        rawStore.enable().success()
        assertEquals(
            DoctorStoreFailure.RECORD_LIMIT,
            rawStore.append(rawContext.touchEvent(rawStore, 1)).failure(),
        )

        val measurementContext = DoctorStoreTestContext(
            temporaryFolder.newFolder("measurement").toPath(),
        )
        val measurementStore = measurementContext.open()
        val segmentLimit = encode(measurementContext.touchEvent(measurementStore, 1))
            .toByteArray(Charsets.UTF_8)
            .size
            .toLong() + 1L
        val faults = ScriptedDoctorIoFaults()
        val rotateContext = DoctorStoreTestContext(
            temporaryFolder.newFolder("rotate").toPath(),
            limits = DoctorStoreLimits.DEFAULT.copy(maxRecordBytes = segmentLimit),
            faults = faults,
        )
        val rotateStore = rotateContext.open()
        rotateStore.enable().success()
        val first = rotateContext.touchEvent(rotateStore, 1)
        rotateStore.append(first).success()
        faults.failNext(DoctorIoOperation.ROTATE_RECORD, DoctorStoreFailure.PERMISSION_DENIED)
        assertEquals(
            DoctorStoreFailure.PERMISSION_DENIED,
            rotateStore.append(rotateContext.touchEvent(rotateStore, 2)).failure(),
        )
        assertEquals(listOf(first), rotateStore.readRecords().success())
    }

    @Test
    fun `gap 與舊 control failure 會飽和持久化並在下一筆 append 後恢復`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val health = context.healthEvent(
            store,
            1,
            DoctorInputScope.ORDINARY,
        ).copy(dropCount = Long.MAX_VALUE, gapFromSequence = 1)
        store.append(health).success()
        assertEquals(Long.MAX_VALUE, store.status().sequenceGap)
        val reopened = context.open()
        assertEquals(Long.MAX_VALUE, reopened.status().sequenceGap)
        assertEquals(listOf(health), reopened.readRecords().success())

        val controlRoot = temporaryFolder.newFolder("control-failure").toPath()
        Files.writeString(
            controlRoot.resolve(CONTROL_FILE),
            controlText(
                enabled = "1",
                failure = "IO_ERROR",
            ),
        )
        val recoveryContext = DoctorStoreTestContext(controlRoot)
        val recovering = recoveryContext.open()
        val recovered = recoveryContext.touchEvent(recovering, 1)
        recovering.append(recovered).success()
        assertNull(recovering.status().failure)
        assertEquals(listOf(recovered), recoveryContext.open().readRecords().success())
    }

    @Test
    fun `foreign epoch 及 age retention 只保留當前合法片段並累計 gap`() {
        val foreignRoot = temporaryFolder.newFolder("foreign").toPath()
        val foreignContext = DoctorStoreTestContext(foreignRoot)
        val before = foreignContext.open()
        before.enable().success()
        val current = foreignContext.touchEvent(before, 2)
        before.append(current).success()
        val record = foreignContext.doctorRecordFiles().single()
        val foreign = current.copy(epoch = 1, sequence = 1)
        Files.writeString(
            record,
            encode(foreign) + "\n" + encode(current) + "\n",
        )

        val foreignReopened = foreignContext.open()
        assertEquals(listOf(current), foreignReopened.readRecords().success())
        assertEquals(1, foreignReopened.status().sequenceGap)

        val limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 1_000)
        val agedRoot = temporaryFolder.newFolder("aged").toPath()
        val agedContext = DoctorStoreTestContext(agedRoot, limits = limits)
        val agedStore = agedContext.open()
        agedStore.enable().success()
        val expired = agedContext.touchEvent(
            agedStore,
            1,
            DoctorStoreTestContext.START_WALL_MS - 2_000,
        )
        val retained = agedContext.touchEvent(
            agedStore,
            2,
            DoctorStoreTestContext.START_WALL_MS,
        )
        agedStore.append(expired).success()
        agedStore.append(retained).success()
        agedContext.clock.setWallUtcMs(DoctorStoreTestContext.START_WALL_MS + 1)

        val agedReopened = agedContext.open()
        assertEquals(listOf(retained), agedReopened.readRecords().success())
        assertTrue(agedReopened.status().sequenceGap >= 1)

        val compactRoot = temporaryFolder.newFolder("compact").toPath()
        val compactContext = DoctorStoreTestContext(compactRoot)
        val compactStore = compactContext.open()
        compactStore.enable().success()
        val old = compactContext.touchEvent(compactStore, 1)
        compactStore.append(old).success()
        val rawBytes = compactStore.status().rawBytes
        val compacted = DoctorStoreTestContext(
            compactRoot,
            clock = compactContext.clock,
            limits = DoctorStoreLimits.DEFAULT.copy(maxRawBytes = rawBytes - 1),
        ).open()
        assertTrue(compacted.readRecords().success().isEmpty())
        assertEquals(0, compacted.status().rawBytes)
        assertTrue(compacted.status().sequenceGap > 0)
    }

    @Test
    fun `受損 record 在下筆 append 前 rotate 且最大 ordinal 固定拒絕`() {
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root)
        val store = context.open()
        store.enable().success()
        val records = root.resolve("records")
        Files.createDirectories(records)
        val damaged = records.resolve(
            "doctor-record-${Long.MAX_VALUE.toString().padStart(8, '0')}.jsonl",
        )
        Files.writeString(damaged, "{\"schemaVersion\":1}\n")

        val reopened = context.open()
        assertEquals(
            DoctorStoreFailure.INVALID_SCHEMA,
            reopened.status().failure,
        )
        assertEquals(
            DoctorStoreFailure.RECORD_LIMIT,
            reopened.append(context.touchEvent(reopened, 1)).failure(),
        )
        assertTrue(Files.exists(damaged))
    }

    private fun encode(event: DoctorEventRecord): String =
        when (val result = DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, event)) {
            is DoctorEventEncode.Accepted -> result.line
            is DoctorEventEncode.Rejected -> error("test event rejected: ${result.reason}")
        }

    private fun controlText(
        enabled: String = "0",
        epoch: String = "0",
        pendingClear: String = "0",
        failure: String = "NONE",
        sequenceGap: String = "0",
    ): String = buildString {
        appendLine(CONTROL_MAGIC)
        appendLine("enabled=$enabled")
        appendLine("epoch=$epoch")
        appendLine("pendingClear=$pendingClear")
        appendLine("failure=$failure")
        appendLine("sequenceGap=$sequenceGap")
    }

    private companion object {
        const val CONTROL_FILE = "doctor-control-v1"
        const val CONTROL_MAGIC = "LIUKAI_DOCTOR_CONTROL_V1"
    }
}

private class ThrowingDoctorIoFaults : ScriptedDoctorIoFaults() {
    private val thrown = mutableMapOf<DoctorIoOperation, MutableList<Exception>>()

    fun throwNext(operation: DoctorIoOperation, failure: Exception) {
        thrown.getOrPut(operation) { mutableListOf() } += failure
    }

    override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
        val queue = thrown[operation]
        if (!queue.isNullOrEmpty()) throw queue.removeAt(0)
        return super.failureBefore(operation)
    }
}

private class ThrowOnSecondControlWrite : ScriptedDoctorIoFaults() {
    private var controlWrites = 0
    private var armedAt = Int.MIN_VALUE
    private var deleteReason: DoctorStoreFailure? = null

    fun arm(deleteReason: DoctorStoreFailure) {
        this.armedAt = controlWrites
        this.deleteReason = deleteReason
    }

    override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
        if (operation == DoctorIoOperation.PERSIST_CONTROL) {
            controlWrites += 1
        }
        if (armedAt != Int.MIN_VALUE && operation == DoctorIoOperation.DELETE_OLD_RECORD) {
            return deleteReason
        }
        if (
            armedAt != Int.MIN_VALUE &&
            operation == DoctorIoOperation.PERSIST_CONTROL &&
            controlWrites == armedAt + 2
        ) {
            return deleteReason
        }
        return null
    }
}
