package com.hamanpaul.liukai.ime
import android.app.Instrumentation
import android.content.Intent
import android.os.SystemClock
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.widget.TextView
import com.hamanpaul.liukai.core.doctor.DoctorEditorState
import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventMode
import com.hamanpaul.liukai.core.engine.InputMode
import com.hamanpaul.liukai.core.engine.Language
import com.hamanpaul.liukai.core.engine.ZhuyinStage
import com.hamanpaul.liukai.core.ime.SoftKey
import com.hamanpaul.liukai.data.KeyboardSettings
import com.hamanpaul.liukai.doctor.AndroidDoctorController
import com.hamanpaul.liukai.doctor.DoctorRuntimeListener
import com.hamanpaul.liukai.doctor.DoctorRuntimeStatus
import com.hamanpaul.liukai.doctor.DoctorRuntimeSubscription
import com.hamanpaul.liukai.doctor.DoctorUiOperation
import com.hamanpaul.liukai.doctor.DoctorUiResult
import com.hamanpaul.liukai.settings.SettingsActivity
import java.lang.ref.ReferenceQueue
import java.lang.ref.WeakReference
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

internal object DoctorStatusDeviceProbe {
    fun run(inst: Instrumentation): String {
        fun waitUntil(label: String, condition: () -> Boolean) {
            val end = SystemClock.uptimeMillis() + 10000
            while (SystemClock.uptimeMillis() < end) {
                if (condition()) return
                SystemClock.sleep(50)
            }
            throw AssertionError("status probe timeout: $label")
        }
        fun views(view: View): List<View> = listOf(view) + if (view is ViewGroup) {
            (0 until view.childCount).flatMap { views(view.getChildAt(it)) }
        } else {
            emptyList()
        }
        val monitor = inst.addMonitor(SettingsActivity::class.java.name, null, false)
        val activity = try {
            inst.targetContext.startActivity(Intent(inst.targetContext, SettingsActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            monitor.waitForActivityWithTimeout(10000) as? SettingsActivity ?: throw AssertionError("status activity timeout")
        } finally {
            inst.removeMonitor(monitor)
        }
        val controller = AndroidDoctorController.get(activity)
        val startupBefore = controller.status()
        val startedAt = SystemClock.elapsedRealtime()
        val initialized = controller.awaitWriterIdleForTest(60000)
        val startupAfter = controller.status()
        Log.i(
            "DoctorStatusDeviceProbe",
            "initialization-wait-ms=" + (SystemClock.elapsedRealtime() - startedAt) +
                " completed=$initialized before=$startupBefore after=$startupAfter",
        )
        check(initialized) { "Doctor writer initialization timed out" }
        val originalEnabled = startupAfter.enabled
        val deliveredBytes = AtomicLong(-1L)
        val deliveredResults = AtomicInteger()
        val listener = object : DoctorRuntimeListener {
            override fun onDoctorStatus(status: DoctorRuntimeStatus) {
                deliveredBytes.set(status.rawBytes)
            }

            override fun onDoctorResult(result: DoctorUiResult) {
                deliveredResults.incrementAndGet()
            }
        }
        val sub = controller.subscribe(listener)
        val subscriptions = mutableListOf(sub)
        var primaryFailure: Throwable? = null
        try {
            val positiveListener = RecordingDoctorListener()
            val positive = controller.subscribe(positiveListener).also { subscriptions.add(it) }
            val weak = subscribeWeakListener(controller).also {
                subscriptions.add(it.subscription)
            }
            val actualStatus = controller.status()
            val syntheticResult = DoctorUiResult(
                DoctorUiOperation.DISABLE,
                success = true,
                failure = null,
            )
            inst.runOnMainSync {
                val positiveStatusesBefore = positiveListener.statusCalls.get()
                val positiveResultsBefore = positiveListener.resultCalls.get()
                positive.deliverStatus(actualStatus)
                positive.deliverResult(syntheticResult)
                check(positiveListener.statusCalls.get() == positiveStatusesBefore + 1) {
                    "strong listener status delivery was not observed"
                }
                check(positiveListener.resultCalls.get() == positiveResultsBefore + 1) {
                    "strong listener result delivery was not observed"
                }
                check(positiveListener.lastStatus.get() === actualStatus) {
                    "strong listener did not receive the actual Doctor status"
                }
                check(positiveListener.lastResult.get() === syntheticResult) {
                    "strong listener did not receive the synthetic Doctor result"
                }

                val weakStatusesBefore = weak.statusCalls.get()
                val weakResultsBefore = weak.resultCalls.get()
                weak.subscription.deliverStatus(actualStatus)
                weak.subscription.deliverResult(syntheticResult)
                check(weak.statusCalls.get() == weakStatusesBefore + 1) {
                    "weak listener did not receive status before collection"
                }
                check(weak.resultCalls.get() == weakResultsBefore + 1) {
                    "weak listener did not receive result before collection"
                }
            }

            check(weak.retainedListener != null) { "positive-control listener was not retained" }
            weak.retainedListener = null
            awaitWeakListenerCollection(weak.reference, weak.queue)
            check(weak.reference.get() == null) {
                "weak listener remained strongly reachable after ReferenceQueue observation"
            }
            val collectedStatuses = weak.statusCalls.get()
            val collectedResults = weak.resultCalls.get()
            inst.runOnMainSync {
                weak.subscription.deliverStatus(controller.status())
                weak.subscription.deliverResult(syntheticResult)
                check(weak.statusCalls.get() == collectedStatuses) {
                    "collected weak listener received a status delivery"
                }
                check(weak.resultCalls.get() == collectedResults) {
                    "collected weak listener received a result delivery"
                }
            }
            Log.i(
                "DoctorStatusDeviceProbe",
                "weak-listener-collected=true status-calls=$collectedStatuses " +
                    "result-calls=$collectedResults strong-control=delivered",
            )

            val delivery = AtomicInteger()
            val resultDelivery = AtomicInteger()
            val normalListener = object : DoctorRuntimeListener {
                override fun onDoctorStatus(status: DoctorRuntimeStatus) {
                    delivery.incrementAndGet()
                }

                override fun onDoctorResult(result: DoctorUiResult) {
                    resultDelivery.incrementAndGet()
                }
            }
            val normal = controller.subscribe(normalListener).also { subscriptions.add(it) }
            waitUntil("normal subscription initial status") { delivery.get() > 0 }
            normal.detach()
            normal.detach()
            val detachedStatus = delivery.get()
            normal.deliverStatus(controller.status())
            normal.deliverResult(DoctorUiResult(DoctorUiOperation.DISABLE, true, null))
            controller.setEnabled(false, normal)
            check(controller.awaitWriterIdleForTest(10000)) { "detached subscription writer idle timeout" }
            check(delivery.get() == detachedStatus && resultDelivery.get() == 0) {
                "detached subscription accepted status=${
                    delivery.get() - detachedStatus
                } results=${resultDelivery.get()}"
            }

            val throwing = AtomicBoolean(false)
            val statusCalls = AtomicInteger()
            val resultCalls = AtomicInteger()
            val throwingListener = object : DoctorRuntimeListener {
                override fun onDoctorStatus(status: DoctorRuntimeStatus) {
                    if (throwing.get()) {
                        statusCalls.incrementAndGet()
                        throw IllegalStateException("status listener test")
                    }
                }

                override fun onDoctorResult(result: DoctorUiResult) = Unit
            }
            val throwingResultListener = object : DoctorRuntimeListener {
                override fun onDoctorStatus(status: DoctorRuntimeStatus) = Unit

                override fun onDoctorResult(result: DoctorUiResult) {
                    if (throwing.get()) {
                        resultCalls.incrementAndGet()
                        throw IllegalStateException("result listener test")
                    }
                }
            }
            val isolated = controller.subscribe(throwingListener).also { subscriptions.add(it) }
            val isolatedResult = controller.subscribe(throwingResultListener).also { subscriptions.add(it) }
            val initializedCallbacks = CountDownLatch(1)
            inst.runOnMainSync { initializedCallbacks.countDown() }
            check(initializedCallbacks.await(1, java.util.concurrent.TimeUnit.SECONDS)) {
                "listener initialization main turn missing"
            }
            var statusFailure: Throwable? = null
            var resultFailure: Throwable? = null
            inst.runOnMainSync {
                throwing.set(true)
                try {
                    statusFailure = runCatching {
                        isolated.deliverStatus(controller.status())
                    }.exceptionOrNull()
                    resultFailure = runCatching {
                        isolatedResult.deliverResult(DoctorUiResult(DoctorUiOperation.DISABLE, true, null))
                    }.exceptionOrNull()
                } finally {
                    throwing.set(false)
                }
            }
            isolated.detach()
            isolatedResult.detach()
            controller.setEnabled(false, sub)
            check(controller.awaitWriterIdleForTest(10000)) { "listener recovery writer idle timeout" }
            waitUntil("healthy listener recovery") { deliveredResults.get() > 0 }
            check(statusFailure == null && resultFailure == null) {
                "listener failure escaped: status=${statusFailure?.javaClass?.simpleName} " +
                    "result=${resultFailure?.javaClass?.simpleName}"
            }
            check(statusCalls.get() == 1 && resultCalls.get() == 1) {
                "both failure paths must run: status=${statusCalls.get()} result=${resultCalls.get()}"
            }
            val numeric = controller.numericSnapshot()
            check(
                numeric.enabled == (if (controller.status().enabled) 1 else 0) &&
                    numeric.collecting == (if (controller.status().collecting) 1 else 0) &&
                    numeric.interrupted == (if (controller.status().interrupted) 1 else 0) &&
                    numeric.epoch == controller.status().epoch &&
                    numeric.rawBytes == controller.status().rawBytes &&
                    numeric.tempBytes == controller.status().tempBytes &&
                    numeric.queueDepth >= 0L &&
                    numeric.dispatchOff.size == numeric.dispatchOn.size &&
                    numeric.hardwareOff.size == numeric.hardwareOn.size
            ) {
                "status/numeric snapshot disagreement: status=${controller.status()} numeric=$numeric"
            }
            val softActions = AtomicInteger()
            val voiceActions = AtomicInteger()
            val settingsActions = AtomicInteger()
            val actions = object : ImeActions {
                override fun onSoftKey(key: SoftKey) {
                    softActions.incrementAndGet()
                }
                override fun onCandidateTap(index: Int) = Unit
                override fun onOpenSettings() {
                    settingsActions.incrementAndGet()
                }
                override fun onVoice() {
                    voiceActions.incrementAndGet()
                }
                override fun onPunctuation(text: String) = Unit
                override fun onLanguage(language: Language) = Unit
                override fun onHide() = Unit
            }
            val changes = AtomicInteger()
            lateinit var statusView: TextView
            lateinit var imeView: ImeView
            inst.runOnMainSync {
                imeView = ImeView(activity, actions, controller)
                imeView.applySettings(KeyboardSettings(keyPreview = false, idleStrip = true))
                imeView.render(
                    UiState(
                        mode = InputMode.CHINESE,
                        composing = "",
                        candidates = emptyList(),
                        pageStart = 0,
                        homophoneOf = null,
                        tableLoaded = true,
                        failed = false,
                        language = Language.TRADITIONAL,
                        languages = listOf(Language.TRADITIONAL),
                        codeHint = null,
                        nextKeys = null,
                        fieldHasText = false,
                        zhuyinStage = ZhuyinStage.INACTIVE,
                        zhuyinText = "",
                        imeActive = true,
                        engineIsComposing = false,
                    ),
                )
                imeView.setKeyboardVisible(true)
                activity.addContentView(imeView, ViewGroup.LayoutParams(1080, 1000))
                statusView = views(imeView).filterIsInstance<TextView>().single { it.contentDescription == "doctor_status" }
                statusView.addTextChangedListener(object : TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                    override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                        changes.incrementAndGet()
                    }
                    override fun afterTextChanged(s: Editable?) = Unit
                })
            }
            fun label(): String {
                var result = ""
                inst.runOnMainSync { result = statusView.text.toString() }
                return result
            }
            fun contentDescriptionView(description: String): View? {
                var result: View? = null
                inst.runOnMainSync {
                    result = views(imeView).firstOrNull {
                        it.contentDescription?.toString() == description &&
                            it.width > 0 && it.height > 0 && it.isShown
                    }
                }
                return result
            }
            fun dispatchImeView(description: String, terminalAction: Int) {
                waitUntil("visible $description") { contentDescriptionView(description) != null }
                val touchHandled = CountDownLatch(1)
                inst.runOnMainSync {
                    val target = views(imeView).first {
                        it.contentDescription?.toString() == description &&
                            it.width > 0 && it.height > 0 && it.isShown
                    }
                    val targetLocation = IntArray(2)
                    val rootLocation = IntArray(2)
                    target.getLocationInWindow(targetLocation)
                    imeView.getLocationInWindow(rootLocation)
                    val x = (targetLocation[0] - rootLocation[0] + target.width / 2f)
                    val y = (targetLocation[1] - rootLocation[1] + target.height / 2f)
                    val downTime = SystemClock.uptimeMillis()
                    val down = MotionEvent.obtain(downTime, downTime, MotionEvent.ACTION_DOWN, x, y, 0)
                    val terminal = MotionEvent.obtain(
                        downTime,
                        downTime + 1,
                        terminalAction,
                        x,
                        y,
                        0,
                    )
                    try {
                        check(imeView.dispatchTouchEvent(down)) { "DOWN was not handled for $description" }
                        check(imeView.dispatchTouchEvent(terminal)) {
                            "terminal action=$terminalAction was not handled for $description"
                        }
                    } finally {
                        down.recycle()
                        terminal.recycle()
                    }
                    imeView.post { touchHandled.countDown() }
                }
                check(touchHandled.await(2, java.util.concurrent.TimeUnit.SECONDS)) {
                    "posted touch callback timed out for $description"
                }
            }
            fun tapImeView(description: String) {
                dispatchImeView(description, MotionEvent.ACTION_UP)
            }
            fun longPressImeView(description: String) {
                waitUntil("visible $description") { contentDescriptionView(description) != null }
                var downX = 0f
                var downY = 0f
                var downTime = 0L
                inst.runOnMainSync {
                    val target = views(imeView).first {
                        it.contentDescription?.toString() == description &&
                            it.width > 0 && it.height > 0 && it.isShown
                    }
                    val targetLocation = IntArray(2)
                    val rootLocation = IntArray(2)
                    target.getLocationInWindow(targetLocation)
                    imeView.getLocationInWindow(rootLocation)
                    downX = targetLocation[0] - rootLocation[0] + target.width / 2f
                    downY = targetLocation[1] - rootLocation[1] + target.height / 2f
                    downTime = SystemClock.uptimeMillis()
                    val down = MotionEvent.obtain(
                        downTime,
                        downTime,
                        MotionEvent.ACTION_DOWN,
                        downX,
                        downY,
                        0,
                    )
                    try {
                        check(imeView.dispatchTouchEvent(down)) {
                            "long-press DOWN was not handled for $description"
                        }
                    } finally {
                        down.recycle()
                    }
                }
                SystemClock.sleep(
                    ViewConfiguration.getLongPressTimeout().toLong() + 150L,
                )
                waitUntil("settings popup from $description") {
                    contentDescriptionView("key:popup:settings") != null
                }
                waitUntil("voice popup from $description") {
                    contentDescriptionView("key:popup:voice") != null
                }
                val released = CountDownLatch(1)
                inst.runOnMainSync {
                    val up = MotionEvent.obtain(
                        downTime,
                        SystemClock.uptimeMillis(),
                        MotionEvent.ACTION_UP,
                        downX,
                        downY,
                        0,
                    )
                    try {
                        check(imeView.dispatchTouchEvent(up)) {
                            "long-press UP was not handled for $description"
                        }
                    } finally {
                        up.recycle()
                    }
                    imeView.post { released.countDown() }
                }
                check(released.await(2, java.util.concurrent.TimeUnit.SECONDS)) {
                    "long-press release callback timed out for $description"
                }
                waitUntil("focused popup remains after $description") {
                    contentDescriptionView("key:popup:settings") != null &&
                        contentDescriptionView("key:popup:voice") != null
                }
            }
            fun popupClosed() =
                contentDescriptionView("key:popup:settings") == null &&
                    contentDescriptionView("key:popup:voice") == null
            fun checkSinglePopupKeyAction(
                label: String,
                cursor: Map<String, Int>,
            ) {
                val byOperation =
                    DoctorActionsDeviceProbe.doctorRecordsSince(activity, cursor)
                        .filter { it.eventCode == DoctorEventCode.TOUCH }
                        .groupBy { it.operationId }
                check(
                    byOperation.size == 1 &&
                        byOperation.all { (operationId, events) ->
                            operationId > 0L &&
                                events.count {
                                    it.callbackSource ==
                                        DoctorCallbackSource.KEY_ACTION
                                } == 1 &&
                                events.all {
                                    it.operationId == operationId &&
                                        it.gestureToken == operationId &&
                                        it.pointerCount != null &&
                                        it.pressed != null
                                }
                        }
                ) {
                    "$label did not persist one correlated KEY_ACTION: " +
                        byOperation.mapValues { (_, events) ->
                            events.map {
                                "${it.callbackSource}:${it.operationId}:" +
                                    "${it.gestureToken}:${it.pressed}"
                            }
                        }
                }
            }
            fun exerciseDoctorPopupAction(
                popupId: String,
                actionCount: AtomicInteger,
            ) {
                val openCursor = DoctorActionsDeviceProbe.doctorRecordCursor(activity)
                longPressImeView("key:,")
                check(controller.awaitWriterIdleForTest(10000)) {
                    "$popupId long-press writer idle timeout"
                }
                checkSinglePopupKeyAction("$popupId popup open", openCursor)
                val actionBefore = actionCount.get()
                val selectionCursor =
                    DoctorActionsDeviceProbe.doctorRecordCursor(activity)

                tapImeView("key:popup:$popupId")
                waitUntil("$popupId popup closed") { popupClosed() }
                check(controller.awaitWriterIdleForTest(10000)) {
                    "$popupId selection writer idle timeout"
                }

                check(actionCount.get() == actionBefore + 1) {
                    "$popupId action did not run exactly once"
                }
                checkSinglePopupKeyAction("$popupId popup selection", selectionCursor)
            }
            fun rapidImeViewDownUpDown(description: String, secondDescription: String) {
                waitUntil("visible $description") { contentDescriptionView(description) != null }
                waitUntil("visible $secondDescription") { contentDescriptionView(secondDescription) != null }
                val oldCleanupHandled = CountDownLatch(1)
                var x = 0f
                var y = 0f
                var secondX = 0f
                var secondY = 0f
                var secondDownTime = 0L
                inst.runOnMainSync {
                    val target = views(imeView).first {
                        it.contentDescription?.toString() == description &&
                            it.width > 0 && it.height > 0 && it.isShown
                    }
                    val targetLocation = IntArray(2)
                    val rootLocation = IntArray(2)
                    target.getLocationInWindow(targetLocation)
                    imeView.getLocationInWindow(rootLocation)
                    x = (targetLocation[0] - rootLocation[0] + target.width / 2f)
                    y = (targetLocation[1] - rootLocation[1] + target.height / 2f)
                    val secondTarget = views(imeView).first {
                        it.contentDescription?.toString() == secondDescription &&
                            it.width > 0 && it.height > 0 && it.isShown
                    }
                    secondTarget.getLocationInWindow(targetLocation)
                    secondX = (targetLocation[0] - rootLocation[0] + secondTarget.width / 2f)
                    secondY = (targetLocation[1] - rootLocation[1] + secondTarget.height / 2f)
                    val firstDownTime = SystemClock.uptimeMillis()
                    secondDownTime = firstDownTime + 2
                    val firstDown = MotionEvent.obtain(
                        firstDownTime, firstDownTime, MotionEvent.ACTION_DOWN, x, y, 0,
                    )
                    val firstUp = MotionEvent.obtain(
                        firstDownTime, firstDownTime + 1, MotionEvent.ACTION_UP, x, y, 0,
                    )
                    val secondDown = MotionEvent.obtain(
                        secondDownTime, secondDownTime, MotionEvent.ACTION_DOWN, secondX, secondY, 0,
                    )
                    try {
                        check(imeView.dispatchTouchEvent(firstDown))
                        check(imeView.dispatchTouchEvent(firstUp))
                        check(imeView.dispatchTouchEvent(secondDown))
                    } finally {
                        firstDown.recycle()
                        firstUp.recycle()
                        secondDown.recycle()
                    }
                    imeView.post { oldCleanupHandled.countDown() }
                }
                check(oldCleanupHandled.await(2, java.util.concurrent.TimeUnit.SECONDS)) {
                    "old touch cleanup timed out for $description"
                }
                val finalTouchHandled = CountDownLatch(1)
                inst.runOnMainSync {
                    val secondUp = MotionEvent.obtain(
                        secondDownTime,
                        secondDownTime + 1,
                        MotionEvent.ACTION_UP,
                        secondX,
                        secondY,
                        0,
                    )
                    try {
                        check(imeView.dispatchTouchEvent(secondUp))
                    } finally {
                        secondUp.recycle()
                    }
                    imeView.post { finalTouchHandled.countDown() }
                }
                check(finalTouchHandled.await(2, java.util.concurrent.TimeUnit.SECONDS)) {
                    "rapid touch callback timed out for $description"
                }
            }
            controller.setEnabled(false, sub)
            waitUntil("off") { !controller.status().enabled && label() == "Doctor 關閉" }
            val settingsBeforeOff = settingsActions.get()
            longPressImeView("key:,")
            tapImeView("key:popup:settings")
            waitUntil("OFF settings popup closed") { popupClosed() }
            check(settingsActions.get() == settingsBeforeOff + 1) {
                "SETTINGS popup action depended on Doctor"
            }
            val offChanges = changes.get()
            controller.setEnabled(true, sub)
            waitUntil("on") { controller.status().collecting && label() == "Doctor 收集中" }
            check(changes.get() > offChanges) { "ON label did not update" }
            val onChanges = changes.get()
            controller.setEnabled(false, sub)
            waitUntil("off-transition") { !controller.status().enabled && label() == "Doctor 關閉" }
            check(changes.get() > onChanges) { "OFF label did not update" }
            controller.setEnabled(true, sub)
            waitUntil("on-again") { controller.status().collecting && label() == "Doctor 收集中" }
            check(controller.awaitWriterIdleForTest(10000)) { "IME view ordinary idle timeout" }
            inst.runOnMainSync {
                controller.resetInstrumentationForTest()
                controller.updateInputState(
                    DoctorEditorState(android.text.InputType.TYPE_CLASS_TEXT, 0),
                    DoctorEventMode.LIU,
                    focus = true,
                )
            }
            val ordinaryRecordCursor =
                DoctorActionsDeviceProbe.doctorRecordCursor(activity)
            tapImeView("key:q")
            tapImeView("key:voice")
            check(controller.awaitWriterIdleForTest(10000)) { "IME view ordinary drain timeout" }
            val ordinaryView = controller.numericSnapshot()
            val ordinaryRecords =
                DoctorActionsDeviceProbe.doctorRecordsSince(
                    activity,
                    ordinaryRecordCursor,
                )
            check(softActions.get() == 1 && voiceActions.get() == 1) {
                "real ImeView actions missing: soft=${softActions.get()} voice=${voiceActions.get()}"
            }
            check(
                ordinaryView.operationsStarted == 2L &&
                    ordinaryView.dispatchOn.sum() == 4L &&
                    ordinaryView.queueDepth == 0L &&
                    ordinaryView.enqueued > 0L &&
                    ordinaryView.written > 0L &&
                    ordinaryView.commitAttempts == 0L
            ) {
                "ordinary ImeView touch chain mismatch: $ordinaryView"
            }
            val ordinaryTouchByOperation = ordinaryRecords
                .filter { it.eventCode == DoctorEventCode.TOUCH }
                .groupBy { it.operationId }
            // These taps hit key centers directly. SLOT_FORWARD is only expected
            // for a touch in the surrounding slot, not every ordinary key tap.
            check(ordinaryTouchByOperation.size == 2 && ordinaryTouchByOperation.all { (_, events) ->
                    events.any { it.callbackSource == DoctorCallbackSource.DISPATCH } &&
                        events.any { it.callbackSource == DoctorCallbackSource.KEY_ACTION }
                }) {
                "posted performClick lost its operation linkage: " +
                    ordinaryTouchByOperation.mapValues { (_, events) ->
                        events.map { "${it.callbackSource}:${it.operationId}:${it.gestureToken}" }
                    }
            }
            check(ordinaryTouchByOperation.all { (operationId, events) ->
                    operationId > 0L && events.all {
                        it.operationId == operationId &&
                            it.gestureToken == operationId &&
                            it.pointerCount != null &&
                            it.pressed != null
                    }
                }) {
                "posted performClick changed touch correlation: $ordinaryTouchByOperation"
            }

            exerciseDoctorPopupAction("settings", settingsActions)
            exerciseDoctorPopupAction("voice", voiceActions)

            check(controller.awaitWriterIdleForTest(10000)) { "IME view cancel idle timeout" }
            inst.runOnMainSync { controller.resetInstrumentationForTest() }
            val cancelRecordCursor =
                DoctorActionsDeviceProbe.doctorRecordCursor(activity)
            val actionsBeforeCancel = softActions.get() + voiceActions.get()
            dispatchImeView("key:q", MotionEvent.ACTION_CANCEL)
            check(controller.awaitWriterIdleForTest(10000)) { "IME view cancel drain timeout" }
            val cancelSnapshot = controller.numericSnapshot()
            val cancelRecords =
                DoctorActionsDeviceProbe.doctorRecordsSince(
                    activity,
                    cancelRecordCursor,
                )
                .filter { it.eventCode == DoctorEventCode.TOUCH }
            check(softActions.get() + voiceActions.get() == actionsBeforeCancel) {
                "ACTION_CANCEL invoked an input action"
            }
            check(cancelSnapshot.operationsStarted == 1L)
            check(
                cancelRecords.isNotEmpty() &&
                    cancelRecords.all { it.operationId > 0L && it.callbackSource != DoctorCallbackSource.KEY_ACTION }
            ) {
                "ACTION_CANCEL emitted invalid linkage: $cancelRecords"
            }

            controller.setEnabled(false, sub)
            waitUntil("rapid-off-control-disabled") { !controller.status().enabled }
            val offActionsBeforeRapid = softActions.get() + voiceActions.get()
            rapidImeViewDownUpDown("key:q", "key:voice")
            val offActionsAfterRapid = softActions.get() + voiceActions.get()
            Log.i("DoctorStatusDeviceProbe", "rapid-off before=$offActionsBeforeRapid " +
                "after=$offActionsAfterRapid delta=${offActionsAfterRapid - offActionsBeforeRapid}")
            check(offActionsAfterRapid == offActionsBeforeRapid + 2) {
                "rapid OFF control did not run both clicks: before=$offActionsBeforeRapid after=$offActionsAfterRapid"
            }
            controller.setEnabled(true, sub)
            waitUntil("rapid-on-control-enabled") { controller.status().collecting }
            check(controller.awaitWriterIdleForTest(10000)) { "IME view rapid-touch idle timeout" }
            inst.runOnMainSync { controller.resetInstrumentationForTest() }
            val rapidRecordCursor =
                DoctorActionsDeviceProbe.doctorRecordCursor(activity)
            val actionsBeforeRapid = softActions.get() + voiceActions.get()
            rapidImeViewDownUpDown("key:q", "key:voice")
            check(controller.awaitWriterIdleForTest(10000)) { "IME view rapid-touch drain timeout" }
            val rapidSnapshot = controller.numericSnapshot()
            val rapidOperations =
                DoctorActionsDeviceProbe.doctorRecordsSince(
                    activity,
                    rapidRecordCursor,
                )
                .filter { it.eventCode == DoctorEventCode.TOUCH }
                .groupBy { it.operationId }
            val actionsAfterRapid = softActions.get() + voiceActions.get()
            check(actionsAfterRapid == actionsBeforeRapid + 2) {
                "rapid touch did not run both posted clicks: before=$actionsBeforeRapid " +
                    "after=$actionsAfterRapid offDelta=${offActionsAfterRapid - offActionsBeforeRapid} " +
                    "operations=${rapidSnapshot.operationsStarted} queue=${rapidSnapshot.queueDepth} " +
                    "enqueued=${rapidSnapshot.enqueued} written=${rapidSnapshot.written} " +
                    "dropped=${rapidSnapshot.dropped} violations=${rapidSnapshot.producerViolations} " +
                    "records=${rapidOperations.mapValues { (_, events) -> events.map { "${it.callbackSource}:${it.operationId}:${it.gestureToken}:${it.pressed}" } }}"
            }
            check(rapidSnapshot.operationsStarted == 2L && rapidOperations.size == 2)
            check(rapidOperations.all { (operationId, events) ->
                    operationId > 0L &&
                        events.any { it.pressed == true } &&
                        events.any { it.pressed == false } &&
                        events.count { it.callbackSource == DoctorCallbackSource.KEY_ACTION } == 1
                }) {
                "old UP cleanup clobbered a new operation: $rapidOperations"
            }

            check(controller.awaitWriterIdleForTest(10000)) { "IME view restricted idle timeout" }
            inst.runOnMainSync {
                controller.resetInstrumentationForTest()
                controller.updateInputState(
                    DoctorEditorState(
                        android.text.InputType.TYPE_CLASS_TEXT or
                            android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD,
                        0,
                    ),
                    DoctorEventMode.NONE,
                    focus = true,
                )
            }
            val softBeforeRestricted = softActions.get()
            val voiceBeforeRestricted = voiceActions.get()
            tapImeView("key:q")
            tapImeView("key:voice")
            check(controller.awaitWriterIdleForTest(10000)) { "IME view restricted drain timeout" }
            val restrictedView = controller.numericSnapshot()
            check(
                softActions.get() == softBeforeRestricted + 1 &&
                    voiceActions.get() == voiceBeforeRestricted + 1
            ) {
                "restricted ImeView changed input behavior"
            }
            check(
                restrictedView.operationsStarted == 0L &&
                    restrictedView.dispatchOff.all { it == 0L } &&
                    restrictedView.dispatchOn.all { it == 0L } &&
                    restrictedView.commitAttempts == 0L &&
                    restrictedView.queueDepth == 0L
            ) {
                "restricted ImeView leaked per-key metrics: $restrictedView"
            }
            inst.runOnMainSync {
                controller.updateInputState(
                    DoctorEditorState(android.text.InputType.TYPE_CLASS_TEXT, 0),
                    DoctorEventMode.LIU,
                    focus = true,
                )
            }
            val before = controller.status().rawBytes
            waitUntil("initial status delivered") { deliveredBytes.get() == before }
            inst.runOnMainSync { changes.set(0); controller.recordInputStarted() }
            waitUntil("new status delivered") { deliveredBytes.get() > before }
            inst.runOnMainSync { }
            val observed = changes.get()
            Log.i("DoctorStatusDeviceProbe", "raw-before=$before raw-delivered=" + deliveredBytes.get() + " same-label-notifications=$observed")
            check(observed == 0) { "unchanged Doctor label reset $observed times after raw bytes changed" }
            check(controller.awaitWriterIdleForTest(10000)) { "status fault precondition idle timeout" }
            val activeRecord = checkNotNull(
                java.io.File(activity.filesDir, "doctor/records").listFiles()
                    ?.filter { it.isFile && it.name.startsWith("doctor-record-") }
                    ?.maxByOrNull { it.name },
            ) { "no Doctor record for interruption probe" }
            val originalMode = android.system.Os.stat(activeRecord.absolutePath).st_mode and 511
            try {
                android.system.Os.chmod(activeRecord.absolutePath, 256)
                inst.runOnMainSync { controller.recordInputStarted() }
                waitUntil("interrupted keyboard label") {
                    controller.status().interrupted &&
                        !controller.status().collecting &&
                        label() == "Doctor 中斷"
                }
            } finally {
                android.system.Os.chmod(activeRecord.absolutePath, originalMode)
            }
            controller.setEnabled(false, sub)
            waitUntil("interrupted collection disabled") {
                !controller.status().enabled && label() == "Doctor 關閉"
            }
            controller.setEnabled(true, sub)
            waitUntil("interrupted collection recovered") {
                controller.status().collecting && label() == "Doctor 收集中"
            }
            return "Doctor status stable text, weak listener collection, real ImeView " +
                "popup/actions, touch/privacy, and interruption recovery verified."
        } catch (failure: Throwable) {
            primaryFailure = failure
            throw failure
        } finally {
            var cleanupFailure: Throwable? = null
            fun recordCleanupFailure(failure: Throwable) {
                val existing = cleanupFailure
                if (existing == null) cleanupFailure = failure
                else existing.addSuppressed(failure)
            }
            try {
                controller.setEnabled(originalEnabled, sub)
                check(controller.awaitWriterIdleForTest(10000)) {
                    "Doctor consent restoration timeout"
                }
            } catch (failure: Throwable) {
                recordCleanupFailure(failure)
            }
            subscriptions.forEach { subscription ->
                try {
                    subscription.detach()
                } catch (failure: Throwable) {
                    recordCleanupFailure(failure)
                }
            }
            try {
                inst.runOnMainSync { activity.finish() }
            } catch (failure: Throwable) {
                recordCleanupFailure(failure)
            }
            val finalCleanupFailure = cleanupFailure
            if (finalCleanupFailure != null) {
                val primary = primaryFailure
                if (primary != null) primary.addSuppressed(finalCleanupFailure)
                else throw finalCleanupFailure
            }
        }
    }

    private fun subscribeWeakListener(controller: AndroidDoctorController): WeakListenerHandle {
        val queue = ReferenceQueue<DoctorRuntimeListener>()
        val statusCalls = AtomicInteger()
        val resultCalls = AtomicInteger()
        val listener: DoctorRuntimeListener = object : DoctorRuntimeListener {
            override fun onDoctorStatus(status: DoctorRuntimeStatus) {
                statusCalls.incrementAndGet()
            }

            override fun onDoctorResult(result: DoctorUiResult) {
                resultCalls.incrementAndGet()
            }
        }
        val reference = WeakReference(listener, queue)
        val subscription = controller.subscribe(listener)
        return WeakListenerHandle(
            subscription,
            reference,
            queue,
            statusCalls,
            resultCalls,
            listener,
        )
    }

    private fun awaitWeakListenerCollection(
        reference: WeakReference<DoctorRuntimeListener>,
        queue: ReferenceQueue<DoctorRuntimeListener>,
    ) {
        val deadline = SystemClock.uptimeMillis() + 2_000L
        while (SystemClock.uptimeMillis() < deadline) {
            System.gc()
            val remaining = (deadline - SystemClock.uptimeMillis()).coerceAtLeast(1L)
            val queued = queue.remove(remaining.coerceAtMost(100L))
            if (queued === reference) return
        }
        throw AssertionError(
            "weak Doctor listener was not reclaimed within 2000ms " +
                "(referentPresent=${reference.get() != null})",
        )
    }

    private class RecordingDoctorListener : DoctorRuntimeListener {
        val statusCalls = AtomicInteger()
        val resultCalls = AtomicInteger()
        val lastStatus = AtomicReference<DoctorRuntimeStatus?>()
        val lastResult = AtomicReference<DoctorUiResult?>()

        override fun onDoctorStatus(status: DoctorRuntimeStatus) {
            lastStatus.set(status)
            statusCalls.incrementAndGet()
        }

        override fun onDoctorResult(result: DoctorUiResult) {
            lastResult.set(result)
            resultCalls.incrementAndGet()
        }
    }

    private data class WeakListenerHandle(
        val subscription: DoctorRuntimeSubscription,
        val reference: WeakReference<DoctorRuntimeListener>,
        val queue: ReferenceQueue<DoctorRuntimeListener>,
        val statusCalls: AtomicInteger,
        val resultCalls: AtomicInteger,
        var retainedListener: DoctorRuntimeListener?,
    )
}
