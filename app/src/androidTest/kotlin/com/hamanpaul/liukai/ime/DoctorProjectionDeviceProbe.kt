package com.hamanpaul.liukai.ime

import android.content.Context
import android.os.SystemClock
import android.text.InputType
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.doctor.DoctorEventProjector
import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import com.hamanpaul.liukai.core.doctor.DoctorTouchSource
import com.hamanpaul.liukai.doctor.AndroidDoctorController
import com.hamanpaul.liukai.doctor.DoctorNumericSnapshot
import com.hamanpaul.liukai.doctor.doctorEditorState
import com.hamanpaul.liukai.doctor.doctorInputScope
import org.json.JSONObject

internal object DoctorProjectionDeviceProbe {
    fun run(context: Context, runOnMain: (Runnable) -> Unit): String {
        val ordinary = EditorInfo().apply { inputType = InputType.TYPE_CLASS_TEXT }
        val password = EditorInfo().apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        val privateField = EditorInfo().apply {
            inputType = InputType.TYPE_CLASS_TEXT
            imeOptions = EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
        }
        val unknown = EditorInfo().apply { inputType = InputType.TYPE_NULL }

        requireScope(ordinary, DoctorInputScope.ORDINARY)
        requireScope(password, DoctorInputScope.SENSITIVE)
        requireScope(privateField, DoctorInputScope.SENSITIVE)
        requireScope(unknown, DoctorInputScope.UNKNOWN)

        val ordinaryState = ordinary.doctorEditorState()
        val event = DoctorEventProjector.touch(
            ordinaryState,
            gestureToken = 0x10203040,
            pointerCount = 2,
            pressed = true,
            source = DoctorTouchSource.SLOT_FORWARD,
        )
        val fields = event!!.toV1Fields()
        require(fields.keys == setOf(
            "schemaVersion", "eventCode", "gestureToken", "pointerCount", "pressed", "callbackSource",
        ))
        require(DoctorEventProjector.touch(
            password.doctorEditorState(), 1, 1, true, DoctorTouchSource.DISPATCH,
        ) == null)
        require(DoctorEventProjector.touch(
            unknown.doctorEditorState(), 1, 1, true, DoctorTouchSource.DISPATCH,
        ) == null)

        val doctor = AndroidDoctorController.get(context)
        val inputConnection = onMain(runOnMain) { BaseInputConnection(View(context), true) }
        require(doctor.awaitWriterIdleForTest(PROBE_TIMEOUT_MS)) {
            "Doctor writer did not acknowledge the initial quiet boundary"
        }
        require(!doctor.numericSnapshot().collectingBool()) {
            "Doctor callback probe requires Doctor to begin disabled"
        }

        val off = onMain(runOnMain) {
            exerciseCallbacks(doctor, ordinary, inputConnection, performanceSampling = false)
        }
        requireNoPerKeyMetrics(off)

        val ordinaryBaseline = onMain(runOnMain) {
            exerciseCallbacks(doctor, ordinary, inputConnection, performanceSampling = true)
        }
        require(ordinaryBaseline.dispatchOff.sum() == 1L)
        require(ordinaryBaseline.hardwareOff.sum() == 1L)
        require(ordinaryBaseline.dispatchOn.isZero())
        require(ordinaryBaseline.hardwareOn.isZero())
        require(ordinaryBaseline.commitAttempts == 1L)
        require(ordinaryBaseline.commitSuccesses == 1L)

        val sensitive = onMain(runOnMain) {
            exerciseCallbacks(doctor, privateField, inputConnection, performanceSampling = true)
        }
        requireNoPerKeyMetrics(sensitive)

        val unknownField = onMain(runOnMain) {
            exerciseCallbacks(doctor, unknown, inputConnection, performanceSampling = true)
        }
        requireNoPerKeyMetrics(unknownField)

        onMain(runOnMain) {
            doctor.setPerformanceSamplingForTest(false)
            doctor.updateInputState(null, DoctorEventMode.NONE, focus = false)
        }
        require(doctor.awaitWriterIdleForTest(PROBE_TIMEOUT_MS)) {
            "Doctor writer did not acknowledge the final quiet boundary"
        }

        return JSONObject()
            .put("ordinary", ordinary.doctorEditorState().doctorInputScope().name)
            .put("password", password.doctorEditorState().doctorInputScope().name)
            .put("private", privateField.doctorEditorState().doctorInputScope().name)
            .put("unknown", unknown.doctorEditorState().doctorInputScope().name)
            .put("ordinaryTouchFields", fields.keys.sorted().joinToString(","))
            .put("offOrdinaryPerKeySamples", off.perKeySamples())
            .put("ordinaryBaselineDispatchSamples", ordinaryBaseline.dispatchOff.sum())
            .put("ordinaryBaselineHardwareSamples", ordinaryBaseline.hardwareOff.sum())
            .put("sensitivePerKeySamples", sensitive.perKeySamples())
            .put("unknownPerKeySamples", unknownField.perKeySamples())
            .put("histogramBucketCount", ordinaryBaseline.dispatchOff.size)
            .put("writtenBytes", unknownField.writtenBytes)
            .toString()
    }

    private fun requireScope(info: EditorInfo, expected: DoctorInputScope) {
        require(info.doctorEditorState().doctorInputScope() == expected) {
            "Doctor scope for inputType=${info.inputType} imeOptions=${info.imeOptions} was not $expected"
        }
    }

    private fun exerciseCallbacks(
        doctor: AndroidDoctorController,
        info: EditorInfo,
        connection: InputConnection,
        performanceSampling: Boolean,
    ): DoctorNumericSnapshot {
        doctor.resetInstrumentationForTest()
        doctor.setPerformanceSamplingForTest(performanceSampling)
        doctor.updateInputState(info.doctorEditorState(), DoctorEventMode.LIU, focus = true)
        val startedNs = SystemClock.elapsedRealtimeNanos()
        val operation = doctor.beginOperation()
        doctor.withOperation(operation, operation.coerceAtLeast(0L), 0, false) {
            doctor.recordDispatch(startedNs)
            doctor.recordHardware(startedNs)
            doctor.recordComposition(handled = true, durationUs = 10L)
            doctor.recordCandidate(handled = true, durationUs = 11L)
            require(commitTextWithDoctor(connection, doctor, "x"))
        }
        return doctor.numericSnapshot()
    }

    private fun requireNoPerKeyMetrics(snapshot: DoctorNumericSnapshot) {
        require(snapshot.queueDepth == 0L)
        require(snapshot.operationsStarted == 0L)
        require(snapshot.producerViolations == 0L)
        require(snapshot.enqueued == 0L)
        require(snapshot.written == 0L)
        require(snapshot.writtenBytes == 0L)
        require(snapshot.dropped == 0L)
        require(snapshot.dispatchOff.isZero())
        require(snapshot.dispatchOn.isZero())
        require(snapshot.hardwareOff.isZero())
        require(snapshot.hardwareOn.isZero())
        require(snapshot.commitAttempts == 0L)
        require(snapshot.commitSuccesses == 0L)
        require(snapshot.lastCommitAttempted == 0)
        require(snapshot.lastCommitCommitted == 0)
    }

    private fun DoctorNumericSnapshot.perKeySamples(): Long =
        operationsStarted +
            dispatchOff.sum() +
            dispatchOn.sum() +
            hardwareOff.sum() +
            hardwareOn.sum() +
            commitAttempts +
            commitSuccesses

    private fun DoctorNumericSnapshot.collectingBool(): Boolean = collecting != 0

    private fun List<Long>.isZero(): Boolean = all { it == 0L }

    private inline fun <T> onMain(runOnMain: (Runnable) -> Unit, crossinline block: () -> T): T {
        var value: Result<T>? = null
        runOnMain(Runnable { value = runCatching(block) })
        return requireNotNull(value).getOrThrow()
    }

    private const val PROBE_TIMEOUT_MS = 5_000L
}
