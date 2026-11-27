package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorClearFailureRecoveryTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `uncommitted clear intent preserves consent and blocks stale data until retry succeeds`() {
        for (enabled in listOf(true, false)) {
            val context = DoctorStoreTestContext(temporary.newFolder().toPath())
            val store = context.open()
            store.enable().success()
            store.append(context.touchEvent(store, 1)).success()
            if (!enabled) store.disable().success()
            val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
            val epoch = store.status().epoch
            context.faults.failNext(DoctorIoOperation.PERSIST_CONTROL, DoctorStoreFailure.DISK_FULL)
            assertEquals(DoctorStoreFailure.DISK_FULL, store.clear().failure())
            assertEquals(enabled, store.status().enabled)
            assertFalse(store.status().collecting)
            assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.status().failure)
            assertEquals(null, store.status().oldestRetainedMs)
            assertEquals(null, store.status().newestRetainedMs)
            assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.readRecords().failure())
            assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.previewExport(snapshot).failure())
            assertEquals(if (enabled) DoctorStoreFailure.EPOCH_COMMIT_FAILED else DoctorStoreFailure.DISABLED,
                store.append(context.touchEvent(store, 2)).failure())
            store.clear().success()
            assertEquals(enabled, store.status().enabled)
            assertEquals(enabled, store.status().collecting)
            assertEquals(epoch + 1, store.status().epoch)
            assertTrue(store.readRecords().success().isEmpty())
        }
    }

    @Test fun `failed epoch and failure-marker writes cannot reopen old records after restart`() {
        var failedCommit = false
        var failedMarker = false
        val faults = object : ScriptedDoctorIoFaults() {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? {
                if (operation == DoctorIoOperation.COMMIT_EPOCH && !failedCommit) {
                    failedCommit = true
                    return DoctorStoreFailure.DISK_FULL
                }
                if (operation == DoctorIoOperation.PERSIST_CONTROL && failedCommit && !failedMarker) {
                    failedMarker = true
                    return DoctorStoreFailure.PERMISSION_DENIED
                }
                return null
            }
        }
        val context = DoctorStoreTestContext(temporary.newFolder().toPath(), faults = faults)
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.clear().failure())
        assertTrue(failedCommit && failedMarker)
        assertFalse(store.status().enabled)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, store.readRecords().failure())
        val reopened = context.open()
        assertFalse(reopened.status().enabled)
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, reopened.readRecords().failure())
        assertEquals(DoctorStoreFailure.EPOCH_COMMIT_FAILED, reopened.enable().failure())
        reopened.clear().success()
        assertTrue(reopened.readRecords().success().isEmpty())
        assertEquals(1L, reopened.status().epoch)
    }
}
