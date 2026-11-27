package com.hamanpaul.liukai.core.doctor

import java.io.FilterOutputStream
import java.io.IOException
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.AccessDeniedException
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.OpenOption
import java.nio.file.ReadOnlyFileSystemException
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.CRC32
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

interface DoctorClock {
    fun wallUtcMs(): Long
    fun monotonicNs(): Long
}

data class DoctorStoreLimits(
    val maxRawBytes: Long,
    val maxAgeMs: Long,
    val maxRecordBytes: Long,
    val maxExportBytes: Long,
    val maxRawAndTempBytes: Long,
    val exportTtlMs: Long,
) {
    init {
        require(maxRawBytes > 0L)
        require(maxAgeMs >= 0L)
        require(maxRecordBytes > 0L)
        require(maxExportBytes > 0L)
        require(maxRawAndTempBytes > 0L)
        require(exportTtlMs > 0L)
    }

    companion object {
        val DEFAULT = DoctorStoreLimits(
            maxRawBytes = 32L * 1024 * 1024,
            maxAgeMs = 7L * 24 * 60 * 60 * 1000,
            maxRecordBytes = 1024L * 1024,
            maxExportBytes = 32L * 1024 * 1024,
            maxRawAndTempBytes = 64L * 1024 * 1024,
            exportTtlMs = 15L * 60 * 1000,
        )
    }
}

enum class DoctorIoOperation {
    PERSIST_CONTROL,
    APPEND_RECORD,
    ROTATE_RECORD,
    COMMIT_EPOCH,
    DELETE_OLD_RECORD,
    CREATE_EXPORT,
    SAVE_EXPORT,
}

enum class DoctorStoreFailure {
    DISABLED,
    PRIVACY_REJECTED,
    INVALID_SCHEMA,
    INVALID_REQUEST,
    INVALID_SNAPSHOT,
    TORN_TAIL,
    EPOCH_MISMATCH,
    EPOCH_COMMIT_FAILED,
    CLEAR_INCOMPLETE,
    DISK_FULL,
    PERMISSION_DENIED,
    IO_ERROR,
    RECORD_LIMIT,
    EXPORT_EXISTS,
    EXPORT_EXPIRED,
    EXPORT_LIMIT,
    TOTAL_LIMIT,
}

interface DoctorIoFaults {
    fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure?

    companion object {
        val NONE: DoctorIoFaults = object : DoctorIoFaults {
            override fun failureBefore(operation: DoctorIoOperation): DoctorStoreFailure? = null
        }
    }
}

sealed interface DoctorStoreResult<out T> {
    data class Ok<T>(val value: T) : DoctorStoreResult<T>
    data class Error(val reason: DoctorStoreFailure) : DoctorStoreResult<Nothing>
}

data class DoctorStoreState(
    val enabled: Boolean,
    val collecting: Boolean,
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

data class DoctorExportWindow(
    val beforeMs: Long,
    val afterMs: Long,
) {
    companion object {
        val DEFAULT = DoctorExportWindow(beforeMs = 5L * 60 * 1000, afterMs = 30_000L)
    }
}

data class DoctorIssueMark(
    val epoch: Long,
    val actionTimeMs: Long,
    val selectedIssueTimeMs: Long,
    val window: DoctorExportWindow,
)

data class DoctorExportSnapshot(
    val epoch: Long,
    val archive: Path,
    val createdAtMs: Long,
    val expiresAtMs: Long,
)

data class DoctorExportPreview(
    val epoch: Long,
    val recordCount: Int,
    val windowStartMs: Long,
    val windowEndMs: Long,
    val oldestRetainedMs: Long?,
    val newestRetainedMs: Long?,
    val ignoredLines: Long,
    val sequenceGap: Long,
)

class DoctorStore private constructor(
    private val root: Path,
    private val build: DoctorBuildMetadata,
    private val clock: DoctorClock,
    private val limits: DoctorStoreLimits,
    private val ioFaults: DoctorIoFaults,
) {
    private var control = DoctorControlState()
    private var runtimeFailure: DoctorStoreFailure? = null
    private var diagnosticFailure: DoctorStoreFailure? = null
    private var ignoredLines = 0L
    private var rawBytes = 0L
    private var tempBytes = 0L
    private val damagedRecords = mutableSetOf<Path>()
    private val recordSummaries = linkedMapOf<Path, DoctorRecordSummary>()

    fun status(): DoctorStoreState {
        try {
            refreshFileSizes()
        } catch (failure: Exception) {
            runtimeFailure = failure.doctorStoreFailure()
        }
        val failure = effectiveFailure()
        val epochBlocked = control.pendingClear || failure == DoctorStoreFailure.EPOCH_COMMIT_FAILED
        return DoctorStoreState(
            enabled = control.enabled,
            collecting = control.enabled &&
                !control.pendingClear &&
                !interruptsCollection(failure),
            pendingClear = control.pendingClear,
            epoch = control.epoch,
            failure = failure,
            rawBytes = rawBytes,
            tempBytes = tempBytes,
            sequenceGap = control.sequenceGap,
            ignoredLines = ignoredLines,
            oldestRetainedMs = if (epochBlocked) null else
                recordSummaries.values.mapNotNull { it.oldestRetainedMs }.minOrNull(),
            newestRetainedMs = if (epochBlocked) null else
                recordSummaries.values.mapNotNull { it.newestRetainedMs }.maxOrNull(),
        )
    }

    fun enable(): DoctorStoreResult<Unit> = controlResult {
        requireAccessibleEpoch(control.epoch)
        val candidate = control.copy(enabled = true, failure = null)
        persistControl(candidate, DoctorIoOperation.PERSIST_CONTROL)
        control = candidate
        runtimeFailure = null
    }

    fun disable(): DoctorStoreResult<Unit> = controlResult {
        requireAccessibleEpoch(control.epoch)
        val candidate = control.copy(enabled = false, failure = null)
        persistControl(candidate, DoctorIoOperation.PERSIST_CONTROL)
        control = candidate
        runtimeFailure = null
    }

    fun append(event: DoctorEventRecord): DoctorStoreResult<Unit> {
        if (!control.enabled) return DoctorStoreResult.Error(DoctorStoreFailure.DISABLED)
        if (control.pendingClear) {
            return DoctorStoreResult.Error(DoctorStoreFailure.CLEAR_INCOMPLETE)
        }
        if (event.epoch != control.epoch) {
            return DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_MISMATCH)
        }
        if (control.failure == DoctorStoreFailure.EPOCH_COMMIT_FAILED ||
            runtimeFailure == DoctorStoreFailure.EPOCH_COMMIT_FAILED) {
            return DoctorStoreResult.Error(DoctorStoreFailure.EPOCH_COMMIT_FAILED)
        }
        val line = when (val encoded = DoctorEventCodec.encodeLine(build, event)) {
            is DoctorEventEncode.Rejected -> {
                val reason = if (isRestrictedFailure(encoded.reason)) {
                    DoctorStoreFailure.PRIVACY_REJECTED
                } else {
                    DoctorStoreFailure.INVALID_SCHEMA
                }
                return DoctorStoreResult.Error(reason)
            }
            is DoctorEventEncode.Accepted -> encoded.line
        }

        return try {
            cleanupRetention()
            appendCanonicalLine(line, event)
            val queuedGap = saturatingAdd(
                event.dropCount ?: 0L,
                if (event.gapFromSequence != null) 1L else 0L,
            )
            if (queuedGap > 0L) persistSequenceGap(queuedGap)
            val staleIoFailure = interruptsCollection(control.failure) || interruptsCollection(runtimeFailure)
            if (staleIoFailure) {
                persistRecoveredControl()
            } else {
                runtimeFailure = null
            }
            refreshFileSizes()
            DoctorStoreResult.Ok(Unit)
        } catch (failure: Exception) {
            val reason = failure.doctorStoreFailure()
            rememberOperationFailure(reason)
            DoctorStoreResult.Error(reason)
        }
    }

    fun readRecords(epoch: Long = status().epoch): DoctorStoreResult<List<DoctorEventRecord>> =
        storeResult {
            requireAccessibleEpoch(epoch)
            cleanupExpiredExports()
            cleanupRetention()
            scanRecords().events.map { it.event }
        }

    fun clear(): DoctorStoreResult<Unit> {
        if (control.pendingClear) return finishClearResult()
        val enabledAfterClear = control.enabled
        return try {
            // Persist the intent before committing the new epoch: if both the
            // commit and its error write fail, reopening must still fail closed.
            val stopped = control.copy(enabled = false, failure = DoctorStoreFailure.EPOCH_COMMIT_FAILED)
            try {
                persistControl(stopped, DoctorIoOperation.PERSIST_CONTROL)
            } catch (failure: Exception) {
                // The intent did not commit: retain consent for an explicit
                // retry, while this writer still blocks old data and input.
                control = control.copy(failure = DoctorStoreFailure.EPOCH_COMMIT_FAILED)
                runtimeFailure = DoctorStoreFailure.EPOCH_COMMIT_FAILED
                return DoctorStoreResult.Error(failure.doctorStoreFailure())
            }
            control = stopped
            runtimeFailure = null

            if (control.epoch == Long.MAX_VALUE) {
                throw epochCommitFailure(DoctorStoreFailure.EPOCH_COMMIT_FAILED)
            }
            val committed = control.copy(
                enabled = enabledAfterClear,
                epoch = control.epoch + 1,
                pendingClear = true,
                failure = null,
                sequenceGap = 0L,
            )
            try {
                persistControl(committed, DoctorIoOperation.COMMIT_EPOCH)
            } catch (failure: Exception) {
                failure.doctorStoreFailure()
                throw epochCommitFailure(DoctorStoreFailure.EPOCH_COMMIT_FAILED)
            }
            control = committed
            diagnosticFailure = null
            ignoredLines = 0L
            damagedRecords.clear()

            try {
                deleteOldDoctorData()
                val completed = control.copy(pendingClear = false, failure = null)
                persistControl(completed, DoctorIoOperation.PERSIST_CONTROL)
                control = completed
                runtimeFailure = null
                refreshFileSizes()
                DoctorStoreResult.Ok(Unit)
            } catch (failure: Exception) {
                persistPendingClearFailure(failure.doctorStoreFailure())
                DoctorStoreResult.Error(DoctorStoreFailure.CLEAR_INCOMPLETE)
            }
        } catch (failure: Exception) {
            val reason = failure.doctorStoreFailure()
            runtimeFailure = reason
            DoctorStoreResult.Error(reason)
        }
    }

    fun markIssue(
        selectedIssueTimeMs: Long,
        window: DoctorExportWindow = DoctorExportWindow.DEFAULT,
    ): DoctorStoreResult<DoctorIssueMark> = storeResult {
        requireAccessibleEpoch(control.epoch)
        if (
            selectedIssueTimeMs < 0L ||
            window.beforeMs < 0L ||
            window.afterMs < 0L
        ) {
            fail(DoctorStoreFailure.INVALID_REQUEST)
        }
        val actionTime = clock.wallUtcMs()
        if (actionTime < 0L) fail(DoctorStoreFailure.INVALID_REQUEST)
        DoctorIssueMark(
            epoch = control.epoch,
            actionTimeMs = actionTime,
            selectedIssueTimeMs = selectedIssueTimeMs,
            window = window,
        )
    }

    fun prepareExport(mark: DoctorIssueMark): DoctorStoreResult<DoctorExportSnapshot> {
        return try {
            requireAccessibleEpoch(mark.epoch)
            if (
                mark.actionTimeMs < 0L ||
                mark.selectedIssueTimeMs < 0L ||
                mark.window.beforeMs < 0L ||
                mark.window.afterMs < 0L
            ) {
                fail(DoctorStoreFailure.INVALID_REQUEST)
            }
            cleanupExpiredExports()
            if (allowlistedExportFiles().isNotEmpty()) {
                fail(DoctorStoreFailure.EXPORT_EXISTS)
            }
            cleanupRetention()
            injectedFailure(DoctorIoOperation.CREATE_EXPORT)
            val scan = scanRecords()
            val windowStart = saturatingSubtract(mark.selectedIssueTimeMs, mark.window.beforeMs)
            val windowEnd = saturatingAdd(mark.selectedIssueTimeMs, mark.window.afterMs)
            val selected = scan.events.filter {
                it.event.wallUtcMs >= windowStart &&
                    it.event.wallUtcMs <= windowEnd
            }
            refreshFileSizes()
            val remainingTotal = limits.maxRawAndTempBytes - rawBytes
            if (remainingTotal <= 0L) fail(DoctorStoreFailure.TOTAL_LIMIT)
            val (snapshotLimit, limitFailure) = if (
                limits.maxExportBytes <= remainingTotal
            ) {
                limits.maxExportBytes to DoctorStoreFailure.EXPORT_LIMIT
            } else {
                remainingTotal to DoctorStoreFailure.TOTAL_LIMIT
            }

            Files.createDirectories(exportDirectory())
            val createdAt = clock.wallUtcMs()
            if (createdAt < 0L) fail(DoctorStoreFailure.INVALID_REQUEST)
            val expiresAt = saturatingAdd(createdAt, limits.exportTtlMs)
            val archive = exportDirectory().resolve(
                "doctor-export-${UUID.randomUUID()}.zip",
            )
            var completed = false
            try {
                writeSnapshotZip(
                    archive = archive,
                    mark = mark,
                    envelopes = selected,
                    createdAtMs = createdAt,
                    expiresAtMs = expiresAt,
                    byteLimit = snapshotLimit,
                    limitFailure = limitFailure,
                )
                completed = true
            } finally {
                if (!completed) Files.deleteIfExists(archive)
            }
            forceDirectory(exportDirectory())
            refreshFileSizes()
            DoctorStoreResult.Ok(
                DoctorExportSnapshot(
                    epoch = control.epoch,
                    archive = archive,
                    createdAtMs = createdAt,
                    expiresAtMs = expiresAt,
                ),
            )
        } catch (failure: DoctorStoreCapacityException) {
            Files.deleteIfExists(failure.path)
            refreshAfterFailure()
            DoctorStoreResult.Error(failure.reason)
        } catch (failure: Exception) {
            val reason = failure.doctorStoreFailure()
            refreshAfterFailure()
            DoctorStoreResult.Error(reason)
        }
    }

    fun previewExport(snapshot: DoctorExportSnapshot): DoctorStoreResult<DoctorExportPreview> =
        storeResult {
            readPreview(snapshot)
        }

    fun saveExport(
        snapshot: DoctorExportSnapshot,
        destination: Path,
    ): DoctorStoreResult<Unit> {
        return try {
            readPreview(snapshot)
            val normalizedDestination = destination.toAbsolutePath().normalize()
            if (normalizedDestination == snapshot.archive.toAbsolutePath().normalize() ||
                (Files.exists(normalizedDestination, LinkOption.NOFOLLOW_LINKS) &&
                    Files.isSameFile(snapshot.archive, normalizedDestination))) {
                fail(DoctorStoreFailure.INVALID_REQUEST)
            }
            injectedFailure(DoctorIoOperation.SAVE_EXPORT)
            saveSnapshotAtomically(snapshot.archive, normalizedDestination)
            Files.delete(snapshot.archive)
            forceDirectory(exportDirectory())
            refreshFileSizes()
            DoctorStoreResult.Ok(Unit)
        } catch (failure: Exception) {
            DoctorStoreResult.Error(failure.doctorStoreFailure())
        }
    }

    fun cancelExport(snapshot: DoctorExportSnapshot): DoctorStoreResult<Unit> =
        storeResult {
            requireCurrentEpoch(snapshot.epoch)
            requireSnapshotLocation(snapshot.archive)
            if (Files.exists(snapshot.archive)) Files.delete(snapshot.archive)
            forceDirectory(exportDirectory())
            refreshFileSizes()
        }

    /** A new process writer has no live UI handles for previews left by its predecessor. */
    fun cancelPendingExports(): DoctorStoreResult<Unit> = storeResult {
        val owned = allowlistedExportFiles()
        for (path in owned) {
            injectedFailure(DoctorIoOperation.DELETE_OLD_RECORD)
            Files.delete(path)
        }
        if (owned.isNotEmpty()) forceDirectory(exportDirectory())
        refreshFileSizes()
    }

    private fun initialize() {
        try {
            ensureRootDirectory()
            loadControlState()
            if (control.pendingClear) {
                finishPendingClear()
            } else {
                cleanupOwnedStaging()
                rebuildRecordSummaries()
            }
            cleanupExpiredExports()
            cleanupRetention()
            refreshFileSizes()
        } catch (failure: Exception) {
            runtimeFailure = failure.doctorStoreFailure()
        }
    }

    private fun finishClearResult(): DoctorStoreResult<Unit> =
        storeResult {
            finishPendingClear()
            refreshFileSizes()
        }

    private fun finishPendingClear() {
        deleteOldDoctorData()
        val completed = control.copy(pendingClear = false, failure = null)
        persistControl(completed, DoctorIoOperation.PERSIST_CONTROL)
        control = completed
        diagnosticFailure = null
        runtimeFailure = null
        ignoredLines = 0L
        damagedRecords.clear()
    }

    private fun epochCommitFailure(reason: DoctorStoreFailure): DoctorStoreFixedException {
        val stopped = control.copy(
            enabled = false,
            pendingClear = false,
            failure = reason,
        )
        try {
            persistControl(stopped, DoctorIoOperation.PERSIST_CONTROL)
            control = stopped
            runtimeFailure = null
        } catch (failure: Exception) {
            failure.doctorStoreFailure()
            control = stopped
            runtimeFailure = reason
        }
        return DoctorStoreFixedException(reason)
    }

    private fun persistPendingClearFailure(reason: DoctorStoreFailure) {
        val pending = control.copy(pendingClear = true, failure = reason)
        control = pending
        runtimeFailure = reason
        try {
            persistControl(pending, DoctorIoOperation.PERSIST_CONTROL)
            runtimeFailure = reason
        } catch (failure: Exception) {
            failure.doctorStoreFailure()
            runtimeFailure = reason
        }
    }

    private fun persistRecoveredControl() {
        val candidate = control.copy(failure = null)
        persistControl(candidate, DoctorIoOperation.PERSIST_CONTROL)
        control = candidate
        runtimeFailure = null
    }

    private fun rememberOperationFailure(reason: DoctorStoreFailure) {
        runtimeFailure = reason
    }

    private fun persistSequenceGap(amount: Long) {
        val candidate = control.copy(
            sequenceGap = saturatingAdd(control.sequenceGap, amount),
        )
        persistControl(candidate, DoctorIoOperation.PERSIST_CONTROL)
        control = candidate
    }

    private fun appendCanonicalLine(line: String, event: DoctorEventRecord) {
        val bytes = "$line\n".toByteArray(Charsets.UTF_8)
        if (bytes.size.toLong() > limits.maxRecordBytes) {
            fail(DoctorStoreFailure.RECORD_LIMIT)
        }
        if (bytes.size.toLong() > limits.maxRawBytes) {
            fail(DoctorStoreFailure.RECORD_LIMIT)
        }

        val directory = recordsDirectory()
        Files.createDirectories(directory)
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) fail(DoctorStoreFailure.IO_ERROR)
        val existing = recordSummaries.keys.lastOrNull()
        val existingSummary = existing?.let(recordSummaries::getValue)
        val shouldRotate =
            existing == null ||
            existing in damagedRecords ||
            existingSummary!!.bytes + bytes.size.toLong() > limits.maxRecordBytes ||
            rawBytes + bytes.size.toLong() > limits.maxRawBytes
        val target = if (shouldRotate) {
            if (existing != null) injectedFailure(DoctorIoOperation.ROTATE_RECORD)
            nextRecordPath(recordSummaries.keys.toList())
        } else {
            existing!!
        }

        injectedFailure(DoctorIoOperation.APPEND_RECORD)
        val options = if (shouldRotate) {
            arrayOf<OpenOption>(StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE,
                StandardOpenOption.APPEND, LinkOption.NOFOLLOW_LINKS)
        } else {
            arrayOf<OpenOption>(StandardOpenOption.WRITE, StandardOpenOption.APPEND, LinkOption.NOFOLLOW_LINKS)
        }
        FileChannel.open(target, *options).use { channel ->
            val buffer = ByteBuffer.wrap(bytes)
            while (buffer.hasRemaining()) channel.write(buffer)
            channel.force(true)
        }
        forceDirectory(directory)
        rememberAppendedEvent(
            path = target,
            summary = if (target == existing) existingSummary else null,
            event = event,
            bytes = bytes.size.toLong(),
        )
        damagedRecords.remove(target)
        cleanupRetention()
    }

    private fun cleanupRetention() {
        val directory = recordsDirectory()
        if (!Files.exists(directory)) {
            recordSummaries.clear()
            damagedRecords.clear()
            rawBytes = 0L
            return
        }
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
            fail(DoctorStoreFailure.IO_ERROR)
        }

        var retainedGap = 0L
        var scanFailure: DoctorStoreFailure? = null
        var observedIgnored = 0L
        val cutoff = saturatingSubtract(clock.wallUtcMs(), limits.maxAgeMs)
        val now = clock.wallUtcMs()
        for ((path, initialSummary) in recordSummaries.toList()) {
            var summary = initialSummary
            if (summary.failure != null) {
                scanFailure = scanFailure ?: summary.failure.toStoreFailure()
                observedIgnored += summary.ignoredLines
            }
            if (summary.failure == DoctorEventFailure.TORN_LINE) {
                repairTornTail(path)
                summary = summary.copy(
                    bytes = Files.size(path),
                    failure = null,
                    ignoredLines = 0L,
                )
                recordSummaries[path] = summary
            }

            val expiresWithoutScan =
                summary.eventCount > 0L &&
                    (
                        summary.currentEpochEventCount == 0L ||
                            summary.newestExpiryMs!! < now
                    )
            if (expiresWithoutScan) {
                retainedGap = saturatingAdd(retainedGap, summary.eventCount)
                deleteRecord(path)
                continue
            }
            if (
                summary.hasForeignEpoch ||
                (summary.oldestExpiryMs != null && summary.oldestExpiryMs < now)
            ) {
                val scan = scanRecordFile(path)
                if (scan.failure != null) {
                    scanFailure = scanFailure ?: scan.failure.toStoreFailure()
                    // The summary and rescan describe the same file. Count a
                    // malformed line once even when retention rescans it.
                    observedIgnored += maxOf(initialSummary.ignoredLines, scan.ignoredLines) -
                        initialSummary.ignoredLines
                }
                val retained = scan.envelopes.filter {
                    it.event.epoch == control.epoch && it.event.wallUtcMs >= cutoff
                }
                val removed = scan.envelopes.size - retained.size
                if (removed > 0) {
                    retainedGap = saturatingAdd(retainedGap, removed.toLong())
                    if (retained.isEmpty()) {
                        deleteRecord(path)
                    } else {
                        atomicRewriteRecord(path, retained)
                    }
                } else {
                    rememberRecordScan(path, scan)
                }
            }
        }

        observeDiagnostics(scanFailure, observedIgnored)
        rawBytes = recordSummaries.values.sumOf { it.bytes }
        while (rawBytes > limits.maxRawBytes) {
            val oldest = recordSummaries.keys.first()
            val removedEvents = recordSummaries.getValue(oldest).eventCount.coerceAtLeast(1L)
            deleteRecord(oldest)
            retainedGap = saturatingAdd(retainedGap, removedEvents)
        }
        if (retainedGap > 0L) persistSequenceGap(retainedGap)
        refreshFileSizes()
    }

    private fun rebuildRecordSummaries() {
        recordSummaries.clear()
        damagedRecords.clear()
        var scanFailure: DoctorStoreFailure? = null
        var observedIgnored = 0L
        for (path in recordFiles()) {
            val scan = scanRecordFile(path)
            rememberRecordScan(path, scan)
            if (scan.failure != null) {
                scanFailure = scanFailure ?: scan.failure.toStoreFailure()
                observedIgnored += scan.ignoredLines
            }
        }
        observeDiagnostics(scanFailure, observedIgnored)
        rawBytes = recordSummaries.values.sumOf { it.bytes }
    }

    private fun scanRecords(): DoctorRecordScan {
        val envelopes = mutableListOf<DoctorEventEnvelope>()
        var failure: DoctorStoreFailure? = null
        var ignored = 0L
        val files = recordFiles()
        recordSummaries.keys.filterNot(files::contains).forEach { path ->
            recordSummaries.remove(path)
            damagedRecords.remove(path)
        }
        for (path in files) {
            val scan = scanRecordFile(path)
            rememberRecordScan(path, scan)
            envelopes += scan.envelopes.filter { it.event.epoch == control.epoch }
            if (scan.failure != null) {
                failure = failure ?: scan.failure.toStoreFailure()
            }
            ignored += scan.ignoredLines
        }
        observeDiagnostics(failure, ignored)
        rawBytes = recordSummaries.values.sumOf { it.bytes }
        return DoctorRecordScan(envelopes, failure, ignored)
    }

    private fun scanRecordFile(path: Path): DoctorFileScan {
        val text = Files.newBufferedReader(path, Charsets.UTF_8).use { it.readText() }
        val decoded = DoctorEventCodec.decodeJsonl(text)
        val failure = decoded.failure
        if (failure != null || (text.isNotEmpty() && !text.endsWith('\n'))) damagedRecords.add(path)
        else damagedRecords.remove(path)
        return DoctorFileScan(
            path = path,
            envelopes = decoded.events,
            failure = failure,
            ignoredLines = if (failure == null) 0L else 1L,
        )
    }

    private fun rememberRecordScan(path: Path, scan: DoctorFileScan) {
        val current = scan.envelopes.filter { it.event.epoch == control.epoch }
        val expiries = current.map { saturatingAdd(it.event.wallUtcMs, limits.maxAgeMs) }
        recordSummaries[path] = DoctorRecordSummary(
            bytes = Files.size(path),
            eventCount = scan.envelopes.size.toLong(),
            currentEpochEventCount = current.size.toLong(),
            hasForeignEpoch = current.size != scan.envelopes.size,
            oldestExpiryMs = expiries.minOrNull(),
            newestExpiryMs = expiries.maxOrNull(),
            failure = scan.failure,
            ignoredLines = scan.ignoredLines,
            oldestRetainedMs = current.minOfOrNull { it.event.wallUtcMs },
            newestRetainedMs = current.maxOfOrNull { it.event.wallUtcMs },
        )
    }

    private fun rememberAppendedEvent(
        path: Path,
        summary: DoctorRecordSummary?,
        event: DoctorEventRecord,
        bytes: Long,
    ) {
        val expiry = saturatingAdd(event.wallUtcMs, limits.maxAgeMs)
        val updated = if (summary == null) {
            DoctorRecordSummary(
                bytes = bytes,
                eventCount = 1L,
                currentEpochEventCount = 1L,
                hasForeignEpoch = false,
                oldestExpiryMs = expiry,
                newestExpiryMs = expiry,
                failure = null,
                ignoredLines = 0L,
                oldestRetainedMs = event.wallUtcMs,
                newestRetainedMs = event.wallUtcMs,
            )
        } else {
            summary.copy(
                bytes = saturatingAdd(summary.bytes, bytes),
                eventCount = saturatingAdd(summary.eventCount, 1L),
                currentEpochEventCount = saturatingAdd(summary.currentEpochEventCount, 1L),
                oldestExpiryMs = minOf(summary.oldestExpiryMs ?: expiry, expiry),
                newestExpiryMs = maxOf(summary.newestExpiryMs ?: expiry, expiry),
                oldestRetainedMs = minOf(summary.oldestRetainedMs ?: event.wallUtcMs, event.wallUtcMs),
                newestRetainedMs = maxOf(summary.newestRetainedMs ?: event.wallUtcMs, event.wallUtcMs),
            )
        }
        recordSummaries[path] = updated
        rawBytes = saturatingAdd(rawBytes, bytes)
    }

    private fun atomicRewriteRecord(path: Path, events: List<DoctorEventEnvelope>) {
        val bytes = events.joinToString(
            separator = "\n",
            // The only caller deletes empty segments instead of rewriting.
            postfix = "\n",
        ) { canonicalLine(it) }.toByteArray(Charsets.UTF_8)
        atomicWrite(path, bytes)
        damagedRecords.remove(path)
        rememberRecordScan(
            path,
            DoctorFileScan(path, events, failure = null, ignoredLines = 0L),
        )
    }

    private fun repairTornTail(path: Path) {
        val bytes = Files.readAllBytes(path)
        var end = bytes.size
        while (end > 0 && bytes[end - 1] != '\n'.code.toByte()) end -= 1
        FileChannel.open(path, StandardOpenOption.WRITE).use { channel ->
            channel.truncate(end.toLong())
            channel.force(true)
        }
        damagedRecords.remove(path)
    }

    private fun deleteRecord(path: Path) {
        if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) {
            fail(DoctorStoreFailure.IO_ERROR)
        }
        Files.delete(path)
        recordSummaries.remove(path)
        damagedRecords.remove(path)
        rawBytes = recordSummaries.values.sumOf { it.bytes }
        forceDirectory(recordsDirectory())
    }

    private fun deleteOldDoctorData() {
        cleanupOwnedStaging()
        for (path in allowlistedExportFiles()) {
            injectedFailure(DoctorIoOperation.DELETE_OLD_RECORD)
            Files.delete(path)
            forceDirectory(exportDirectory())
        }
        for (path in recordFiles()) {
            injectedFailure(DoctorIoOperation.DELETE_OLD_RECORD)
            deleteRecord(path)
        }
        recordSummaries.clear()
        damagedRecords.clear()
        rawBytes = 0L
        tempBytes = 0L
    }

    private fun cleanupOwnedStaging() {
        for (path in ownedStagingFiles()) {
            injectedFailure(DoctorIoOperation.DELETE_OLD_RECORD)
            if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) continue
            Files.delete(path)
            forceDirectory(path.parent)
        }
    }

    private fun cleanupExpiredExports() {
        val files = allowlistedExportFiles().sortedBy { it.fileName.toString() }
        val now = clock.wallUtcMs()
        var active: Path? = null
        for (path in files) {
            val manifest = readManifestIfValid(path)
            if (
                manifest == null ||
                manifest.epoch != control.epoch ||
                now < manifest.createdAtMs || manifest.expiresAtMs <= now ||
                active != null
            ) {
                Files.delete(path)
                forceDirectory(exportDirectory())
            } else {
                active = path
            }
        }
        refreshFileSizes()
    }

    private fun writeSnapshotZip(
        archive: Path,
        mark: DoctorIssueMark,
        envelopes: List<DoctorEventEnvelope>,
        createdAtMs: Long,
        expiresAtMs: Long,
        byteLimit: Long,
        limitFailure: DoctorStoreFailure,
    ) {
        val oldest = envelopes.minOfOrNull { it.event.wallUtcMs }
        val newest = envelopes.maxOfOrNull { it.event.wallUtcMs }
        val events = envelopes.joinToString(
            separator = "\n",
            postfix = if (envelopes.isEmpty()) "" else "\n",
        ) { canonicalLine(it) }
        val manifest = buildString {
            append("{\"schemaVersion\":1")
            append(",\"version\":\"").append(build.version).append('"')
            append(",\"sourceCommit\":\"").append(build.sourceCommit).append('"')
            append(",\"epoch\":").append(control.epoch)
            append(",\"createdAtMs\":").append(createdAtMs)
            append(",\"expiresAtMs\":").append(expiresAtMs)
            append(",\"recordCount\":").append(envelopes.size)
            append(",\"oldestRetainedMs\":")
            if (oldest == null) append("0") else append(oldest)
            append(",\"newestRetainedMs\":")
            if (newest == null) append("0") else append(newest)
            append(",\"ignoredLines\":").append(ignoredLines)
            append(",\"sequenceGap\":").append(control.sequenceGap)
            append('}')
        }
        val markJson = buildString {
            append("{\"schemaVersion\":1")
            append(",\"epoch\":").append(mark.epoch)
            append(",\"actionTimeMs\":").append(mark.actionTimeMs)
            append(",\"selectedIssueTimeMs\":").append(mark.selectedIssueTimeMs)
            append(",\"beforeMs\":").append(mark.window.beforeMs)
            append(",\"afterMs\":").append(mark.window.afterMs)
            append('}')
        }

        val output = Files.newOutputStream(
            archive,
            StandardOpenOption.CREATE_NEW,
            StandardOpenOption.WRITE,
        )
        val bounded = DoctorBoundedOutputStream(
            output = output,
            limit = byteLimit,
            failure = limitFailure,
            path = archive,
        )
        try {
            ZipOutputStream(bounded).use { zip ->
                zip.putFixedEntry("doctor/manifest-v1.json", manifest)
                zip.putFixedEntry("doctor/mark-v1.json", markJson)
                zip.putFixedEntry("doctor/events-v1.jsonl", events)
            }
        } catch (failure: DoctorStoreCapacityException) {
            throw failure
        }
        FileChannel.open(archive, StandardOpenOption.WRITE).use { it.force(true) }
    }

    private fun readPreview(snapshot: DoctorExportSnapshot): DoctorExportPreview {
        requireAccessibleEpoch(snapshot.epoch)
        requireSnapshotLocation(snapshot.archive)
        val now = clock.wallUtcMs()
        if (now < snapshot.createdAtMs || snapshot.expiresAtMs <= now) {
            Files.deleteIfExists(snapshot.archive)
            refreshFileSizes()
            fail(DoctorStoreFailure.EXPORT_EXPIRED)
        }
        if (!Files.isRegularFile(snapshot.archive, LinkOption.NOFOLLOW_LINKS)) {
            fail(DoctorStoreFailure.INVALID_SNAPSHOT)
        }
        try {
            return ZipFile(snapshot.archive.toFile()).use { zip ->
                val names = zip.entries().asSequence().map { it.name }.toList()
                if (names.size != ZIP_ENTRIES.size || names.toSet() != ZIP_ENTRIES) {
                    fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                }
                val eventsText = zip.readUtf8("doctor/events-v1.jsonl")
                    ?: fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                val markText = zip.readUtf8("doctor/mark-v1.json")
                    ?: fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                val decoded = DoctorEventCodec.decodeJsonl(eventsText)
                if (decoded.failure != null) fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                if (decoded.events.any { it.event.epoch != snapshot.epoch }) {
                    fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                }
                val markFields = parseDoctorArchiveObject(markText, MARK_FIELDS)
                    ?: fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                val markEpoch = markFields.longField("epoch")
                val action = markFields.longField("actionTimeMs")
                val before = markFields.longField("beforeMs")
                val after = markFields.longField("afterMs")
                val selected = markFields.longField("selectedIssueTimeMs")
                if (
                    markFields.longField("schemaVersion") != 1L || markEpoch != snapshot.epoch ||
                    action == null ||
                    before == null ||
                    after == null ||
                    selected == null ||
                    action < 0L ||
                    before < 0L ||
                    after < 0L || selected < 0L
                ) {
                    fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                }
                val manifest = readManifestFromText(
                    zip.readUtf8("doctor/manifest-v1.json")
                        ?: fail(DoctorStoreFailure.INVALID_SNAPSHOT),
                ) ?: fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                val oldest: Long? = decoded.events.minOfOrNull { it.event.wallUtcMs }
                val newest: Long? = decoded.events.maxOfOrNull { it.event.wallUtcMs }
                val manifestOldest: Long = oldest ?: 0L
                val manifestNewest: Long = newest ?: 0L
                if (
                    manifest.epoch != snapshot.epoch ||
                    manifest.createdAtMs != snapshot.createdAtMs ||
                    manifest.expiresAtMs != snapshot.expiresAtMs ||
                    manifest.recordCount != decoded.events.size ||
                    manifest.oldestRetainedMs != manifestOldest ||
                    manifest.newestRetainedMs != manifestNewest
                ) {
                    fail(DoctorStoreFailure.INVALID_SNAPSHOT)
                }
                DoctorExportPreview(
                    epoch = snapshot.epoch,
                    recordCount = decoded.events.size,
                    windowStartMs = saturatingSubtract(selected, before),
                    windowEndMs = saturatingAdd(selected, after),
                    oldestRetainedMs = oldest,
                    newestRetainedMs = newest,
                    ignoredLines = manifest.ignoredLines,
                    sequenceGap = manifest.sequenceGap,
                )
            }
        } catch (failure: IOException) {
            fail(DoctorStoreFailure.INVALID_SNAPSHOT)
        }
    }

    // Envelopes on these private rewrite paths were already accepted by the
    // codec. Keep their original build metadata when producing canonical bytes.
    private fun canonicalLine(envelope: DoctorEventEnvelope): String =
        (DoctorEventCodec.encodeLine(envelope.build, envelope.event) as DoctorEventEncode.Accepted).line

    private fun saveSnapshotAtomically(source: Path, destination: Path) {
        val parent = destination.parent ?: fail(DoctorStoreFailure.INVALID_REQUEST)
        if (!Files.isDirectory(parent, LinkOption.NOFOLLOW_LINKS)) {
            fail(DoctorStoreFailure.INVALID_REQUEST)
        }
        val temporary = parent.resolve(".doctor-save-${UUID.randomUUID()}.tmp")
        try {
            Files.copy(source, temporary, StandardCopyOption.COPY_ATTRIBUTES)
            FileChannel.open(temporary, StandardOpenOption.WRITE).use { it.force(true) }
            Files.move(
                temporary,
                destination,
                StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING,
            )
        } catch (failure: Throwable) {
            Files.deleteIfExists(temporary)
            throw failure
        }
        forceDirectory(parent)
    }

    private fun readManifestIfValid(path: Path): DoctorExportManifest? {
        // Enumeration already accepted an owned regular file. Substitution
        // during cleanup is an I/O failure, not permission to delete its replacement.
        if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) fail(DoctorStoreFailure.IO_ERROR)
        return try {
            ZipFile(path.toFile()).use { zip ->
                val names = zip.entries().asSequence().map { it.name }.toList()
                if (names.size != ZIP_ENTRIES.size || names.toSet() != ZIP_ENTRIES) {
                    return@use null
                }
                val text = zip.readUtf8("doctor/manifest-v1.json") ?: return@use null
                readManifestFromText(text)
            }
        } catch (failure: IOException) {
            null
        }
    }

    private fun readManifestFromText(text: String): DoctorExportManifest? {
        val fields = parseDoctorArchiveObject(text, MANIFEST_FIELDS) ?: return null
        if (fields.longField("schemaVersion") != 1L) return null
        val version = (fields["version"] as? DoctorJsonString)?.value ?: return null
        val commit = (fields["sourceCommit"] as? DoctorJsonString)?.value ?: return null
        if (!DoctorEventCodec.validBuild(DoctorBuildMetadata(version, commit))) return null
        val epoch = fields.longField("epoch") ?: return null
        val created = fields.longField("createdAtMs") ?: return null
        val expires = fields.longField("expiresAtMs") ?: return null
        val recordCount = fields.intField("recordCount") ?: return null
        val ignored = fields.longField("ignoredLines") ?: return null
        val gap = fields.longField("sequenceGap") ?: return null
        val oldest = fields.longField("oldestRetainedMs") ?: return null
        val newest = fields.longField("newestRetainedMs") ?: return null
        if (
            epoch < 0L ||
            created < 0L ||
            expires < created ||
            recordCount < 0 ||
            ignored < 0L ||
            gap < 0L || oldest < 0L || newest < oldest
        ) {
            return null
        }
        return DoctorExportManifest(epoch, created, expires, recordCount, ignored, gap, oldest, newest)
    }

    private fun requireCurrentEpoch(epoch: Long) {
        if (epoch != control.epoch) fail(DoctorStoreFailure.EPOCH_MISMATCH)
    }

    private fun requireAccessibleEpoch(epoch: Long) {
        requireCurrentEpoch(epoch)
        if (control.pendingClear) fail(DoctorStoreFailure.CLEAR_INCOMPLETE)
        control.failure?.let(::fail)
        if (runtimeFailure == DoctorStoreFailure.EPOCH_COMMIT_FAILED) fail(runtimeFailure!!)
    }

    private fun requireSnapshotLocation(path: Path) {
        val normalized = path.toAbsolutePath().normalize()
        val directory = exportDirectory().toAbsolutePath().normalize()
        if (
            normalized.parent != directory ||
            !EXPORT_FILE_NAME.matches(path.fileName.toString())
        ) {
            fail(DoctorStoreFailure.INVALID_SNAPSHOT)
        }
    }

    private fun loadControlState() {
        val path = controlPath()
        if (!Files.exists(path)) {
            control = DoctorControlState()
            return
        }
        if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) {
            fail(DoctorStoreFailure.INVALID_SCHEMA)
        }
        try {
            control = parseControl(Files.newBufferedReader(path, Charsets.UTF_8).use { it.readText() })
        } catch (failure: DoctorControlParseException) {
            control = DoctorControlState(failure = DoctorStoreFailure.INVALID_SCHEMA)
            runtimeFailure = DoctorStoreFailure.INVALID_SCHEMA
        }
    }

    private fun parseControl(text: String): DoctorControlState {
        val lines = text.lineSequence()
            .map { it.trimEnd('\r') }
            .filter { it.isNotEmpty() }
            .toList()
        if (lines.firstOrNull() != CONTROL_MAGIC || lines.size != 6) {
            throw DoctorControlParseException()
        }
        val fields = linkedMapOf<String, String>()
        for (line in lines.drop(1)) {
            val separator = line.indexOf('=')
            if (separator <= 0 || separator == line.lastIndex) throw DoctorControlParseException()
            if (fields.put(line.substring(0, separator), line.substring(separator + 1)) != null) {
                throw DoctorControlParseException()
            }
        }
        if (fields.keys != CONTROL_FIELDS) throw DoctorControlParseException()
        val epoch = fields.getValue("epoch").toLongOrNull()
            ?: throw DoctorControlParseException()
        val sequenceGap = fields.getValue("sequenceGap").toLongOrNull()
            ?: throw DoctorControlParseException()
        if (epoch < 0L || sequenceGap < 0L) throw DoctorControlParseException()
        val enabled = when (fields.getValue("enabled")) {
            "0" -> false
            "1" -> true
            else -> throw DoctorControlParseException()
        }
        val pending = when (fields.getValue("pendingClear")) {
            "0" -> false
            "1" -> true
            else -> throw DoctorControlParseException()
        }
        val failureText = fields.getValue("failure")
        val failure = if (failureText == "NONE") {
            null
        } else {
            DoctorStoreFailure.entries.firstOrNull { it.name == failureText }
                ?: throw DoctorControlParseException()
        }
        return DoctorControlState(enabled, epoch, pending, failure, sequenceGap)
    }

    private fun persistControl(
        state: DoctorControlState,
        operation: DoctorIoOperation,
    ) {
        injectedFailure(operation)
        val bytes = buildString {
            append(CONTROL_MAGIC).append('\n')
            append("enabled=").append(if (state.enabled) 1 else 0).append('\n')
            append("epoch=").append(state.epoch).append('\n')
            append("pendingClear=").append(if (state.pendingClear) 1 else 0).append('\n')
            append("failure=").append(state.failure?.name ?: "NONE").append('\n')
            append("sequenceGap=").append(state.sequenceGap).append('\n')
        }.toByteArray(Charsets.UTF_8)
        Files.createDirectories(root)
        atomicWrite(controlPath(), bytes)
    }

    private fun atomicWrite(destination: Path, bytes: ByteArray) {
        val temporary = destination.resolveSibling(".${destination.fileName}.${UUID.randomUUID()}.tmp")
        // CREATE_NEW either gives this operation ownership or throws before
        // cleanup is needed. A successful atomic move consumes the staging file.
        val channel = FileChannel.open(
            temporary,
            StandardOpenOption.CREATE_NEW,
            StandardOpenOption.WRITE,
        )
        try {
            channel.use {
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) it.write(buffer)
                it.force(true)
            }
            Files.move(
                temporary,
                destination,
                StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING,
            )
            forceDirectory(destination.parent)
        } catch (failure: Throwable) {
            if (Files.isRegularFile(temporary, LinkOption.NOFOLLOW_LINKS)) {
                Files.delete(temporary)
            }
            throw failure
        }
    }

    private fun forceDirectory(path: Path) {
        FileChannel.open(path, StandardOpenOption.READ).use { it.force(true) }
    }

    private fun ensureRootDirectory() {
        if (Files.exists(root)) {
            if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) {
                fail(DoctorStoreFailure.IO_ERROR)
            }
        } else {
            Files.createDirectories(root)
        }
    }

    private fun nextRecordPath(files: List<Path>): Path {
        // Summaries are rebuilt in ordinal order and new segments append to it.
        val next = files.lastOrNull()?.let(::recordOrdinal)?.let { current ->
            if (current == Long.MAX_VALUE) fail(DoctorStoreFailure.RECORD_LIMIT)
            current + 1
        } ?: 0L
        return recordsDirectory().resolve(
            "doctor-record-${next.toString().padStart(8, '0')}.jsonl",
        )
    }

    private fun allowlistedFiles(directory: Path, pattern: Regex): List<Path> {
        if (!Files.exists(directory)) return emptyList()
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
            fail(DoctorStoreFailure.IO_ERROR)
        }
        return Files.list(directory).use { paths ->
            paths.filter {
                pattern.matches(it.fileName.toString()) &&
                    Files.isRegularFile(it, LinkOption.NOFOLLOW_LINKS)
            }.iterator().asSequence().toList()
        }
    }

    private fun recordFiles(): List<Path> =
        allowlistedFiles(recordsDirectory(), RECORD_FILE_NAME).sortedBy(::recordOrdinal)

    private fun recordStagingFiles(): List<Path> =
        allowlistedFiles(recordsDirectory(), RECORD_STAGING_FILE_NAME).sortedBy { it.fileName.toString() }

    private fun ownedStagingFiles(): List<Path> {
        val controlFiles = allowlistedFiles(root, CONTROL_STAGING_FILE_PATTERN)
        return (recordStagingFiles() + controlFiles).sortedBy { it.toString() }
    }

    private fun allowlistedExportFiles(): List<Path> =
        allowlistedFiles(exportDirectory(), EXPORT_FILE_NAME)

    private fun refreshFileSizes() {
        rawBytes = recordSummaries.values.sumOf { it.bytes }
        tempBytes = allowlistedExportFiles().sumOf { Files.size(it) }
    }

    private fun refreshAfterFailure() {
        try {
            refreshFileSizes()
        } catch (failure: Exception) {
            runtimeFailure = failure.doctorStoreFailure()
        }
    }

    private fun observeDiagnostics(
        failure: DoctorStoreFailure?,
        observedIgnored: Long,
    ) {
        if (failure != null) diagnosticFailure = failure
        ignoredLines = maxOf(ignoredLines, observedIgnored)
    }

    private fun effectiveFailure(): DoctorStoreFailure? =
        runtimeFailure ?: control.failure ?: diagnosticFailure

    private fun injectedFailure(operation: DoctorIoOperation) {
        ioFaults.failureBefore(operation)?.let(::fail)
    }

    private fun controlPath(): Path = root.resolve(CONTROL_FILE_NAME)
    private fun recordsDirectory(): Path = root.resolve("records")
    private fun exportDirectory(): Path = root.resolve("temp")

    private fun isRestrictedFailure(failure: DoctorEventFailure): Boolean =
        failure == DoctorEventFailure.RESTRICTED_INPUT_EVENT ||
            failure == DoctorEventFailure.RESTRICTED_CORRELATION ||
            failure == DoctorEventFailure.RESTRICTED_COUNT_FIELD

    private fun DoctorEventFailure.toStoreFailure(): DoctorStoreFailure =
        if (this == DoctorEventFailure.TORN_LINE) {
            DoctorStoreFailure.TORN_TAIL
        } else {
            DoctorStoreFailure.INVALID_SCHEMA
        }

    private fun interruptsCollection(failure: DoctorStoreFailure?): Boolean =
        failure == DoctorStoreFailure.EPOCH_COMMIT_FAILED ||
            failure == DoctorStoreFailure.CLEAR_INCOMPLETE ||
            failure == DoctorStoreFailure.DISK_FULL ||
            failure == DoctorStoreFailure.PERMISSION_DENIED ||
            failure == DoctorStoreFailure.IO_ERROR

    private inline fun fail(reason: DoctorStoreFailure): Nothing =
        throw DoctorStoreFixedException(reason)

    private inline fun controlResult(block: () -> Unit): DoctorStoreResult<Unit> =
        try {
            block()
            DoctorStoreResult.Ok(Unit)
        } catch (failure: Exception) {
            val reason = failure.doctorStoreFailure()
            runtimeFailure = reason
            DoctorStoreResult.Error(reason)
        }

    private inline fun <T> storeResult(block: () -> T): DoctorStoreResult<T> =
        try {
            DoctorStoreResult.Ok(block())
        } catch (failure: Exception) {
            DoctorStoreResult.Error(failure.doctorStoreFailure())
        }

    companion object {
        fun open(
            root: Path,
            build: DoctorBuildMetadata,
            clock: DoctorClock,
            limits: DoctorStoreLimits = DoctorStoreLimits.DEFAULT,
            ioFaults: DoctorIoFaults = DoctorIoFaults.NONE,
        ): DoctorStore =
            DoctorStore(root.toAbsolutePath().normalize(), build, clock, limits, ioFaults)
                .also(DoctorStore::initialize)

        private const val CONTROL_FILE_NAME = "doctor-control-v1"
        private const val UUID_PATTERN = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
        private val CONTROL_STAGING_FILE_PATTERN = Regex("\\.doctor-control-v1(?:\\.$UUID_PATTERN)?\\.tmp")
        private const val CONTROL_MAGIC = "LIUKAI_DOCTOR_CONTROL_V1"
        private val CONTROL_FIELDS = setOf(
            "enabled",
            "epoch",
            "pendingClear",
            "failure",
            "sequenceGap",
        )
        private val RECORD_FILE_NAME = Regex("doctor-record-[0-9]+\\.jsonl")
        private val RECORD_STAGING_FILE_NAME =
            Regex("\\.doctor-record-[0-9]+\\.jsonl(?:\\.$UUID_PATTERN)?\\.tmp")
        private val EXPORT_FILE_NAME =
            Regex("doctor-export-$UUID_PATTERN\\.zip")
        private val ZIP_ENTRIES = setOf(
            "doctor/manifest-v1.json",
            "doctor/mark-v1.json",
            "doctor/events-v1.jsonl",
        )
        private val MANIFEST_FIELDS = setOf("schemaVersion", "version", "sourceCommit", "epoch",
            "createdAtMs", "expiresAtMs", "recordCount", "oldestRetainedMs", "newestRetainedMs",
            "ignoredLines", "sequenceGap")
        private val MARK_FIELDS = setOf("schemaVersion", "epoch", "actionTimeMs",
            "selectedIssueTimeMs", "beforeMs", "afterMs")

        private fun recordOrdinal(path: Path): Long =
            path.fileName.toString()
                .substringAfter("doctor-record-")
                .substringBefore(".jsonl")
                .toLongOrNull()
                ?: throw DoctorStoreFixedException(DoctorStoreFailure.INVALID_SCHEMA)

        private fun saturatingAdd(first: Long, second: Long): Long =
            if (first > Long.MAX_VALUE - second) Long.MAX_VALUE else first + second

        private fun saturatingSubtract(first: Long, second: Long): Long =
            if (second > first) 0L else first - second
    }
}

private data class DoctorControlState(
    val enabled: Boolean = false,
    val epoch: Long = 0L,
    val pendingClear: Boolean = false,
    val failure: DoctorStoreFailure? = null,
    val sequenceGap: Long = 0L,
)

private data class DoctorFileScan(
    val path: Path,
    val envelopes: List<DoctorEventEnvelope>,
    val failure: DoctorEventFailure?,
    val ignoredLines: Long,
)

private data class DoctorRecordSummary(
    val bytes: Long,
    val eventCount: Long,
    val currentEpochEventCount: Long,
    val hasForeignEpoch: Boolean,
    val oldestExpiryMs: Long?,
    val newestExpiryMs: Long?,
    val failure: DoctorEventFailure?,
    val ignoredLines: Long,
    val oldestRetainedMs: Long?,
    val newestRetainedMs: Long?,
)

private data class DoctorRecordScan(
    val events: List<DoctorEventEnvelope>,
    val failure: DoctorStoreFailure?,
    val ignoredLines: Long,
)

private data class DoctorExportManifest(
    val epoch: Long,
    val createdAtMs: Long,
    val expiresAtMs: Long,
    val recordCount: Int,
    val ignoredLines: Long,
    val sequenceGap: Long,
    val oldestRetainedMs: Long,
    val newestRetainedMs: Long,
)

private class DoctorBoundedOutputStream(
    output: OutputStream,
    private val limit: Long,
    private val failure: DoctorStoreFailure,
    private val path: Path,
) : FilterOutputStream(output) {
    private var written = 0L

    override fun write(value: Int) {
        ensureCapacity(1)
        super.write(value)
        written += 1
    }

    override fun write(bytes: ByteArray, offset: Int, length: Int) {
        ensureCapacity(length.toLong())
        // FilterOutputStream.write(array) calls write(int) again; delegating
        // directly avoids counting the payload twice and writing byte by byte.
        out.write(bytes, offset, length)
        written += length.toLong()
    }

    private fun ensureCapacity(additional: Long) {
        if (additional > limit - written) {
            throw DoctorStoreCapacityException(failure, path)
        }
    }

}

private fun ZipOutputStream.putFixedEntry(name: String, text: String) {
    val bytes = text.toByteArray(Charsets.UTF_8)
    val entry = ZipEntry(name).apply {
        time = 0L
        method = ZipEntry.STORED
        size = bytes.size.toLong()
        compressedSize = size
        crc = CRC32().apply { update(bytes) }.value
    }
    putNextEntry(entry)
    write(bytes)
    closeEntry()
}

private fun ZipFile.readUtf8(name: String): String? {
    // Both callers first verify the exact three-entry allowlist in this open ZIP.
    val entry = getEntry(name)
    // ZIP lengths are unsigned. This also rejects the unknown-size sentinel -1.
    if (entry.method != ZipEntry.STORED ||
        java.lang.Long.compareUnsigned(entry.size, DoctorStoreLimits.DEFAULT.maxExportBytes) > 0 ||
        entry.compressedSize != entry.size) return null
    val bytes = getInputStream(entry).use { it.readBytes() }
    if (bytes.size.toLong() != entry.size || CRC32().apply { update(bytes) }.value != entry.crc) return null
    return bytes.toString(Charsets.UTF_8)
}

private fun parseDoctorArchiveObject(text: String, fields: Set<String>): Map<String, DoctorJsonValue>? =
    try { DoctorJsonParser(text).parseObject().takeIf { it.keys == fields } }
    catch (failure: DoctorJsonParseFailure) { null }

private fun Map<String, DoctorJsonValue>.longField(name: String): Long? =
    (get(name) as? DoctorJsonLong)?.value

private fun Map<String, DoctorJsonValue>.intField(name: String): Int? =
    longField(name)?.takeIf {
        it in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()
    }?.toInt()

private class DoctorStoreFixedException(
    val reason: DoctorStoreFailure,
) : IOException()

private class DoctorStoreCapacityException(
    val reason: DoctorStoreFailure,
    val path: Path,
) : IOException()

private class DoctorControlParseException : IOException()

private fun Exception.doctorStoreFailure(): DoctorStoreFailure =
    when (this) {
        is DoctorStoreFixedException -> reason
        is AccessDeniedException -> DoctorStoreFailure.PERMISSION_DENIED
        is SecurityException -> DoctorStoreFailure.PERMISSION_DENIED
        is ReadOnlyFileSystemException -> DoctorStoreFailure.IO_ERROR
        is IOException -> DoctorStoreFailure.IO_ERROR
        else -> throw this
    }
