package com.hamanpaul.liukai.doctor

import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicLongArray

internal data class DoctorInstrumentationSnapshot(
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
)

internal class DoctorInstrumentation {
    private val operationsStarted = AtomicLong()
    private val producerViolations = AtomicLong()
    private val enqueued = AtomicLong()
    private val written = AtomicLong()
    private val writtenBytes = AtomicLong()
    private val dropped = AtomicLong()
    private val dispatchOff = AtomicLongArray(HISTOGRAM_BUCKET_COUNT)
    private val dispatchOn = AtomicLongArray(HISTOGRAM_BUCKET_COUNT)
    private val hardwareOff = AtomicLongArray(HISTOGRAM_BUCKET_COUNT)
    private val hardwareOn = AtomicLongArray(HISTOGRAM_BUCKET_COUNT)
    private val commitAttempts = AtomicLong()
    private val commitSuccesses = AtomicLong()
    private val lastCommitAttempted = AtomicInteger()
    private val lastCommitCommitted = AtomicInteger()

    val droppedCount: Long get() = dropped.get()

    fun recordOperationStarted() {
        increment(operationsStarted)
    }

    fun recordProducerViolation() {
        increment(producerViolations)
    }

    fun recordEnqueued() {
        increment(enqueued)
    }

    fun recordWritten(bytes: Long) {
        increment(written)
        if (bytes > 0L) writtenBytes.addAndGet(bytes)
    }

    fun recordDrop() {
        increment(dropped)
    }

    fun recordDispatch(enabled: Boolean, durationUs: Long) =
        recordDispatch(ordinary = true, enabled = enabled, durationUs = durationUs)

    fun recordDispatch(ordinary: Boolean, enabled: Boolean, durationUs: Long) {
        if (!ordinary) return
        recordHistogram(if (enabled) dispatchOn else dispatchOff, durationUs)
    }

    fun recordHardware(enabled: Boolean, durationUs: Long) =
        recordHardware(ordinary = true, enabled = enabled, durationUs = durationUs)

    fun recordHardware(ordinary: Boolean, enabled: Boolean, durationUs: Long) {
        if (!ordinary) return
        recordHistogram(if (enabled) hardwareOn else hardwareOff, durationUs)
    }

    fun recordCommit(ordinary: Boolean, committed: Boolean) {
        if (!ordinary) return
        increment(commitAttempts)
        if (committed) increment(commitSuccesses)
        lastCommitAttempted.set(1)
        lastCommitCommitted.set(if (committed) 1 else 0)
    }

    fun snapshot(): DoctorInstrumentationSnapshot =
        DoctorInstrumentationSnapshot(
            operationsStarted = operationsStarted.get(),
            producerViolations = producerViolations.get(),
            enqueued = enqueued.get(),
            written = written.get(),
            writtenBytes = writtenBytes.get(),
            dropped = dropped.get(),
            dispatchOff = dispatchOff.toList(),
            dispatchOn = dispatchOn.toList(),
            hardwareOff = hardwareOff.toList(),
            hardwareOn = hardwareOn.toList(),
            commitAttempts = commitAttempts.get(),
            commitSuccesses = commitSuccesses.get(),
            lastCommitAttempted = lastCommitAttempted.get(),
            lastCommitCommitted = lastCommitCommitted.get(),
        )

    fun resetForTest() {
        operationsStarted.set(0L)
        producerViolations.set(0L)
        enqueued.set(0L)
        written.set(0L)
        writtenBytes.set(0L)
        dropped.set(0L)
        clear(dispatchOff)
        clear(dispatchOn)
        clear(hardwareOff)
        clear(hardwareOn)
        commitAttempts.set(0L)
        commitSuccesses.set(0L)
        lastCommitAttempted.set(0)
        lastCommitCommitted.set(0)
    }

    private fun recordHistogram(histogram: AtomicLongArray, durationUs: Long) {
        val bounded = durationUs.coerceAtLeast(0L)
        val index = minOf(bounded / DURATION_BUCKET_WIDTH_US,
            (HISTOGRAM_BUCKET_COUNT - 1).toLong()).toInt()
        increment(histogram, index)
    }

    private fun increment(counter: AtomicLong) {
        counter.getAndIncrement()
    }

    private fun increment(counter: AtomicLongArray, index: Int) {
        counter.incrementAndGet(index)
    }

    private fun clear(counter: AtomicLongArray) {
        for (index in 0 until counter.length()) counter.set(index, 0L)
    }

    private fun AtomicLongArray.toList(): List<Long> =
        List(length()) { get(it) }

    companion object {
        const val DURATION_BUCKET_WIDTH_US = 100L
        const val DURATION_OVERFLOW_US = 256_000L
        private const val HISTOGRAM_BUCKET_COUNT = 2_561
    }
}
