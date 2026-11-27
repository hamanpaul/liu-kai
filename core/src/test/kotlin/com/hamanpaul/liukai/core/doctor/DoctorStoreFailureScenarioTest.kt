package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorStoreFailureScenarioTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `cached retained bounds follow partial expiration restart and clear`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        assertEquals(null, store.status().oldestRetainedMs)
        store.enable().success()
        val start = context.clock.wallUtcMs()
        store.append(context.touchEvent(store, 1, wallUtcMs = start)).success()
        store.append(context.touchEvent(store, 2, wallUtcMs = start + 1_000)).success()
        assertEquals(start, store.status().oldestRetainedMs)
        assertEquals(start + 1_000, store.status().newestRetainedMs)
        context.clock.setWallUtcMs(start + context.limits.maxAgeMs + 500)
        assertEquals(1, store.readRecords().success().size)
        assertEquals(start + 1_000, store.status().oldestRetainedMs)
        val reopened = context.open()
        assertEquals(start + 1_000, reopened.status().oldestRetainedMs)
        assertEquals(start + 1_000, reopened.status().newestRetainedMs)
        reopened.clear().success()
        assertEquals(null, reopened.status().oldestRetainedMs)
        assertEquals(null, reopened.status().newestRetainedMs)
    }

    @Test fun `failed epoch commit blocks old reads and exports until a successful clear retry`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val mark = store.markIssue(context.clock.wallUtcMs()).success()
        val snapshot = store.prepareExport(mark).success()
        context.faults.failNext(DoctorIoOperation.COMMIT_EPOCH, DoctorStoreFailure.IO_ERROR)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.clear().failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.readRecords().failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.markIssue(context.clock.wallUtcMs()).failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.prepareExport(mark).failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.previewExport(snapshot).failure())
        val destination = context.root.resolve("must-not-exist.zip")
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.saveExport(snapshot, destination).failure())
        assertFalse(Files.exists(destination))
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.enable().failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.disable().failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, context.open().readRecords().failure())
        store.clear().success()
        assertTrue(store.readRecords().success().isEmpty())
    }

    @Test fun `pending clear rejects every new operation then retry completes the same epoch`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val mark = store.markIssue(context.clock.wallUtcMs()).success()
        context.faults.failNext(DoctorIoOperation.DELETE_OLD_RECORD, DoctorStoreFailure.PERMISSION_DENIED)
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.clear().failure())
        val epoch = store.status().epoch
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.append(context.touchEvent(store, 2)).failure())
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.markIssue(context.clock.wallUtcMs()).failure())
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.prepareExport(mark.copy(epoch = epoch)).failure())
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.enable().failure())
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.disable().failure())
        assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, store.readRecords().failure())
        store.clear().success()
        assertEquals(epoch, store.status().epoch)
        assertTrue(store.status().enabled)
        assertFalse(store.status().pendingClear)
        assertTrue(store.readRecords().success().isEmpty())
    }

    @Test fun `invalid identifiers epochs and oversized records never enter history`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        assertEquals(DoctorStoreFailure.EPOCH_MISMATCH,
            store.append(context.touchEvent(store, 1).copy(epoch = store.status().epoch + 1)).failure())
        assertEquals(DoctorStoreFailure.INVALID_SCHEMA,
            store.append(context.touchEvent(store, 1).copy(producerId = 0)).failure())
        assertTrue(store.readRecords().success().isEmpty())
        for (limits in listOf(DoctorStoreLimits.DEFAULT.copy(maxRecordBytes = 8),
            DoctorStoreLimits.DEFAULT.copy(maxRawBytes = 8))) {
            val limited = DoctorStoreTestContext(temporary.newFolder().toPath(), limits = limits)
            val small = limited.open()
            small.enable().success()
            assertEquals(DoctorStoreFailure.RECORD_LIMIT, small.append(limited.touchEvent(small, 1)).failure())
            assertEquals(0L, small.status().rawBytes)
        }
    }

    @Test fun `negative mark parameters and a duplicate live preview are rejected`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val good = store.markIssue(context.clock.wallUtcMs()).success()
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.markIssue(-1).failure())
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.markIssue(0, DoctorExportWindow(-1, 0)).failure())
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.markIssue(0, DoctorExportWindow(0, -1)).failure())
        for (mark in listOf(good.copy(actionTimeMs = -1), good.copy(selectedIssueTimeMs = -1),
            good.copy(window = DoctorExportWindow(-1, 0)), good.copy(window = DoctorExportWindow(0, -1)))) {
            assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.prepareExport(mark).failure())
        }
        context.clock.setWallUtcMs(-1)
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.markIssue(0).failure())
        context.clock.setWallUtcMs(DoctorStoreTestContext.START_WALL_MS)
        val first = store.prepareExport(good).success()
        assertEquals(DoctorStoreFailure.EXPORT_EXISTS, store.prepareExport(good).failure())
        assertTrue(Files.exists(first.archive))
        store.cancelExport(first).success()
    }

    @Test fun `save refuses a snapshot alias or missing parent and leaves a retryable preview`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
        val before = Files.readAllBytes(snapshot.archive)
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.saveExport(snapshot, snapshot.archive).failure())
        val alias = context.root.resolve("alias.zip")
        Files.createLink(alias, snapshot.archive)
        assertEquals(DoctorStoreFailure.INVALID_REQUEST, store.saveExport(snapshot, alias).failure())
        assertEquals(DoctorStoreFailure.INVALID_REQUEST,
            store.saveExport(snapshot, context.root.resolve("missing/saved.zip")).failure())
        assertTrue(before.contentEquals(Files.readAllBytes(snapshot.archive)))
        assertTrue(before.contentEquals(Files.readAllBytes(alias)))
        val destination = context.root.resolve("saved.zip")
        store.saveExport(snapshot, destination).success()
        assertTrue(before.contentEquals(Files.readAllBytes(destination)))
    }
}
