package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorHealthRecoveryTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `沒有任何按鍵的固定健康訊號在寫入失敗後仍能恢復並讀回`() {
        val faults = ScriptedDoctorIoFaults()
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath(), faults = faults)
        val store = context.open()
        store.enable().success()
        val first = context.healthEvent(store, 1, DoctorInputScope.UNKNOWN)
        store.append(first).success()
        faults.failNext(DoctorIoOperation.APPEND_RECORD, DoctorStoreFailure.DISK_FULL)
        assertEquals(
            DoctorStoreFailure.DISK_FULL,
            store.append(context.healthEvent(store, 2, DoctorInputScope.UNKNOWN)).failure(),
        )
        val resumed = context.healthEvent(store, 3, DoctorInputScope.UNKNOWN)
        store.append(resumed).success()

        assertEquals(listOf(first, resumed), context.open().readRecords().success())
    }
}
