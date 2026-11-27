package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorBuildMetadata
import com.hamanpaul.liukai.core.doctor.DoctorEventCodec
import com.hamanpaul.liukai.core.doctor.DoctorEventEncode
import com.hamanpaul.liukai.core.doctor.DoctorEventRecord
import com.hamanpaul.liukai.core.doctor.DoctorExportPreview
import com.hamanpaul.liukai.core.doctor.DoctorExportSnapshot
import com.hamanpaul.liukai.core.doctor.DoctorExportWindow
import com.hamanpaul.liukai.core.doctor.DoctorIssueMark
import com.hamanpaul.liukai.core.doctor.DoctorStore
import com.hamanpaul.liukai.core.doctor.DoctorStoreFailure
import com.hamanpaul.liukai.core.doctor.DoctorStoreResult
import java.io.IOException
import java.io.OutputStream
import java.nio.file.Files

internal data class DoctorWriterState(
    val enabled: Boolean,
    val collecting: Boolean,
    val interrupted: Boolean,
    val pendingClear: Boolean,
    val epoch: Long,
    val failure: DoctorStoreFailure?,
    val rawBytes: Long,
    val tempBytes: Long,
    val sequenceGap: Long,
    val ignoredLines: Long,
    val oldestRetainedMs: Long?,
    val newestRetainedMs: Long?,
)

internal data class DoctorExportHandle(
    val token: Long,
    val epoch: Long,
    val expiresAtMs: Long,
)

internal data class DoctorPreparedExport(
    val handle: DoctorExportHandle,
    val preview: DoctorExportPreview,
)

internal interface DoctorExportTarget {
    fun validate(): Boolean
    fun open(): OutputStream?
    fun discard()
}

internal class DoctorWriterCore(
    private val store: DoctorStore,
    private val ring: DoctorSpscRing,
    private val producer: DoctorInputProducer,
    private val metrics: DoctorInstrumentation,
    private val producerId: Long,
    private val sessionId: Long,
    private val timezoneOffsetMinutes: Int,
    private val build: DoctorBuildMetadata,
) {
    private var state = emptyState()
    private var lastMark: DoctorIssueMark? = null
    private var prepared: OwnedExport? = null
    private var nextHandle = 0L
    private var observedFailure: DoctorStoreFailure? = null
    private var collectionFailed = false
    private val copyBuffer = ByteArray(COPY_BUFFER_BYTES)

    init {
        val recovery = store.cancelPendingExports()
        if (recovery is DoctorStoreResult.Error) observedFailure = recovery.reason
        refreshState(refreshRetention = true)
        if (state.collecting) activateProducer()
    }

    fun state(): DoctorWriterState = state.copy()

    fun enable(): DoctorStoreResult<Unit> {
        val result = store.enable()
        if (result is DoctorStoreResult.Ok) {
            collectionFailed = false
            observedFailure = null
        }
        refreshState()
        if (result is DoctorStoreResult.Ok && state.collecting) {
            activateProducer()
        } else {
            producer.close()
        }
        return result
    }

    fun disable(): DoctorStoreResult<Unit> {
        producer.close()
        discardQueued()
        val result = store.disable()
        if (result is DoctorStoreResult.Ok) {
            collectionFailed = false
            observedFailure = null
        }
        refreshState()
        producer.close()
        return result
    }

    fun clear(): DoctorStoreResult<Unit> {
        producer.close()
        invalidatePrepared()
        discardQueued()
        lastMark = null
        val result = store.clear()
        if (result is DoctorStoreResult.Ok) {
            collectionFailed = false
            observedFailure = null
        }
        refreshState(refreshRetention = true)
        if (result is DoctorStoreResult.Ok && state.collecting) {
            activateProducer()
        } else {
            producer.close()
        }
        return result
    }

    fun pumpOneBatch(limit: Int = DEFAULT_BATCH_SIZE): Int {
        var processed = 0
        var failedThisBatch = false
        while (processed < limit && !collectionFailed && ring.poll { slot ->
            val ordinary = slot.scope == com.hamanpaul.liukai.core.doctor.DoctorInputScope.ORDINARY
            val event = slot.toEventRecord(producerId, sessionId)
            val encodedBytes = encodedLineBytes(build, event)
            when (val result = store.append(event)) {
                is DoctorStoreResult.Ok -> {
                    metrics.recordWritten(bytes = encodedBytes)
                }
                is DoctorStoreResult.Error -> {
                    if (ordinary) metrics.recordDrop()
                    observedFailure = result.reason
                    collectionFailed = true
                    failedThisBatch = true
                    producer.close()
                }
            }
        }) {
            processed += 1
        }
        if (failedThisBatch) discardQueued()
        if (processed > 0) refreshState()
        return processed
    }

    private fun encodedLineBytes(
        build: DoctorBuildMetadata,
        event: DoctorEventRecord,
    ): Long =
        when (val encoded = DoctorEventCodec.encodeLine(build, event)) {
            is DoctorEventEncode.Accepted ->
                (encoded.line.toByteArray(Charsets.UTF_8).size + 1).toLong()
            is DoctorEventEncode.Rejected -> 0L
        }

    fun markIssue(
        selectedIssueTimeMs: Long,
        window: DoctorExportWindow = DoctorExportWindow.DEFAULT,
    ): DoctorStoreResult<DoctorIssueMark> {
        val result = store.markIssue(selectedIssueTimeMs, window)
        if (result is DoctorStoreResult.Ok) lastMark = result.value
        return result
    }

    fun prepareExport(): DoctorStoreResult<DoctorPreparedExport> {
        val mark = lastMark ?: return DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST)
        invalidatePrepared()
        val snapshot = when (val result = store.prepareExport(mark)) {
            is DoctorStoreResult.Ok -> result.value
            is DoctorStoreResult.Error -> return result
        }
        val preview = when (val result = store.previewExport(snapshot)) {
            is DoctorStoreResult.Ok -> result.value
            is DoctorStoreResult.Error -> {
                store.cancelExport(snapshot)
                refreshState()
                return result
            }
        }
        nextHandle = if (nextHandle == Long.MAX_VALUE) 1L else nextHandle + 1L
        val handle = DoctorExportHandle(nextHandle, snapshot.epoch, snapshot.expiresAtMs)
        prepared = OwnedExport(handle, snapshot)
        refreshState()
        return DoctorStoreResult.Ok(DoctorPreparedExport(handle, preview))
    }

    fun saveExport(
        handle: DoctorExportHandle,
        target: DoctorExportTarget,
    ): DoctorStoreResult<Unit> {
        val status = store.status()
        if (handle.epoch != status.epoch) {
            return DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_MISMATCH)
        }
        val owned = prepared
        if (owned == null || owned.handle != handle) {
            return DoctorStoreResult.Error(DoctorStoreFailure.INVALID_SNAPSHOT)
        }
        if (store.previewExport(owned.snapshot) !is DoctorStoreResult.Ok) {
            invalidatePrepared()
            return DoctorStoreResult.Error(DoctorStoreFailure.EXPORT_EXPIRED)
        }
        var copied = false
        try {
            if (!target.validate()) {
                return DoctorStoreResult.Error(DoctorStoreFailure.INVALID_REQUEST)
            }
            Files.newInputStream(owned.snapshot.archive).use { input ->
                val output = target.open() ?: throw IOException("SAF target unavailable")
                output.use {
                    var read = input.read(copyBuffer)
                    while (read >= 0) {
                        if (read > 0) it.write(copyBuffer, 0, read)
                        read = input.read(copyBuffer)
                    }
                    it.flush()
                }
            }
            copied = true
            val cleanup = store.cancelExport(owned.snapshot)
            if (cleanup !is DoctorStoreResult.Ok) return cleanup
            prepared = null
            refreshState()
            return DoctorStoreResult.Ok(Unit)
        } catch (failure: SecurityException) {
            observedFailure = DoctorStoreFailure.PERMISSION_DENIED
            return DoctorStoreResult.Error(observedFailure!!)
        } catch (failure: IOException) {
            observedFailure = DoctorStoreFailure.IO_ERROR
            return DoctorStoreResult.Error(observedFailure!!)
        } catch (failure: RuntimeException) {
            // A failed external document provider must not terminate the IME process.
            observedFailure = DoctorStoreFailure.IO_ERROR
            return DoctorStoreResult.Error(observedFailure!!)
        } finally {
            if (!copied) {
                target.discard()
            }
        }
    }

    fun cancelExport(handle: DoctorExportHandle): DoctorStoreResult<Unit> {
        val status = store.status()
        if (handle.epoch != status.epoch) {
            return DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_MISMATCH)
        }
        val owned = prepared
        if (owned == null || owned.handle != handle) {
            return DoctorStoreResult.Error(DoctorStoreFailure.INVALID_SNAPSHOT)
        }
        val result = store.cancelExport(owned.snapshot)
        if (result is DoctorStoreResult.Ok) prepared = null
        refreshState()
        return result
    }

    fun refresh() {
        refreshState(refreshRetention = true)
        if (state.collecting) activateProducer() else producer.close()
    }

    private fun activateProducer() {
        producer.activate(
            epoch = state.epoch,
            producerId = producerId,
            sessionId = sessionId,
            timezoneOffsetMinutes = timezoneOffsetMinutes,
        )
    }

    private fun discardQueued() {
        var ordinary = 0L
        var sequenceGap = false
        ring.discard {
            if (it.scope == com.hamanpaul.liukai.core.doctor.DoctorInputScope.ORDINARY) {
                ordinary += 1L
            }
            sequenceGap = true
        }
        producer.recordDiscarded(ordinary, sequenceGap, resetEpoch = false)
    }

    private fun invalidatePrepared() {
        val owned = prepared ?: return
        store.cancelExport(owned.snapshot)
        prepared = null
        refreshState()
    }

    private fun refreshState(refreshRetention: Boolean = false) {
        var refreshFailure = observedFailure
        if (refreshRetention) {
            when (val records = store.readRecords()) {
                is DoctorStoreResult.Ok -> Unit
                is DoctorStoreResult.Error -> refreshFailure = records.reason
            }
        }
        val status = store.status()
        val failure = refreshFailure ?: status.failure
        val collecting = status.collecting && !collectionFailed
        state = DoctorWriterState(
            enabled = status.enabled,
            collecting = collecting,
            interrupted = collectionFailed || status.pendingClear ||
                status.failure == DoctorStoreFailure.EPOCH_COMMIT_FAILED ||
                (status.enabled && !collecting),
            pendingClear = status.pendingClear,
            epoch = status.epoch,
            failure = failure,
            rawBytes = status.rawBytes,
            tempBytes = status.tempBytes,
            sequenceGap = status.sequenceGap,
            ignoredLines = status.ignoredLines,
            oldestRetainedMs = status.oldestRetainedMs,
            newestRetainedMs = status.newestRetainedMs,
        )
        if (state.collecting) observedFailure = null
    }

    private fun emptyState() =
        DoctorWriterState(
            enabled = false,
            collecting = false,
            interrupted = false,
            pendingClear = false,
            epoch = 0L,
            failure = null,
            rawBytes = 0L,
            tempBytes = 0L,
            sequenceGap = 0L,
            ignoredLines = 0L,
            oldestRetainedMs = null,
            newestRetainedMs = null,
        )

    private data class OwnedExport(
        val handle: DoctorExportHandle,
        val snapshot: DoctorExportSnapshot,
    )

    companion object {
        private const val DEFAULT_BATCH_SIZE = 32
        private const val COPY_BUFFER_BYTES = 8192
    }
}
