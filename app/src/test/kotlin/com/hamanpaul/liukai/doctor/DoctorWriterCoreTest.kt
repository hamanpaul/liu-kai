package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorBuildMetadata
import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorClock
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import com.hamanpaul.liukai.core.doctor.DoctorIoFaults
import com.hamanpaul.liukai.core.doctor.DoctorIoOperation
import com.hamanpaul.liukai.core.doctor.DoctorStore
import com.hamanpaul.liukai.core.doctor.DoctorStoreLimits
import com.hamanpaul.liukai.core.doctor.DoctorStoreFailure
import com.hamanpaul.liukai.core.doctor.DoctorStoreResult
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.OutputStream
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.attribute.PosixFilePermission
import java.util.zip.ZipInputStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

class DoctorWriterCoreTest {
    @Test
    fun `write failure stops the batch and remains visibly interrupted until explicit retry`() {
        var failurePending = true
        withFixture(faults = object : DoctorIoFaults {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                if (operation == DoctorIoOperation.APPEND_RECORD && failurePending) {
                    failurePending = false
                    DoctorStoreFailure.IO_ERROR
                } else null
        }) { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.enqueueTouch()

            assertEquals(1, fixture.writer.pumpOneBatch())
            assertEquals(0L, fixture.ring.depth)
            assertTrue(fixture.writer.state().enabled)
            assertFalse(fixture.writer.state().collecting)
            assertTrue(fixture.writer.state().interrupted)
            assertFalse(fixture.producer.isCollecting)
            assertTrue(fixture.records().isEmpty())

            fixture.writer.enable().okValue()
            assertTrue(fixture.writer.state().collecting)
            assertFalse(fixture.writer.state().interrupted)
            fixture.enqueueTouch()
            assertEquals(1, fixture.writer.pumpOneBatch())
            assertEquals(1, fixture.records().size)
        }
    }

    @Test
    fun `restricted lifecycle write failures stay non ordinary and recover to persisted ordinary input`() {
        listOf(DoctorInputScope.SENSITIVE, DoctorInputScope.UNKNOWN).forEach { scope ->
            var failurePending = true
            withFixture(faults = object : DoctorIoFaults {
                override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                    if (operation == DoctorIoOperation.APPEND_RECORD && failurePending) {
                        failurePending = false
                        DoctorStoreFailure.IO_ERROR
                    } else {
                        null
                    }
            }) { fixture ->
                fixture.writer.enable().okValue()
                fixture.producer.setInputState(scope, DoctorEventMode.NONE, focus = false)
                assertTrue(fixture.producer.recordLifecycle(
                    com.hamanpaul.liukai.core.doctor.DoctorEventCode.FOCUS,
                ))

                assertEquals(1, fixture.writer.pumpOneBatch())

                val failed = fixture.writer.state()
                assertTrue(failed.enabled)
                assertFalse(failed.collecting)
                assertTrue(failed.interrupted)
                assertFalse(failed.pendingClear)
                assertEquals(DoctorStoreFailure.IO_ERROR, failed.failure)
                assertFalse(fixture.producer.isCollecting)
                assertEquals(0L, fixture.metrics.snapshot().dropped)
                assertEquals(0L, fixture.metrics.snapshot().written)
                assertEquals(0L, fixture.ring.depth)
                assertTrue(fixture.records().isEmpty())

                fixture.producer.setInputState(
                    DoctorInputScope.ORDINARY,
                    DoctorEventMode.LIU,
                    focus = true,
                )
                fixture.writer.enable().okValue()
                assertTrue(fixture.producer.isOrdinaryCollecting)
                assertTrue(fixture.writer.state().collecting)
                assertFalse(fixture.writer.state().interrupted)
                assertEquals(null, fixture.writer.state().failure)

                fixture.enqueueTouch()
                assertEquals(1, fixture.writer.pumpOneBatch())

                val persisted = fixture.records().single()
                assertEquals(DoctorInputScope.ORDINARY, persisted.scope)
                assertEquals(fixture.writer.state().epoch, persisted.epoch)
                assertEquals(0L, fixture.metrics.snapshot().dropped)
                assertEquals(1L, fixture.metrics.snapshot().written)
                assertTrue(fixture.metrics.snapshot().writtenBytes > 0L)
            }
        }
    }

    @Test
    fun `synthetic default build metadata remains writable`() {
        withFixture(
            build = DoctorBuildMetadata("0.0.0-test", "0000000"),
        ) { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()

            assertEquals(1, fixture.writer.pumpOneBatch())

            assertEquals(1, fixture.records().size)
            assertTrue(fixture.writer.state().collecting)
            assertTrue(fixture.writer.state().rawBytes > 0L)
            assertFalse(fixture.writer.state().interrupted)
        }
    }

    @Test
    fun `export provider failure does not report an interrupted input collector`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            val prepared = fixture.writer.prepareExport().okValue()
            val unavailable = object : DoctorExportTarget {
                override fun validate(): Boolean = throw IOException("provider unavailable")
                override fun open(): OutputStream = error("must not open")
                override fun discard() = Unit
            }
            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.IO_ERROR),
                fixture.writer.saveExport(prepared.handle, unavailable))
            fixture.writer.refresh()
            assertTrue(fixture.writer.state().collecting)
            assertFalse(fixture.writer.state().interrupted)
            assertTrue(fixture.producer.isCollecting)
        }
    }

    @Test
    fun `visible retained range advances when rotation removes the oldest segment`() {
        withFixture { measure ->
            measure.writer.enable()
            measure.enqueueTouch()
            measure.writer.pumpOneBatch()
            val oneRecord = measure.writer.state().rawBytes
            assertTrue(oneRecord > 0)
            val root = Files.createTempDirectory("doctor-app-rotation")
            try {
                val limited = Fixture(root, DoctorIoFaults.NONE, DoctorStoreLimits.DEFAULT.copy(
                    maxRawBytes = oneRecord + 16, maxRecordBytes = oneRecord + 16))
                limited.writer.enable()
                repeat(3) {
                    limited.clock.wall += 1_000
                    limited.enqueueTouch()
                    assertEquals(1, limited.writer.pumpOneBatch())
                }
                val remaining = limited.records()
                assertEquals(1, remaining.size)
                assertEquals(remaining.single().wallUtcMs, limited.writer.state().oldestRetainedMs)
                assertEquals(remaining.single().wallUtcMs, limited.writer.state().newestRetainedMs)
            } finally { root.toFile().deleteRecursively() }
        }
    }

    @Test
    fun `new process writer cancels orphan preview without clearing raw history or consent`() {
        withFixture { fixture ->
            fixture.writer.enable()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            fixture.writer.markIssue(fixture.clock.wall)
            fixture.writer.prepareExport().okValue()
            val history = fixture.records()
            val restarted = Fixture(fixture.root, DoctorIoFaults.NONE)
            assertEquals(0L, restarted.writer.state().tempBytes)
            assertEquals(history, restarted.records())
            assertTrue(restarted.writer.state().enabled)
            restarted.writer.markIssue(restarted.clock.wall)
            assertTrue(restarted.writer.prepareExport() is DoctorStoreResult.Ok)
        }
    }

    @Test
    fun `startup orphan cleanup failure retains evidence and next process retries`() {
        var failCleanup = false
        val faults = object : DoctorIoFaults {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                if (failCleanup && operation == DoctorIoOperation.DELETE_OLD_RECORD) {
                    DoctorStoreFailure.IO_ERROR
                } else {
                    null
                }
        }
        withFixture(faults = faults) { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            fixture.writer.prepareExport().okValue()
            val history = fixture.records()
            assertTrue(fixture.writer.state().tempBytes > 0L)

            failCleanup = true
            val interruptedRestart = Fixture(fixture.root, faults)

            assertEquals(DoctorStoreFailure.IO_ERROR, interruptedRestart.writer.state().failure)
            assertTrue(interruptedRestart.writer.state().tempBytes > 0L)
            assertEquals(history, interruptedRestart.records())

            failCleanup = false
            val recoveredRestart = Fixture(fixture.root, faults)
            assertEquals(null, recoveredRestart.writer.state().failure)
            assertEquals(0L, recoveredRestart.writer.state().tempBytes)
            assertEquals(history, recoveredRestart.records())
        }
    }

    @Test
    fun `provider validation failures do not escape the writer or prevent later input and retry`() {
        for (failure in listOf(IOException("provider gone"), SecurityException("permission revoked"),
            IllegalStateException("provider unavailable"))) {
            withFixture { fixture ->
                fixture.writer.enable()
                fixture.enqueueTouch()
                fixture.writer.pumpOneBatch()
                fixture.writer.markIssue(fixture.clock.wall)
                val prepared = fixture.writer.prepareExport().okValue()
                var opened = false
                var discarded = false
                val target = object : DoctorExportTarget {
                    override fun validate(): Boolean = throw failure
                    override fun open(): OutputStream { opened = true; return ByteArrayOutputStream() }
                    override fun discard() { discarded = true }
                }
                val expected = if (failure is SecurityException) DoctorStoreFailure.PERMISSION_DENIED
                    else DoctorStoreFailure.IO_ERROR
                assertEquals(DoctorStoreResult.Error(expected), fixture.writer.saveExport(prepared.handle, target))
                assertFalse(opened)
                assertTrue(discarded)
                fixture.enqueueTouch()
                assertEquals(1, fixture.writer.pumpOneBatch())
                assertTrue(fixture.writer.saveExport(prepared.handle, RecordingTarget()) is DoctorStoreResult.Ok)
            }
        }
    }

    @Test
    fun `disable discards queued events without deleting history`() {
        withFixture { fixture ->
            fixture.writer.enable()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            val before = fixture.records().size
            fixture.enqueueTouch()

            assertTrue(fixture.writer.disable() is DoctorStoreResult.Ok)

            assertEquals(before, fixture.records().size)
            assertEquals(0L, fixture.ring.depth)
            assertFalse(fixture.producer.isOrdinaryCollecting)
        }
    }

    @Test
    fun `disable preserves retained history and permits a fixed snapshot export`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            val retained = fixture.records()

            fixture.writer.disable().okValue()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            val prepared = fixture.writer.prepareExport().okValue()

            assertFalse(fixture.writer.state().enabled)
            assertFalse(fixture.writer.state().collecting)
            assertEquals(retained.size, prepared.preview.recordCount)
            assertEquals(retained, fixture.records())
            assertTrue(fixture.writer.cancelExport(prepared.handle) is DoctorStoreResult.Ok)
        }
    }

    @Test
    fun `zero batch limit leaves the queue and counters unchanged`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            val before = fixture.metrics.snapshot()

            assertEquals(0, fixture.writer.pumpOneBatch(limit = 0))

            assertEquals(1L, fixture.ring.depth)
            assertEquals(before, fixture.metrics.snapshot())
            assertEquals(1, fixture.writer.pumpOneBatch())
            assertEquals(1L, fixture.metrics.snapshot().written)
        }
    }

    @Test
    fun `prepare without a mark is rejected without owned temporary data`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()

            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST),
                fixture.writer.prepareExport(),
            )
            assertEquals(0L, fixture.writer.state().tempBytes)

            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST),
                fixture.writer.markIssue(-1L),
            )
            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST),
                fixture.writer.prepareExport(),
            )
        }
    }

    @Test
    fun `clear consumes the mark and requires a new issue mark for export`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.writer.markIssue(fixture.clock.wall).okValue()

            fixture.writer.clear().okValue()

            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST),
                fixture.writer.prepareExport(),
            )
            assertEquals(0L, fixture.writer.state().tempBytes)
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            assertTrue(fixture.writer.prepareExport() is DoctorStoreResult.Ok)
        }
    }

    @Test
    fun `validation rejection preserves the owned snapshot for a later save`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            val prepared = fixture.writer.prepareExport().okValue()
            var opened = false
            var discarded = false
            val rejected = object : DoctorExportTarget {
                override fun validate(): Boolean = false
                override fun open(): OutputStream {
                    opened = true
                    return ByteArrayOutputStream()
                }

                override fun discard() {
                    discarded = true
                }
            }

            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST),
                fixture.writer.saveExport(prepared.handle, rejected),
            )
            assertFalse(opened)
            assertTrue(discarded)
            assertTrue(fixture.writer.state().tempBytes > 0L)
            assertTrue(
                fixture.writer.saveExport(prepared.handle, RecordingTarget()) is
                    DoctorStoreResult.Ok,
            )
        }
    }

    @Test
    fun `disable counts only queued ordinary records as dropped`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.producer.setInputState(
                DoctorInputScope.UNKNOWN,
                DoctorEventMode.NONE,
                focus = false,
            )
            assertTrue(fixture.producer.recordLifecycle(
                com.hamanpaul.liukai.core.doctor.DoctorEventCode.FOCUS,
            ))

            fixture.writer.disable().okValue()

            assertEquals(1L, fixture.metrics.snapshot().dropped)
            assertEquals(0L, fixture.ring.depth)
        }
    }

    @Test
    fun `restricted lifecycle persists with fixed correlation and real written bytes`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.producer.setInputState(
                DoctorInputScope.SENSITIVE,
                DoctorEventMode.NONE,
                focus = false,
            )
            assertTrue(fixture.producer.recordLifecycle(
                com.hamanpaul.liukai.core.doctor.DoctorEventCode.FOCUS,
            ))

            assertEquals(1, fixture.writer.pumpOneBatch())

            val record = fixture.records().single()
            assertEquals(DoctorInputScope.SENSITIVE, record.scope)
            assertEquals(0L, record.operationId)
            assertEquals(DoctorCallbackSource.NONE, record.callbackSource)
            assertEquals(0L, fixture.metrics.snapshot().dropped)
            assertTrue(fixture.metrics.snapshot().writtenBytes > 0L)
        }
    }

    @Test
    fun `numeric written bytes match successful persisted bytes`() {
        withFixture { fixture ->
            fixture.writer.enable()
            fixture.enqueueTouch()

            assertEquals(1, fixture.writer.pumpOneBatch())

            val snapshot = fixture.metrics.snapshot()
            assertTrue(snapshot.writtenBytes > 0L)
            assertEquals(fixture.writer.state().rawBytes, snapshot.writtenBytes)
        }
    }

    @Test
    fun `clear discards the old epoch and reopens only on the committed new epoch`() {
        withFixture { fixture ->
            fixture.writer.enable()
            val oldEpoch = fixture.writer.state().epoch
            fixture.enqueueTouch()

            assertTrue(fixture.writer.clear() is DoctorStoreResult.Ok)

            val state = fixture.writer.state()
            assertEquals(oldEpoch + 1L, state.epoch)
            assertTrue(state.collecting)
            assertTrue(fixture.producer.isOrdinaryCollecting)
            assertEquals(0L, fixture.ring.depth)
            assertTrue(fixture.records().isEmpty())
        }
    }

    @Test
    fun `incomplete clear stays closed and retry resumes collection on the same new epoch`() {
        var failDeletion = false
        withFixture(
            faults = object : DoctorIoFaults {
                override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                    if (operation == DoctorIoOperation.DELETE_OLD_RECORD && failDeletion) {
                        DoctorStoreFailure.PERMISSION_DENIED
                    } else {
                        null
                    }
            },
        ) { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            assertEquals(1, fixture.writer.pumpOneBatch())
            val oldEpoch = fixture.writer.state().epoch
            failDeletion = true

            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.CLEAR_INCOMPLETE),
                fixture.writer.clear(),
            )

            val blocked = fixture.writer.state()
            assertEquals(oldEpoch + 1L, blocked.epoch)
            assertTrue(blocked.enabled)
            assertTrue(blocked.pendingClear)
            assertTrue(blocked.interrupted)
            assertFalse(blocked.collecting)
            assertFalse(fixture.producer.isCollecting)
            assertEquals(DoctorStoreFailure.CLEAR_INCOMPLETE, blocked.failure)

            failDeletion = false
            assertEquals(DoctorStoreResult.Ok(Unit), fixture.writer.clear())

            val recovered = fixture.writer.state()
            assertEquals(blocked.epoch, recovered.epoch)
            assertFalse(recovered.pendingClear)
            assertFalse(recovered.interrupted)
            assertTrue(recovered.collecting)
            assertTrue(fixture.producer.isCollecting)
            assertTrue(fixture.records().isEmpty())

            fixture.enqueueTouch()
            assertEquals(1, fixture.writer.pumpOneBatch())
            assertEquals(recovered.epoch, fixture.records().single().epoch)
            assertTrue(fixture.writer.state().collecting)
        }
    }

    @Test
    fun `epoch commit failure keeps the producer closed`() {
        withFixture(
            faults = object : DoctorIoFaults {
                override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                    if (operation == DoctorIoOperation.COMMIT_EPOCH) {
                        DoctorStoreFailure.EPOCH_COMMIT_FAILED
                    } else {
                        null
                    }
            },
        ) { fixture ->
            fixture.writer.enable()

            val result = fixture.writer.clear()

            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_COMMIT_FAILED), result)
            assertFalse(fixture.producer.isOrdinaryCollecting)
            assertTrue(fixture.writer.state().interrupted)
        }
    }

    @Test
    fun `save copies and deletes the same prepared snapshot`() {
        withFixture { fixture ->
            fixture.writer.enable()
            fixture.clock.wall = 1_700_000_000_000L
            fixture.writer.markIssue(1_699_999_900_000L)
            val prepared = fixture.writer.prepareExport().okValue()
            val target = RecordingTarget()

            assertTrue(fixture.writer.saveExport(prepared.handle, target) is DoctorStoreResult.Ok)

            assertEquals(1, target.opened)
            assertTrue(target.bytes.isNotEmpty())
            assertEquals(0L, fixture.writer.state().tempBytes)
            assertTrue(fixture.writer.cancelExport(prepared.handle) is DoctorStoreResult.Error)
        }
    }

    @Test
    fun `completed save remains delivered when preview cleanup fails`() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            val history = fixture.records()
            val epoch = fixture.writer.state().epoch
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            val prepared = fixture.writer.prepareExport().okValue()
            val exportDirectory = fixture.root.resolve("temp")
            val originalPermissions = Files.getPosixFilePermissions(exportDirectory)
            assertTrue(originalPermissions.containsAll(setOf(
                PosixFilePermission.OWNER_READ,
                PosixFilePermission.OWNER_WRITE,
                PosixFilePermission.OWNER_EXECUTE,
            )))
            val noWrite = originalPermissions - setOf(
                PosixFilePermission.OWNER_WRITE,
                PosixFilePermission.GROUP_WRITE,
                PosixFilePermission.OTHERS_WRITE,
            )
            val target = RecordingTarget {
                Files.setPosixFilePermissions(exportDirectory, noWrite)
                check(Files.getPosixFilePermissions(exportDirectory) == noWrite)
            }
            val saveResult = try {
                fixture.writer.saveExport(prepared.handle, target)
            } finally {
                Files.setPosixFilePermissions(exportDirectory, originalPermissions)
            }
            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.PERMISSION_DENIED),
                saveResult,
            )
            assertEquals(1, target.validated)
            assertEquals(1, target.opened)
            assertEquals(1, target.closed)
            assertEquals(0, target.discarded)
            assertTrue(target.bytes.isNotEmpty())
            ZipInputStream(target.bytes.inputStream()).use { zip ->
                val entries = buildList {
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        assertTrue(zip.readBytes().isNotEmpty())
                        add(entry.name)
                        zip.closeEntry()
                    }
                }
                assertEquals(
                    listOf(
                        "doctor/manifest-v1.json",
                        "doctor/mark-v1.json",
                        "doctor/events-v1.jsonl",
                    ),
                    entries,
                )
            }
            assertEquals(history, fixture.records())
            val failedCleanupState = fixture.writer.state()
            assertTrue(failedCleanupState.enabled)
            assertEquals(epoch, failedCleanupState.epoch)
            assertTrue(failedCleanupState.collecting)
            assertFalse(failedCleanupState.interrupted)
            assertFalse(failedCleanupState.pendingClear)
            assertEquals(null, failedCleanupState.failure)
            assertTrue(failedCleanupState.tempBytes > 0L)
            assertEquals(history.last().wallUtcMs, failedCleanupState.newestRetainedMs)

            assertEquals(
                DoctorStoreResult.Ok(Unit),
                fixture.writer.cancelExport(prepared.handle),
            )
            assertEquals(0L, fixture.writer.state().tempBytes)
            assertEquals(history, fixture.records())
            fixture.enqueueTouch()
            assertEquals(1, fixture.writer.pumpOneBatch())
            assertEquals(2, fixture.records().size)
            assertTrue(fixture.writer.state().collecting)
            assertTrue(fixture.producer.isOrdinaryCollecting)
        }
    }

    @Test
    fun `stale snapshot is rejected before destination access after clear`() {
        withFixture { fixture ->
            fixture.writer.enable()
            fixture.writer.markIssue(fixture.clock.wall)
            val prepared = fixture.writer.prepareExport().okValue()
            fixture.writer.clear()
            val target = RecordingTarget()

            val result = fixture.writer.saveExport(prepared.handle, target)

            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_MISMATCH), result)
            assertEquals(0, target.validated)
            assertEquals(0, target.opened)
            assertNotEquals(prepared.handle.epoch, fixture.writer.state().epoch)
            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_MISMATCH),
                fixture.writer.cancelExport(prepared.handle),
            )
        }
    }

    @Test
    fun expiredPreviewRejectsProviderAndCanPrepareAgain() {
        withFixture { fixture ->
            fixture.writer.enable().okValue()
            fixture.enqueueTouch()
            fixture.writer.pumpOneBatch()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            val prepared = fixture.writer.prepareExport().okValue()
            fixture.clock.wall = prepared.handle.expiresAtMs
            val target = RecordingTarget()
            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.EXPORT_EXPIRED),
                fixture.writer.saveExport(prepared.handle, target))
            assertEquals(0, target.validated)
            assertEquals(0, target.opened)
            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.INVALID_SNAPSHOT),
                fixture.writer.saveExport(prepared.handle, target))
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            assertTrue(fixture.writer.prepareExport() is DoctorStoreResult.Ok)
        }
    }

    @Test
    fun immediatePreviewExpiryRemovesOwnedTempAndAllowsRetry() {
        withFixture(limits = DoctorStoreLimits.DEFAULT.copy(exportTtlMs = 1L)) { fixture ->
            fixture.writer.enable().okValue()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            fixture.clock.advanceWallPerRead = 1L

            assertEquals(
                DoctorStoreResult.Error(DoctorStoreFailure.EXPORT_EXPIRED),
                fixture.writer.prepareExport(),
            )
            assertEquals(0L, fixture.writer.state().tempBytes)
            assertTrue(fixture.writer.state().collecting)

            fixture.clock.advanceWallPerRead = 0L
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            val retry = fixture.writer.prepareExport().okValue()
            assertTrue(fixture.writer.cancelExport(retry.handle) is DoctorStoreResult.Ok)
            assertEquals(0L, fixture.writer.state().tempBytes)
        }
    }

    @Test
    fun failedOutputOpenOrWriteDiscardsTargetAndAllowsRetry() {
        for (case in 0..2) {
            withFixture { fixture ->
                fixture.writer.enable().okValue()
                fixture.writer.markIssue(fixture.clock.wall).okValue()
                val prepared = fixture.writer.prepareExport().okValue()
                var discarded = false
                val target = object : DoctorExportTarget {
                    override fun validate() = true
                    override fun open(): OutputStream? = when (case) {
                        0 -> null
                        1 -> throw SecurityException("revoked")
                        else -> object : OutputStream() {
                            override fun write(value: Int) { throw IOException("write failed") }
                        }
                    }
                    override fun discard() { discarded = true }
                }
                val expected = if (case == 1) DoctorStoreFailure.PERMISSION_DENIED else DoctorStoreFailure.IO_ERROR
                assertEquals(DoctorStoreResult.Error(expected), fixture.writer.saveExport(prepared.handle, target))
                assertTrue(discarded)
                assertTrue(fixture.writer.state().collecting)
                assertTrue(fixture.writer.saveExport(prepared.handle, RecordingTarget()) is DoctorStoreResult.Ok)
            }
        }
    }

    @Test
    fun exportCreationFailureLeavesNoPreparedHandleAndRetryWorks() {
        var fail = false
        withFixture(faults = object : DoctorIoFaults {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                if (fail && operation == DoctorIoOperation.CREATE_EXPORT) DoctorStoreFailure.IO_ERROR else null
        }) { fixture ->
            fixture.writer.enable().okValue()
            fixture.writer.markIssue(fixture.clock.wall).okValue()
            fail = true
            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.IO_ERROR), fixture.writer.prepareExport())
            assertEquals(0L, fixture.writer.state().tempBytes)
            assertTrue(fixture.writer.state().collecting)
            fail = false
            val prepared = fixture.writer.prepareExport().okValue()
            assertTrue(fixture.writer.cancelExport(prepared.handle) is DoctorStoreResult.Ok)
            assertEquals(0L, fixture.writer.state().tempBytes)
        }
    }

    @Test
    fun enableControlFailureLeavesCollectionClosedUntilRetry() {
        var fail = false
        withFixture(faults = object : DoctorIoFaults {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? =
                if (fail && operation == DoctorIoOperation.PERSIST_CONTROL) DoctorStoreFailure.PERMISSION_DENIED else null
        }) { fixture ->
            fail = true
            assertEquals(DoctorStoreResult.Error(DoctorStoreFailure.PERMISSION_DENIED), fixture.writer.enable())
            assertFalse(fixture.writer.state().collecting)
            assertFalse(fixture.producer.isCollecting)
            fail = false
            assertTrue(fixture.writer.enable() is DoctorStoreResult.Ok)
            assertTrue(fixture.writer.state().collecting)
            assertTrue(fixture.producer.isCollecting)
        }
    }

    private fun withFixture(
        faults: DoctorIoFaults = DoctorIoFaults.NONE,
        limits: DoctorStoreLimits = DoctorStoreLimits.DEFAULT,
        build: DoctorBuildMetadata = DoctorBuildMetadata("0.0.0-test", "1234567"),
        block: (Fixture) -> Unit,
    ) {
        val root = Files.createTempDirectory("doctor-app-writer")
        try {
            block(Fixture(root, faults, limits, build))
        } finally {
            root.toFile().deleteRecursively()
        }
    }

    private class Fixture(
        val root: Path,
        faults: DoctorIoFaults,
        limits: DoctorStoreLimits = DoctorStoreLimits.DEFAULT,
        val build: DoctorBuildMetadata = DoctorBuildMetadata("0.0.0-test", "1234567"),
    ) {
        val clock = MutableClock()
        val store = DoctorStore.open(
            root = root,
            build = build,
            clock = clock,
            limits = limits,
            ioFaults = faults,
        )
        val ring = DoctorSpscRing(8)
        val metrics = DoctorInstrumentation()
        val producer = DoctorInputProducer(ring, metrics, clock, Thread.currentThread().id) {}
        val writer = DoctorWriterCore(
            store = store,
            ring = ring,
            producer = producer,
            metrics = metrics,
            producerId = 101L,
            sessionId = 102L,
            timezoneOffsetMinutes = 0,
            build = build,
        )

        init {
            producer.setInputState(DoctorInputScope.ORDINARY, DoctorEventMode.LIU, focus = true)
        }

        fun enqueueTouch() {
            val operation = producer.beginOperation()
            assertTrue(
                producer.recordTouch(
                    operation,
                    operation,
                    1,
                    true,
                    DoctorCallbackSource.DISPATCH,
                    true,
                    10L,
                ),
            )
        }

        fun records() = (store.readRecords() as DoctorStoreResult.Ok).value
    }

    private class RecordingTarget(
        private val afterClose: () -> Unit = {},
    ) : DoctorExportTarget {
        val output = ByteArrayOutputStream()
        var validated = 0
        var opened = 0
        var closed = 0
        var discarded = 0
        val bytes: ByteArray get() = output.toByteArray()

        override fun validate(): Boolean {
            validated += 1
            return true
        }

        override fun open(): OutputStream {
            opened += 1
            return object : OutputStream() {
                override fun write(value: Int) {
                    output.write(value)
                }

                override fun write(bytes: ByteArray, offset: Int, length: Int) {
                    output.write(bytes, offset, length)
                }

                override fun flush() {
                    output.flush()
                }

                override fun close() {
                    closed += 1
                    output.flush()
                    output.close()
                    afterClose()
                }
            }
        }

        override fun discard() {
            discarded += 1
        }
    }

    private class MutableClock : DoctorClock {
        var wall = 1_700_000_000_000L
        var monotonic = 1_000_000L
        var advanceWallPerRead = 0L
        override fun wallUtcMs(): Long {
            val value = wall
            wall += advanceWallPerRead
            return value
        }
        override fun monotonicNs(): Long {
            monotonic += 1_000L
            return monotonic
        }
    }

    private fun <T> DoctorStoreResult<T>.okValue(): T = (this as DoctorStoreResult.Ok).value
}
