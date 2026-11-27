package com.hamanpaul.liukai.doctor

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorClock
import com.hamanpaul.liukai.core.doctor.DoctorEditorState
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorEventPhase
import com.hamanpaul.liukai.core.doctor.DoctorEventProjector
import com.hamanpaul.liukai.core.doctor.DoctorExportPreview
import com.hamanpaul.liukai.core.doctor.DoctorExportWindow
import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import com.hamanpaul.liukai.core.doctor.DoctorResultCode
import com.hamanpaul.liukai.core.doctor.DoctorStore
import com.hamanpaul.liukai.core.doctor.DoctorStoreFailure
import com.hamanpaul.liukai.core.doctor.DoctorStoreResult
import java.lang.ref.WeakReference
import java.security.SecureRandom
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.LockSupport
import java.util.TimeZone

internal data class DoctorRuntimeStatus(
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
    val dropped: Long,
    val oldestRetainedMs: Long?,
    val newestRetainedMs: Long?,
)

internal data class DoctorNumericSnapshot(
    val enabled: Int,
    val collecting: Int,
    val interrupted: Int,
    val epoch: Long,
    val queueDepth: Long,
    val rawBytes: Long,
    val tempBytes: Long,
    val sequenceGap: Long,
    val ignoredLines: Long,
    val operationsStarted: Long,
    val producerViolations: Long,
    val enqueued: Long,
    val written: Long,
    val writtenBytes: Long,
    val dropped: Long,
    val dispatchOff: List<Long>,
    val dispatchOn: List<Long>,
    val hardwareOff: List<Long>,
    val hardwareOn: List<Long>,
    val commitAttempts: Long,
    val commitSuccesses: Long,
    val lastCommitAttempted: Int,
    val lastCommitCommitted: Int,
    val durationBucketWidthUs: Long = DoctorInstrumentation.DURATION_BUCKET_WIDTH_US,
    val durationOverflowUs: Long = DoctorInstrumentation.DURATION_OVERFLOW_US,
)

internal enum class DoctorUiOperation {
    ENABLE,
    DISABLE,
    MARK,
    PREPARE,
    SAVE,
    CANCEL_EXPORT,
    CLEAR,
}

internal data class DoctorUiResult(
    val operation: DoctorUiOperation,
    val success: Boolean,
    val failure: DoctorStoreFailure?,
    val export: DoctorPreparedExport? = null,
)

internal interface DoctorRuntimeListener {
    fun onDoctorStatus(status: DoctorRuntimeStatus)
    fun onDoctorResult(result: DoctorUiResult)
}

internal class DoctorRuntimeSubscription internal constructor(
    private val controller: AndroidDoctorController,
    listener: DoctorRuntimeListener,
) {
    private val active = AtomicBoolean(true)
    private val target = WeakReference(listener)

    internal fun deliverStatus(status: DoctorRuntimeStatus) {
        if (!active.get()) return
        try {
            target.get()?.onDoctorStatus(status)
        } catch (_: Exception) {
            detach()
        }
    }

    internal fun deliverResult(result: DoctorUiResult) {
        if (!active.get()) return
        try {
            target.get()?.onDoctorResult(result)
        } catch (_: Exception) {
            detach()
        }
    }

    fun detach() {
        if (active.compareAndSet(true, false)) controller.remove(this)
    }
}

internal class AndroidDoctorController private constructor(context: Context) {
    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())
    private val commands = ConcurrentLinkedQueue<WriterCommand>()
    private val subscriptions = CopyOnWriteArrayList<DoctorRuntimeSubscription>()
    private val performanceSampling = AtomicBoolean(false)
    private val metrics = DoctorInstrumentation()
    private val ring = DoctorSpscRing(QUEUE_CAPACITY)
    private val writerExecutor = Executors.newSingleThreadExecutor { task ->
        Thread(task, "liu-kai-doctor-writer").apply { isDaemon = true }
    }
    private val clock = object : DoctorClock {
        override fun wallUtcMs(): Long = System.currentTimeMillis()
        override fun monotonicNs(): Long = SystemClock.elapsedRealtimeNanos()
    }
    private val producer = DoctorInputProducer(
        ring = ring,
        metrics = metrics,
        clock = clock,
        producerThreadId = Looper.getMainLooper().thread.id,
        wakeWriter = ::signalWriter,
    )
    private val writerRunnable = Runnable(::writerLoop)
    private val healthRunnable = object : Runnable {
        override fun run() {
            if (producer.isCollecting) {
                producer.recordHealth()
            }
            mainHandler.postDelayed(this, HEALTH_INTERVAL_MS)
        }
    }
    @Volatile private var writerThread: Thread? = null
    @Volatile private var core: DoctorWriterCore? = null
    @Volatile private var runtime = initialStatus()
    @Volatile private var lastPublishedNs = 0L
    @Volatile private var ordinaryInputField = false

    init {
        writerExecutor.execute(writerRunnable)
        mainHandler.post(healthRunnable)
    }

    fun subscribe(listener: DoctorRuntimeListener): DoctorRuntimeSubscription {
        val subscription = DoctorRuntimeSubscription(this, listener)
        subscriptions += subscription
        val snapshot = runtime
        mainHandler.post { subscription.deliverStatus(snapshot) }
        return subscription
    }

    fun setEnabled(enabled: Boolean, subscription: DoctorRuntimeSubscription) {
        if (!enabled) producer.close()
        enqueue(
            if (enabled) {
                WriterCommand.Enable(subscription)
            } else {
                WriterCommand.Disable(subscription)
            },
        )
    }

    fun markIssue(
        selectedIssueTimeMs: Long,
        subscription: DoctorRuntimeSubscription,
        window: DoctorExportWindow = DoctorExportWindow.DEFAULT,
    ) {
        enqueue(WriterCommand.Mark(selectedIssueTimeMs, window, subscription))
    }

    fun prepareExport(subscription: DoctorRuntimeSubscription) {
        enqueue(WriterCommand.Prepare(subscription))
    }

    fun saveExport(
        handle: DoctorExportHandle,
        target: DoctorExportTarget,
        subscription: DoctorRuntimeSubscription,
    ) {
        enqueue(WriterCommand.Save(handle, target, subscription))
    }

    fun cancelExport(handle: DoctorExportHandle, subscription: DoctorRuntimeSubscription?) {
        enqueue(WriterCommand.Cancel(handle, subscription))
    }

    fun clear(subscription: DoctorRuntimeSubscription) {
        producer.close()
        enqueue(WriterCommand.Clear(subscription))
    }

    fun updateInputState(
        editor: DoctorEditorState?,
        mode: DoctorEventMode,
        focus: Boolean,
    ) {
        val scope = DoctorEventProjector.classify(editor)
        val ordinary = scope == DoctorInputScope.ORDINARY
        if (!ordinary) ordinaryInputField = false
        producer.setInputState(scope, mode, focus)
        if (ordinary) ordinaryInputField = true
    }

    fun updateMode(mode: DoctorEventMode) {
        producer.setInputState(producer.inputScope, mode, producer.inputFocus)
    }

    fun recordInputStarted() {
        producer.recordLifecycle(DoctorEventCode.ACTIVATE_BEGIN)
        producer.recordLifecycle(DoctorEventCode.FOCUS)
    }

    fun recordInputFinished() {
        producer.recordLifecycle(DoctorEventCode.DEACTIVATE)
    }

    fun beginOperation(): Long = producer.beginOperation()

    inline fun <T> withOperation(
        operationId: Long,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
        block: () -> T,
    ): T = producer.withOperation(operationId, gestureToken, pointerCount, pressed, block)

    val activeOperationId: Long
        get() = producer.activeOperationId

    fun recordTouch(
        operationId: Long,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
        source: DoctorCallbackSource,
        handled: Boolean,
        durationUs: Long,
    ) {
        producer.recordTouch(
            operationId,
            gestureToken,
            pointerCount,
            pressed,
            source,
            handled,
            durationUs,
        )
    }

    fun beginTouchChain(
        operationId: Long,
        gestureToken: Long,
        pointerCount: Int,
        pressed: Boolean,
    ): Boolean = producer.beginTouchChain(operationId, gestureToken, pointerCount, pressed)

    fun finishTouchChain(handled: Boolean, durationUs: Long) {
        producer.finishTouchChain(handled, durationUs)
    }

    fun recordComposition(handled: Boolean, durationUs: Long) {
        recordActiveStage(
            eventCode = DoctorEventCode.EDIT_REQUEST,
            phase = DoctorEventPhase.COMPOSITION,
            handled = handled,
            resultCode = handled.resultCode(),
            durationUs = durationUs,
        )
    }

    fun recordCandidate(handled: Boolean, durationUs: Long) {
        recordActiveStage(
            eventCode = DoctorEventCode.EDIT_REQUEST,
            phase = DoctorEventPhase.CANDIDATE,
            handled = handled,
            resultCode = handled.resultCode(),
            durationUs = durationUs,
        )
    }

    fun recordCommit(committed: Boolean, durationUs: Long) {
        if (!ordinaryMetricsAllowed()) return
        metrics.recordCommit(ordinary = ordinaryInputField, committed = committed)
        recordActiveStage(
            eventCode = DoctorEventCode.EDIT_APPLIED,
            phase = DoctorEventPhase.COMMIT,
            handled = committed,
            resultCode = committed.resultCode(),
            durationUs = durationUs,
        )
    }

    fun recordDispatch(startedNs: Long) {
        if (!ordinaryMetricsAllowed()) return
        metrics.recordDispatch(
            ordinary = ordinaryInputField,
            enabled = producer.isCollecting,
            durationUs = elapsedUs(startedNs),
        )
    }

    fun recordHardware(startedNs: Long) {
        if (!ordinaryMetricsAllowed()) return
        metrics.recordHardware(
            ordinary = ordinaryInputField,
            enabled = producer.isCollecting,
            durationUs = elapsedUs(startedNs),
        )
    }

    fun status(): DoctorRuntimeStatus = runtime

    fun numericSnapshot(): DoctorNumericSnapshot {
        val state = runtime
        val probe = metrics.snapshot()
        return DoctorNumericSnapshot(
            enabled = state.enabled.toInt(),
            collecting = state.collecting.toInt(),
            interrupted = state.interrupted.toInt(),
            epoch = state.epoch,
            queueDepth = ring.depth,
            rawBytes = state.rawBytes,
            tempBytes = state.tempBytes,
            sequenceGap = state.sequenceGap,
            ignoredLines = state.ignoredLines,
            operationsStarted = probe.operationsStarted,
            producerViolations = probe.producerViolations,
            enqueued = probe.enqueued,
            written = probe.written,
            writtenBytes = probe.writtenBytes,
            dropped = probe.dropped,
            dispatchOff = probe.dispatchOff,
            dispatchOn = probe.dispatchOn,
            hardwareOff = probe.hardwareOff,
            hardwareOn = probe.hardwareOn,
            commitAttempts = probe.commitAttempts,
            commitSuccesses = probe.commitSuccesses,
            lastCommitAttempted = probe.lastCommitAttempted,
            lastCommitCommitted = probe.lastCommitCommitted,
        )
    }

    fun setPerformanceSamplingForTest(enabled: Boolean) {
        performanceSampling.set(enabled)
    }

    fun resetInstrumentationForTest() {
        metrics.resetForTest()
    }

    fun awaitWriterIdleForTest(timeoutMs: Long): Boolean {
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "Doctor writer probe must not wait on the main looper"
        }
        val acknowledged = CountDownLatch(1)
        enqueue(WriterCommand.Idle(acknowledged))
        return acknowledged.await(timeoutMs.coerceAtLeast(0L), TimeUnit.MILLISECONDS)
    }

    internal fun remove(subscription: DoctorRuntimeSubscription) {
        subscriptions.remove(subscription)
    }

    private fun writerLoop() {
        writerThread = Thread.currentThread()
        val random = SecureRandom()
        val writerCore = DoctorWriterCore(
            store = DoctorStore.open(
                root = appContext.filesDir.toPath().resolve(DOCTOR_DIRECTORY),
                build = AndroidDoctorBuild.metadata,
                clock = clock,
            ),
            ring = ring,
            producer = producer,
            metrics = metrics,
            producerId = random.positiveId(),
            sessionId = random.positiveId(),
            timezoneOffsetMinutes = currentTimezoneOffsetMinutes(),
            build = AndroidDoctorBuild.metadata,
        )
        core = writerCore
        publish(writerCore.state(), force = true)

        while (true) {
            var progressed = false
            var command = commands.poll()
            while (command != null) {
                execute(writerCore, command)
                progressed = true
                command = commands.poll()
            }
            if (writerCore.pumpOneBatch() > 0) {
                publish(writerCore.state())
                progressed = true
            }
            if (!progressed) {
                LockSupport.parkNanos(IDLE_WAIT_NS)
            }
        }
    }

    private fun execute(writer: DoctorWriterCore, command: WriterCommand) {
        when (command) {
            is WriterCommand.Enable -> deliver(
                command.subscription,
                DoctorUiOperation.ENABLE,
                writer.enable(),
            )
            is WriterCommand.Disable -> deliver(
                command.subscription,
                DoctorUiOperation.DISABLE,
                writer.disable(),
            )
            is WriterCommand.Mark -> deliver(
                command.subscription,
                DoctorUiOperation.MARK,
                writer.markIssue(command.selectedIssueTimeMs, command.window),
            )
            is WriterCommand.Prepare -> {
                val result = writer.prepareExport()
                deliver(
                    command.subscription,
                    when (result) {
                        is DoctorStoreResult.Ok -> DoctorUiResult(DoctorUiOperation.PREPARE, true, null, result.value)
                        is DoctorStoreResult.Error -> result.errorUi(DoctorUiOperation.PREPARE)
                    },
                )
            }
            is WriterCommand.Save -> deliver(
                command.subscription,
                DoctorUiOperation.SAVE,
                writer.saveExport(command.handle, command.target),
            )
            is WriterCommand.Cancel -> deliver(
                command.subscription,
                DoctorUiOperation.CANCEL_EXPORT,
                writer.cancelExport(command.handle),
            )
            is WriterCommand.Clear -> deliver(
                command.subscription,
                DoctorUiOperation.CLEAR,
                writer.clear(),
            )
            is WriterCommand.Idle -> {
                try {
                    while (writer.pumpOneBatch() > 0) Unit
                    publish(writer.state(), force = true)
                } finally {
                    command.acknowledged.countDown()
                }
            }
        }
        publish(writer.state())
    }

    private fun enqueue(command: WriterCommand) {
        commands.offer(command)
        signalWriter()
    }

    private fun signalWriter() {
        writerThread?.let(LockSupport::unpark)
    }

    private fun deliver(
        subscription: DoctorRuntimeSubscription?,
        operation: DoctorUiOperation,
        result: DoctorStoreResult<*>,
    ) {
        val uiResult = when (result) {
            is DoctorStoreResult.Ok -> DoctorUiResult(operation, true, null)
            is DoctorStoreResult.Error -> DoctorUiResult(operation, false, result.reason)
        }
        deliver(subscription, uiResult)
    }

    private fun deliver(
        subscription: DoctorRuntimeSubscription?,
        result: DoctorUiResult,
    ) {
        if (subscription != null) {
            mainHandler.post { subscription.deliverResult(result) }
        }
    }

    private fun publish(state: DoctorWriterState, force: Boolean = false) {
        val status = DoctorRuntimeStatus(
            enabled = state.enabled,
            collecting = state.collecting,
            interrupted = state.interrupted,
            pendingClear = state.pendingClear,
            epoch = state.epoch,
            failure = state.failure,
            rawBytes = state.rawBytes,
            tempBytes = state.tempBytes,
            sequenceGap = state.sequenceGap,
            ignoredLines = state.ignoredLines,
            dropped = metrics.droppedCount,
            oldestRetainedMs = state.oldestRetainedMs,
            newestRetainedMs = state.newestRetainedMs,
        )
        val now = clock.monotonicNs()
        if (!force && status == runtime && now - lastPublishedNs < PUBLISH_INTERVAL_NS) return
        runtime = status
        lastPublishedNs = now
        mainHandler.post {
            subscriptions.forEach { it.deliverStatus(status) }
        }
    }

    private fun recordActiveStage(
        eventCode: DoctorEventCode,
        phase: DoctorEventPhase,
        handled: Boolean,
        resultCode: DoctorResultCode,
        durationUs: Long,
    ) {
        producer.recordStage(
            operationId = producer.activeOperationId,
            eventCode = eventCode,
            phase = phase,
            handled = handled,
            resultCode = resultCode,
            durationUs = durationUs,
        )
    }

    private fun ordinaryMetricsAllowed(): Boolean =
        ordinaryInputField &&
            (producer.isCollecting || performanceSampling.get())

    private fun Boolean.resultCode(): DoctorResultCode =
        if (this) DoctorResultCode.OK else DoctorResultCode.REJECTED

    private fun Boolean.toInt(): Int = if (this) 1 else 0

    private fun elapsedUs(startedNs: Long): Long =
        ((clock.monotonicNs() - startedNs) / 1_000L).coerceAtLeast(0L)

    private fun currentTimezoneOffsetMinutes(): Int =
        TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000

    private fun SecureRandom.positiveId(): Long {
        var value: Long
        do {
            value = nextLong() and Long.MAX_VALUE
        } while (value == 0L)
        return value
    }

    private fun DoctorStoreResult.Error.errorUi(operation: DoctorUiOperation): DoctorUiResult =
        DoctorUiResult(operation, false, reason)

    private fun initialStatus() =
        DoctorRuntimeStatus(
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
            dropped = 0L,
            oldestRetainedMs = null,
            newestRetainedMs = null,
        )

    private sealed interface WriterCommand {
        data class Enable(val subscription: DoctorRuntimeSubscription) : WriterCommand
        data class Disable(val subscription: DoctorRuntimeSubscription) : WriterCommand
        data class Mark(
            val selectedIssueTimeMs: Long,
            val window: DoctorExportWindow,
            val subscription: DoctorRuntimeSubscription,
        ) : WriterCommand
        data class Prepare(val subscription: DoctorRuntimeSubscription) : WriterCommand
        data class Save(
            val handle: DoctorExportHandle,
            val target: DoctorExportTarget,
            val subscription: DoctorRuntimeSubscription,
        ) : WriterCommand
        data class Cancel(
            val handle: DoctorExportHandle,
            val subscription: DoctorRuntimeSubscription?,
        ) : WriterCommand
        data class Clear(val subscription: DoctorRuntimeSubscription) : WriterCommand
        data class Idle(val acknowledged: CountDownLatch) : WriterCommand
    }

    companion object {
        private const val DOCTOR_DIRECTORY = "doctor"
        private const val QUEUE_CAPACITY = 1024
        private const val HEALTH_INTERVAL_MS = 30_000L
        private const val IDLE_WAIT_NS = 50_000_000L
        private const val PUBLISH_INTERVAL_NS = 250_000_000L

        @Volatile private var instance: AndroidDoctorController? = null

        fun get(context: Context): AndroidDoctorController =
            instance ?: synchronized(this) {
                instance ?: AndroidDoctorController(context).also { instance = it }
            }
    }
}
