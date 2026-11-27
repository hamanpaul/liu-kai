package com.hamanpaul.liukai.core.doctor

import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.rules.TemporaryFolder

class DoctorRecordLinkTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun `首段檔名被符號連結佔用時不可寫向外部目標`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        val outside = temporaryFolder.newFile().toPath()
        Files.write(outside, byteArrayOf(1, 2, 3))
        val records = context.root.resolve("records")
        Files.createDirectories(records)
        val link = records.resolve("doctor-record-00000000.jsonl")
        Files.createSymbolicLink(link, outside)

        assertEquals(DoctorStoreFailure.IO_ERROR, store.append(context.touchEvent(store, 1)).failure())
        assertTrue(Files.isSymbolicLink(link))
        assertEquals(listOf<Byte>(1, 2, 3), Files.readAllBytes(outside).toList())
    }

    @Test
    fun `已開啟片段被替換為符號連結也不可繼續 append 外部目標`() {
        val context = DoctorStoreTestContext(temporaryFolder.newFolder().toPath())
        val store = context.open()
        store.enable().success()
        store.append(context.touchEvent(store, 1)).success()
        val record = context.doctorRecordFiles().single()
        val outside = temporaryFolder.newFile().toPath()
        Files.write(outside, byteArrayOf(4, 5, 6))
        Files.delete(record)
        Files.createSymbolicLink(record, outside)

        assertEquals(DoctorStoreFailure.IO_ERROR, store.append(context.touchEvent(store, 2)).failure())
        assertTrue(Files.isSymbolicLink(record))
        assertEquals(listOf<Byte>(4, 5, 6), Files.readAllBytes(outside).toList())
    }
}
