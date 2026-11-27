package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import java.util.UUID
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorAbandonedExportTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `cancel abandoned preview preserves consent raw history symlinks and foreign files`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val history = store.readRecords().success()
        val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
        val foreign = snapshot.archive.parent.resolve("unrelated.zip")
        Files.writeString(foreign, "foreign")
        val outside = temporary.newFile().toPath()
        Files.writeString(outside, "outside")
        val link = snapshot.archive.parent.resolve("doctor-export-${UUID.randomUUID()}.zip")
        Files.createSymbolicLink(link, outside)

        store.cancelPendingExports().success()

        assertFalse(Files.exists(snapshot.archive))
        assertTrue(Files.isSymbolicLink(link))
        assertEquals("outside", Files.readString(outside))
        assertEquals("foreign", Files.readString(foreign))
        assertEquals(history, store.readRecords().success())
        assertTrue(store.status().enabled)
        assertEquals(0L, store.status().tempBytes)
        store.cancelPendingExports().success()
        store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
    }

    @Test fun `failed cancellation retains preview and can be retried without deleting raw history`() {
        val context = DoctorStoreTestContext(temporary.newFolder().toPath())
        val store = context.open()
        val snapshot = store.prepareExport(store.markIssue(context.clock.wallUtcMs()).success()).success()
        context.faults.failNext(DoctorIoOperation.DELETE_OLD_RECORD, DoctorStoreFailure.PERMISSION_DENIED)
        assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.PERMISSION_DENIED), store.cancelPendingExports())
        assertTrue(Files.exists(snapshot.archive))
        store.cancelPendingExports().success()
        assertFalse(Files.exists(snapshot.archive))
        assertFalse(store.status().enabled)
    }
}
