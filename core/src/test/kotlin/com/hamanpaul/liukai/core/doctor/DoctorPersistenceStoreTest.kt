package com.hamanpaul.liukai.core.doctor

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorPersistenceStoreTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `預設關閉 明確 enable 才持久化 且 stop 不刪既有 history`() {
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root)
        val store = context.open()

        assertFalse(store.status().enabled)
        assertFalse(store.status().collecting)
        assertEquals(DoctorStoreFailure.DISABLED, store.append(context.touchEvent(store, 1)).failure())

        store.enable().success()
        val event = context.touchEvent(store, 1)
        store.append(event).success()

        val reopened = context.open()
        assertTrue(reopened.status().enabled)
        assertTrue(reopened.status().collecting)
        assertEquals(listOf(event), reopened.readRecords().success())

        reopened.disable().success()
        assertEquals(
            DoctorStoreFailure.DISABLED,
            reopened.append(context.touchEvent(reopened, 2)).failure(),
        )
        val stopped = context.open()
        assertFalse(stopped.status().enabled)
        assertFalse(stopped.status().collecting)
        assertEquals(listOf(event), stopped.readRecords().success())
    }

    @Test
    fun `控制檔與寫入故障只回固定 enum 並保留先前完整事件`() {
        val root = temporaryFolder.newFolder().toPath()
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(root, faults = faults)
        faults.failNext(DoctorIoOperation.PERSIST_CONTROL, DoctorStoreFailure.PERMISSION_DENIED)

        val store = context.open()
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, store.enable().failure())
        assertFalse(store.status().enabled)
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, store.status().failure)

        store.enable().success()
        val first = context.touchEvent(store, 1)
        store.append(first).success()
        faults.failNext(DoctorIoOperation.APPEND_RECORD, DoctorStoreFailure.DISK_FULL)
        assertEquals(
            DoctorStoreFailure.DISK_FULL,
            store.append(context.touchEvent(store, 2)).failure(),
        )
        assertEquals(DoctorStoreFailure.DISK_FULL, store.status().failure)
        assertEquals(listOf(first), store.readRecords().success())

        val actualIoRoot = temporaryFolder.newFolder().toPath()
        val actualIoContext = DoctorStoreTestContext(actualIoRoot)
        val actualIoStore = actualIoContext.open()
        actualIoStore.enable().success()
        val records = actualIoRoot.resolve("records")
        if (Files.exists(records)) Files.delete(records)
        Files.writeString(records, "not a directory")
        assertEquals(
            DoctorStoreFailure.IO_ERROR,
            actualIoStore.append(actualIoContext.touchEvent(actualIoStore, 1)).failure(),
        )
    }

    @Test
    fun `rotation 與 raw 上限會先淘汰最舊片段並留下 gap`() {
        val limits = DoctorStoreLimits.DEFAULT.copy(
            maxRawBytes = 2_200,
            maxRecordBytes = 700,
        )
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root, limits = limits)
        val store = context.open()
        store.enable().success()

        val events = (1L..8L).map { sequence ->
            context.clock.advance(10)
            context.touchEvent(store, sequence).also {
                store.append(it).success()
                assertTrue(store.status().rawBytes <= limits.maxRawBytes)
            }
        }
        val retained = store.readRecords().success()
        val recordFiles = context.doctorRecordFiles()
        val rawBytes = recordFiles.sumOf { Files.size(it) }

        assertTrue(recordFiles.size > 1)
        assertTrue(recordFiles.all { Files.size(it) <= limits.maxRecordBytes })
        assertTrue(rawBytes <= limits.maxRawBytes)
        assertEquals(rawBytes, store.status().rawBytes)
        assertTrue(retained.size < events.size)
        assertEquals(events.last(), retained.last())
        assertTrue(store.status().sequenceGap > 0)
    }

    @Test
    fun `七天保留用毫秒邊界且開始與讀取都會清理過期 raw`() {
        val limits = DoctorStoreLimits.DEFAULT.copy(maxAgeMs = 1_000)
        val firstRoot = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(firstRoot, limits = limits)
        val store = context.open()
        store.enable().success()
        val original = context.touchEvent(store, 1)
        store.append(original).success()

        context.clock.setWallUtcMs(DoctorStoreTestContext.START_WALL_MS + 1_000)
        assertEquals(listOf(original), store.readRecords().success())
        context.clock.advance(1)
        assertTrue(store.readRecords().success().isEmpty())
        assertEquals(0, store.status().rawBytes)
        assertTrue(context.doctorRecordFiles().isEmpty())

        val secondRoot = temporaryFolder.newFolder().toPath()
        val restartContext = DoctorStoreTestContext(secondRoot, limits = limits)
        val beforeRestart = restartContext.open()
        beforeRestart.enable().success()
        beforeRestart.append(restartContext.touchEvent(beforeRestart, 1)).success()
        restartContext.clock.advance(1_001)

        val restarted = restartContext.open()
        assertTrue(restarted.readRecords().success().isEmpty())
        assertEquals(0, restarted.status().rawBytes)
        assertTrue(restartContext.doctorRecordFiles().isEmpty())
    }

    @Test
    fun `wall clock 後退與 monotonic 重置不改已落盤序號順序`() {
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root)
        val store = context.open()
        store.enable().success()
        val first = context.touchEvent(store, 1)
        store.append(first).success()

        context.clock.setWallUtcMs(DoctorStoreTestContext.START_WALL_MS - 10 * 24 * 60 * 60 * 1000)
        context.clock.advanceMonotonicMs(1)
        val second = context.touchEvent(store, 2)
        store.append(second).success()

        context.clock.resetMonotonic()
        val reopened = context.open()
        context.clock.advanceMonotonicMs(1)
        val third = context.touchEvent(reopened, 3)
        reopened.append(third).success()

        assertEquals(
            listOf(1L, 2L, 3L),
            reopened.readRecords().success().map { it.sequence },
        )
    }

    @Test
    fun `reopen 遇 torn tail 或非法 schema 只保留有效前綴並回固定狀態`() {
        val tornRoot = temporaryFolder.newFolder().toPath()
        val tornContext = DoctorStoreTestContext(tornRoot)
        val tornStore = tornContext.open()
        tornStore.enable().success()
        val valid = tornContext.touchEvent(tornStore, 1)
        tornStore.append(valid).success()
        val tail = doctorFixture("torn-tail.jsonl")
            .substringAfter('\n')
            .trimEnd('\r', '\n')
        Files.write(
            tornContext.doctorRecordFiles().single(),
            tail.toByteArray(StandardCharsets.UTF_8),
            StandardOpenOption.APPEND,
        )

        val tornReopened = tornContext.open()
        assertEquals(listOf(valid), tornReopened.readRecords().success())
        assertEquals(DoctorStoreFailure.TORN_TAIL, tornReopened.status().failure)
        assertEquals(1, tornReopened.status().ignoredLines)

        val invalidRoot = temporaryFolder.newFolder().toPath()
        val invalidContext = DoctorStoreTestContext(invalidRoot)
        val invalidStore = invalidContext.open()
        invalidStore.enable().success()
        val invalidPrefix = invalidContext.touchEvent(invalidStore, 1)
        invalidStore.append(invalidPrefix).success()
        Files.write(
            invalidContext.doctorRecordFiles().single(),
            doctorFixture("bad-schema.jsonl").toByteArray(StandardCharsets.UTF_8),
            StandardOpenOption.APPEND,
        )

        val invalidReopened = invalidContext.open()
        assertEquals(listOf(invalidPrefix), invalidReopened.readRecords().success())
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA, invalidReopened.status().failure)
        assertEquals(1, invalidReopened.status().ignoredLines)
    }

    @Test
    fun `敏感未知逐鍵拒絕不增加 raw sequence gap 或 opId 計數`() {
        val root = temporaryFolder.newFolder().toPath()
        val context = DoctorStoreTestContext(root)
        val store = context.open()
        store.enable().success()
        val sensitiveHealth = context.healthEvent(store, 1, DoctorInputScope.SENSITIVE)
        store.append(sensitiveHealth).success()
        val rawBeforeRejectedInput = store.status().rawBytes
        val gapsBeforeRejectedInput = store.status().sequenceGap
        val rejected = (1..25).flatMap { index ->
            listOf(
                context.touchEvent(
                    store,
                    sequence = index + 1L,
                    scope = DoctorInputScope.SENSITIVE,
                ),
                context.touchEvent(
                    store,
                    sequence = index + 1L,
                    scope = DoctorInputScope.UNKNOWN,
                ),
            )
        }

        rejected.forEach {
            assertEquals(DoctorStoreFailure.PRIVACY_REJECTED, store.append(it).failure())
        }

        assertEquals(rawBeforeRejectedInput, store.status().rawBytes)
        assertEquals(gapsBeforeRejectedInput, store.status().sequenceGap)
        val unknownHealth = context.healthEvent(store, 2, DoctorInputScope.UNKNOWN)
        store.append(unknownHealth).success()
        assertEquals(
            listOf(sensitiveHealth, unknownHealth),
            context.open().readRecords().success(),
        )
    }

    @Test
    fun `clear 先提交新 epoch 並封鎖舊 read snapshot export 再續清`() {
        val root = temporaryFolder.newFolder().toPath()
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(root, faults = faults)
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val mark = store.markIssue(context.clock.wallUtcMs()).success()
        val oldSnapshot = store.prepareExport(mark).success()
        val oldEpoch = store.status().epoch
        val destination = root.resolve("old-export.zip")
        faults.failNext(DoctorIoOperation.DELETE_OLD_RECORD, DoctorStoreFailure.PERMISSION_DENIED)

        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.clear().failure())
        val clearing = store.status()
        assertEquals(oldEpoch + 1, clearing.epoch)
        assertTrue(clearing.enabled)
        assertFalse(clearing.collecting)
        assertTrue(clearing.pendingClear)
        assertEquals(DoctorStoreFailure.PERMISSION_DENIED, clearing.failure)
        assertEquals(
            DoctorStoreFailure.EPOCH_MISMATCH,
            store.readRecords(oldEpoch).failure(),
        )
        assertEquals(
            DoctorStoreFailure.EPOCH_MISMATCH,
            store.previewExport(oldSnapshot).failure(),
        )
        assertEquals(
            DoctorStoreFailure.EPOCH_MISMATCH,
            store.saveExport(oldSnapshot, destination).failure(),
        )
        assertFalse(Files.exists(destination))

        val resumed = context.open()
        assertFalse(resumed.status().pendingClear)
        assertTrue(resumed.status().collecting)
        assertNull(resumed.status().failure)
        assertTrue(resumed.readRecords().success().isEmpty())
        assertFalse(Files.exists(oldSnapshot.archive))
        resumed.append(context.touchEvent(resumed, 1)).success()
        assertEquals(1, resumed.readRecords().success().size)
    }

    @Test
    fun `epoch 持久化失敗保持 off error 且不可假報 clear success`() {
        val root = temporaryFolder.newFolder().toPath()
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(root, faults = faults)
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val oldEpoch = store.status().epoch
        faults.failNext(DoctorIoOperation.COMMIT_EPOCH, DoctorStoreFailure.DISK_FULL)

        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.clear().failure())
        assertEquals(oldEpoch, store.status().epoch)
        assertFalse(store.status().enabled)
        assertFalse(store.status().collecting)
        assertFalse(store.status().pendingClear)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.status().failure)
        assertEquals(DoctorStoreFailure.DISABLED, store.append(context.touchEvent(store, 2)).failure())

        val reopened = context.open()
        assertFalse(reopened.status().enabled)
        assertFalse(reopened.status().collecting)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, reopened.status().failure)

        reopened.clear().success()
        assertEquals(oldEpoch + 1, reopened.status().epoch)
        assertTrue(reopened.readRecords().success().isEmpty())
        assertNull(reopened.status().failure)
    }
}

internal class DoctorStoreTestContext(
    val root: Path,
    val clock: ManualDoctorClock = ManualDoctorClock(),
    val limits: DoctorStoreLimits = DoctorStoreLimits.DEFAULT,
    val faults: ScriptedDoctorIoFaults = ScriptedDoctorIoFaults(),
) {
    fun open(): DoctorStore = DoctorStore.open(
        root = root,
        build = DOCTOR_TEST_BUILD,
        clock = clock,
        limits = limits,
        ioFaults = faults,
    )

    fun touchEvent(
        store: DoctorStore,
        sequence: Long,
        wallUtcMs: Long = clock.wallUtcMs(),
        scope: DoctorInputScope = DoctorInputScope.ORDINARY,
    ) = DoctorEventRecord(
        producerId = 42,
        sessionId = 7,
        epoch = store.status().epoch,
        operationId = 1_000 + sequence,
        sequence = sequence,
        monotonicNs = clock.monotonicNs() + sequence,
        wallUtcMs = wallUtcMs,
        timezoneOffsetMinutes = 480,
        eventCode = DoctorEventCode.TOUCH,
        callbackSource = DoctorCallbackSource.KEY_ACTION,
        scope = scope,
        mode = DoctorEventMode.LIU,
        phase = DoctorEventPhase.TOUCH,
        focus = true,
        handled = true,
        resultCode = DoctorResultCode.OK,
        durationUs = 1,
        gestureToken = 500 + sequence,
        pointerCount = 1,
        pressed = true,
    )

    fun healthEvent(
        store: DoctorStore,
        sequence: Long,
        scope: DoctorInputScope,
        wallUtcMs: Long = clock.wallUtcMs(),
    ) = DoctorEventRecord(
        producerId = 42,
        sessionId = 7,
        epoch = store.status().epoch,
        operationId = 0,
        sequence = sequence,
        monotonicNs = clock.monotonicNs() + sequence,
        wallUtcMs = wallUtcMs,
        timezoneOffsetMinutes = 480,
        eventCode = DoctorEventCode.HEALTH,
        callbackSource = DoctorCallbackSource.NONE,
        scope = scope,
        mode = DoctorEventMode.NONE,
        phase = DoctorEventPhase.HEALTH,
        focus = false,
        handled = true,
        resultCode = DoctorResultCode.OK,
        durationUs = 0,
    )

    fun doctorRecordFiles(): List<Path> {
        val records = root.resolve("records")
        if (!Files.exists(records)) return emptyList()
        return Files.list(records).use { paths ->
            paths.filter { it.fileName.toString().matches(DOCTOR_RECORD_NAME) }
                .toList()
                .sortedBy { it.fileName.toString() }
        }
    }

    companion object {
        const val START_WALL_MS = 1_764_057_600_000L
        const val START_MONOTONIC_NS = 9_000_000_000L
        private val DOCTOR_RECORD_NAME = Regex("doctor-record-\\d+\\.jsonl")
    }
}

internal class ManualDoctorClock(
    private var wallMs: Long = DoctorStoreTestContext.START_WALL_MS,
    private var monotonicNsValue: Long = DoctorStoreTestContext.START_MONOTONIC_NS,
) : DoctorClock {
    override fun wallUtcMs(): Long = wallMs

    override fun monotonicNs(): Long = monotonicNsValue

    fun setWallUtcMs(value: Long) {
        wallMs = value
    }

    fun advance(milliseconds: Long) {
        wallMs += milliseconds
        monotonicNsValue += milliseconds * 1_000_000
    }

    fun advanceMonotonicMs(milliseconds: Long) {
        monotonicNsValue += milliseconds * 1_000_000
    }

    fun resetMonotonic() {
        monotonicNsValue = 0
    }
}

internal open class ScriptedDoctorIoFaults : DoctorIoFaults {
    private val failures = mutableMapOf<DoctorIoOperation, MutableList<DoctorStoreFailure>>()

    fun failNext(operation: DoctorIoOperation, failure: DoctorStoreFailure) {
        failures.getOrPut(operation) { mutableListOf() }.add(failure)
    }

    override open fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
        val queue = failures[operation] ?: return null
        if (queue.isEmpty()) return null
        return queue.removeAt(0)
    }
}

internal val DOCTOR_TEST_BUILD = DoctorBuildMetadata(
    version = "0.1.0",
    sourceCommit = "e045f978eb52001abc8b632482a6f4536cbdf2ab",
)

internal fun doctorFixture(name: String): String =
    requireNotNull(DoctorStoreTestContext::class.java.getResource("/doctor-v1/$name")) {
        "missing fixture doctor-v1/$name"
    }.readText()

internal fun <T> DoctorStoreResult<T>.success(): T =
    when (this) {
        is DoctorStoreResult.Ok -> value
        is DoctorStoreResult.Error -> fail("unexpected Doctor store failure: $reason")
    }

internal fun DoctorStoreResult<*>.failure(): DoctorStoreFailure =
    when (this) {
        is DoctorStoreResult.Ok -> fail("expected Doctor store failure")
        is DoctorStoreResult.Error -> reason
    }
