package com.hamanpaul.liukai.ime

import android.accessibilityservice.AccessibilityServiceInfo
import android.app.Instrumentation
import android.content.Context
import android.content.Intent
import android.graphics.Rect
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import com.hamanpaul.liukai.core.doctor.DoctorEventCodec
import com.hamanpaul.liukai.core.doctor.DoctorCallbackSource
import com.hamanpaul.liukai.core.doctor.DoctorEventCode
import com.hamanpaul.liukai.core.doctor.DoctorEventPhase
import com.hamanpaul.liukai.core.doctor.DoctorEventRecord
import com.hamanpaul.liukai.data.ImePrefs
import com.hamanpaul.liukai.doctor.AndroidDoctorController
import com.hamanpaul.liukai.settings.SettingsActivity

/** Device-only actions exercise real SettingsActivity/controller; no production hooks. */
internal object DoctorActionsDeviceProbe {
    internal fun doctorRecordCursor(context: Context): Map<String, Int> =
        decodeDoctorRecords(context).mapValues { (_, events) -> events.size }

    internal fun doctorRecordsSince(
        context: Context,
        cursor: Map<String, Int>,
    ): List<DoctorEventRecord> =
        decodeDoctorRecords(context).flatMap { (name, events) ->
            events.drop(cursor[name] ?: 0)
        }

    private fun decodeDoctorRecords(
        context: Context,
    ): Map<String, List<DoctorEventRecord>> {
        val records = java.io.File(context.filesDir, "doctor/records")
            .listFiles()
            ?.filter { it.isFile && it.name.startsWith("doctor-record-") }
            ?.sortedBy { it.name }
            .orEmpty()
        return buildMap {
            records.forEach { file ->
                val decoded =
                    DoctorEventCodec.decodeJsonl(file.readText(Charsets.UTF_8))
                check(decoded.failure == null) {
                    "Doctor record decode failed: file=${file.name} " +
                        "failure=${decoded.failure} line=${decoded.failureLine}"
                }
                put(file.name, decoded.events.map { it.event })
            }
        }
    }

    fun run(inst: Instrumentation): String = run(inst, safLifetimeOnly = false)

    fun runSafLifetime(inst: Instrumentation): String = run(inst, safLifetimeOnly = true)

    private fun run(inst: Instrumentation, safLifetimeOnly: Boolean): String {
        fun stage(s: String) { Log.i("DoctorActionsDeviceProbe", "stage=$s") }
        fun waitUntil(label: String, timeout: Long = 10000, pred: () -> Boolean) {
            val end = SystemClock.uptimeMillis() + timeout
            while (SystemClock.uptimeMillis() < end) { if (pred()) return; SystemClock.sleep(100) }
            throw AssertionError("stage=$label timeout")
        }
        fun all(v: View): List<View> = listOf(v) + if (v is ViewGroup) (0 until v.childCount).flatMap { all(v.getChildAt(it)) } else emptyList()
        val monitor = inst.addMonitor(SettingsActivity::class.java.name, null, false)
        inst.targetContext.startActivity(Intent(inst.targetContext, SettingsActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        var activity = monitor.waitForActivityWithTimeout(10000) as? SettingsActivity ?: throw AssertionError("launch settings failed")
        inst.removeMonitor(monitor)
        val controller = AndroidDoctorController.get(activity)
        fun textPresent(text: String): Boolean {
            var found = false
            inst.runOnMainSync { found = all(activity.window.decorView).filterIsInstance<TextView>().any { it.text.toString().contains(text) } }
            return found
        }
        fun button(label: String) {
            waitUntil("button-$label") { var found=false; inst.runOnMainSync { found=all(activity.window.decorView).filterIsInstance<Button>().any { it.text.toString()==label && it.isEnabled } }; found }
            inst.runOnMainSync {
                val b = all(activity.window.decorView).filterIsInstance<Button>().first { it.text.toString()==label && it.isEnabled }
                b.requestRectangleOnScreen(Rect(0,0,b.width,b.height), true)
                check(b.performClick()) { "button click failed: $label" }
            }
        }

        fun nodes(n: AccessibilityNodeInfo): List<AccessibilityNodeInfo> = listOf(n) + (0 until n.childCount).mapNotNull { n.getChild(it) }.flatMap { nodes(it) }
        fun allWindowNodes(): List<AccessibilityNodeInfo> =
            inst.uiAutomation.windows.flatMap { window ->
                window.root?.let { nodes(it) } ?: emptyList()
            }
        fun shellText(command: String): String =
            android.os.ParcelFileDescriptor
                .AutoCloseInputStream(inst.uiAutomation.executeShellCommand(command))
                .bufferedReader()
                .use { it.readText() }
                .trim()
        fun keyboardNode(description: String): AccessibilityNodeInfo? =
            allWindowNodes().firstOrNull {
                it.contentDescription?.toString() == description &&
                    it.packageName?.toString() == inst.targetContext.packageName &&
                    it.isClickable && it.isVisibleToUser
            }
        fun clickKeyboard(description: String) {
            waitUntil("ime-key-$description") { keyboardNode(description) != null }
            val node = checkNotNull(keyboardNode(description))
            check(node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) {
                "IME key click failed: $description"
            }
        }
        fun touchKeyboard(description: String) {
            waitUntil("ime-touch-$description") { keyboardNode(description) != null }
            val node = checkNotNull(keyboardNode(description))
            val bounds = Rect()
            node.getBoundsInScreen(bounds)
            val x = bounds.exactCenterX()
            val y = bounds.exactCenterY()
            val downTime = SystemClock.uptimeMillis()
            val down = MotionEvent.obtain(
                downTime, downTime, MotionEvent.ACTION_DOWN, x, y, 0,
            )
            val up = MotionEvent.obtain(
                downTime, downTime + 1, MotionEvent.ACTION_UP, x, y, 0,
            )
            try {
                inst.sendPointerSync(down)
                inst.sendPointerSync(up)
            } finally {
                down.recycle()
                up.recycle()
            }
        }
        fun dialogButton(label: String) {
            val dialogStarted = SystemClock.elapsedRealtime()
            try {
                waitUntil("dialog-$label") {
                    val root=inst.uiAutomation.rootInActiveWindow ?: return@waitUntil false
                    nodes(root).any { it.text?.toString()==label && it.isClickable }
                }
            } catch (failure: AssertionError) {
                val root = inst.uiAutomation.rootInActiveWindow
                val fixedMarkers = root?.let { r -> nodes(r).mapNotNull { it.text?.toString() }
                    .filter { it == "診斷 ZIP 預覽" || it == "診斷操作未完成" || it == "沒有檔案存取權限" || it == "已取消匯出" } }
                Log.e("DoctorActionsDeviceProbe", "dialog-timeout=$label elapsed-ms=" +
                    (SystemClock.elapsedRealtime() - dialogStarted) + " package=" + root?.packageName +
                    " fixed-markers=$fixedMarkers status=" + controller.status() + " numeric=" + controller.numericSnapshot())
                if (label == "選擇儲存位置") {
                    val end = SystemClock.elapsedRealtime() + 60000
                    while (SystemClock.elapsedRealtime() < end) {
                        val heartbeat = java.util.concurrent.CountDownLatch(1)
                        android.os.Handler(android.os.Looper.getMainLooper()).post { heartbeat.countDown() }
                        val responsive = heartbeat.await(500, java.util.concurrent.TimeUnit.MILLISECONDS)
                        val current = inst.uiAutomation.rootInActiveWindow
                        val visible = current?.let { r -> nodes(r).any { it.text?.toString() == label && it.isClickable } } == true
                        Log.i("DoctorActionsDeviceProbe", "post-timeout elapsed-ms=" +
                            (SystemClock.elapsedRealtime() - dialogStarted) + " main-responsive=$responsive preview-visible=$visible status=" + controller.status())
                        if (visible) break
                        SystemClock.sleep(2000)
                    }
                }
                throw failure
            }
            val root=inst.uiAutomation.rootInActiveWindow ?: throw AssertionError("dialog missing")
            val node=nodes(root).first { it.text?.toString()==label && it.isClickable }
            check(node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "dialog click failed $label" }
        }
        val exportName = "liu-kai-doctor-device-test-${SystemClock.uptimeMillis()}.zip"
        var exportCreated = false
        fun systemPositive(
            packageName: String,
            kind: String?,
            fileName: String = exportName,
        ) {
            waitUntil("system-$packageName-$kind") {
                val root = inst.uiAutomation.rootInActiveWindow ?: return@waitUntil false
                val allNodes = nodes(root)
                root.packageName?.toString() == packageName &&
                    (kind == null || allNodes.any { it.className?.toString()?.contains(kind) == true }) &&
                    allNodes.any { it.viewIdResourceName == "android:id/button1" && it.isClickable }
            }
            var root = inst.uiAutomation.rootInActiveWindow ?: throw AssertionError("system dialog missing")
            var visible = nodes(root)
            if (packageName == "com.google.android.documentsui") {
                fun clickPickerNode(node: AccessibilityNodeInfo) {
                    var current: AccessibilityNodeInfo? = node
                    while (current != null && !current.isClickable) current = current.parent
                    check(current?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true) { "picker navigation click failed" }
                }
                val roots = visible.first { it.contentDescription?.toString() == "Show roots" }
                clickPickerNode(roots)
                waitUntil("picker-storage-root") {
                    inst.uiAutomation.rootInActiveWindow?.let { r -> nodes(r).any { it.text?.toString() == android.os.Build.MODEL && it.viewIdResourceName == "android:id/title" } } == true
                }
                root = inst.uiAutomation.rootInActiveWindow ?: throw AssertionError("picker roots missing")
                clickPickerNode(nodes(root).first { it.text?.toString() == android.os.Build.MODEL && it.viewIdResourceName == "android:id/title" })
                waitUntil("picker-download-folder") {
                    inst.uiAutomation.rootInActiveWindow?.let { r -> nodes(r).any { it.text?.toString() == "Download" && it.viewIdResourceName == "android:id/title" } } == true
                }
                root = inst.uiAutomation.rootInActiveWindow ?: throw AssertionError("storage root missing")
                clickPickerNode(nodes(root).first { it.text?.toString() == "Download" && it.viewIdResourceName == "android:id/title" })
                waitUntil("picker-download-open") {
                    inst.uiAutomation.rootInActiveWindow?.let { r -> nodes(r).any { it.text?.toString() == "Download" && it.viewIdResourceName == "com.google.android.documentsui:id/breadcrumb_text" } } == true
                }
                root = inst.uiAutomation.rootInActiveWindow ?: throw AssertionError("Download folder missing")
                visible = nodes(root)
                val name = visible.first { it.viewIdResourceName == "android:id/title" && it.className?.toString() == "android.widget.EditText" }
                val args = android.os.Bundle().apply {
                    putCharSequence(
                        AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,
                        fileName,
                    )
                }
                check(name.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)) { "cannot set test file name" }
            }
            val target=visible.first { it.viewIdResourceName == "android:id/button1" && it.isClickable }
            check(target.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "system button click failed" }
        }
        val uiAutomation = inst.uiAutomation
        val originalUiAutomationFlags = uiAutomation.serviceInfo.flags
        uiAutomation.serviceInfo = uiAutomation.serviceInfo.apply {
            flags = flags or AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
        }
        var restoreIdleStrip: (() -> Unit)? = null
        var originalAlwaysFinishActivities: String? = null
        var ownsSafPicker = false
        val staleResultExportName =
            "liu-kai-doctor-stale-ok-${SystemClock.uptimeMillis()}.zip"
        var staleResultUri: android.net.Uri? = null
        var actionFailure: Throwable? = null
        try {
            if (safLifetimeOnly) {
                stage("saf-lifetime-only")
                check(controller.awaitWriterIdleForTest(60000)) {
                    "SAF lifetime writer initialization timed out"
                }
                if (!controller.status().collecting) {
                    if (controller.status().enabled) {
                        button("關閉 Doctor 診斷")
                        waitUntil("saf-lifetime-disabled") {
                            !controller.status().enabled
                        }
                    }
                    button("開啟 Doctor 診斷")
                    dialogButton("開啟")
                    waitUntil("saf-lifetime-enabled") {
                        controller.status().collecting
                    }
                }
                button("標記剛才的問題")
                dialogButton("標記")
                waitUntil("saf-lifetime-marked") {
                    textPresent("已標記問題時間")
                }
                button("預覽與匯出")
                waitUntil("saf-lifetime-preview") {
                    inst.uiAutomation.rootInActiveWindow?.let { root ->
                        nodes(root).any { it.text?.toString() == "診斷 ZIP 預覽" }
                    } == true && controller.status().tempBytes > 0L
                }

                originalAlwaysFinishActivities =
                    shellText("settings get global always_finish_activities")
                check(
                    originalAlwaysFinishActivities == "null" ||
                        originalAlwaysFinishActivities == "0" ||
                        originalAlwaysFinishActivities == "1"
                ) {
                    "unexpected always_finish_activities value: " +
                        originalAlwaysFinishActivities
                }
                shellText("settings put global always_finish_activities 1")
                check(
                    shellText("settings get global always_finish_activities") == "1"
                ) { "could not enable always_finish_activities for fixture" }

                val destroyedActivity = activity
                val returnMonitor = inst.addMonitor(
                    SettingsActivity::class.java.name,
                    null,
                    false,
                )
                try {
                    dialogButton("選擇儲存位置")
                    ownsSafPicker = true
                    // Let Android save state while the picker backgrounds this
                    // Activity; it may already be destroyed when the click returns.
                    waitUntil("saf-lifetime-picker") {
                        inst.uiAutomation.rootInActiveWindow
                            ?.packageName
                            ?.toString() == "com.google.android.documentsui"
                    }
                    waitUntil("saf-lifetime-destroyed") {
                        destroyedActivity.isDestroyed
                    }
                    check(controller.awaitWriterIdleForTest(10000)) {
                        "SAF lifetime destroy writer idle timeout"
                    }
                    val tempAfterDestroy = controller.status().tempBytes
                    check(tempAfterDestroy > 0L) {
                        "Activity destruction invalidated SAF preview early: " +
                            "tempBytes=$tempAfterDestroy " +
                            "epoch=${controller.status().epoch}"
                    }
                    Log.i(
                        "DoctorActionsDeviceProbe",
                        "saf-lifetime-picker=true activity-destroyed=true " +
                            "temp-before-return=$tempAfterDestroy " +
                            "epoch=${controller.status().epoch}",
                    )
                    fun pickerBreadcrumbCount(): Int {
                        val root = inst.uiAutomation.rootInActiveWindow ?: return 0
                        if (root.packageName?.toString() != "com.google.android.documentsui") return 0
                        return nodes(root).count {
                            it.viewIdResourceName == "com.google.android.documentsui:id/breadcrumb_text"
                        }
                    }
                    var depth = pickerBreadcrumbCount()
                    check(depth > 0) { "SAF picker breadcrumb is not visible" }
                    var navigationSteps = 0
                    while (depth > 1) {
                        check(navigationSteps++ < 10) { "SAF picker did not reach root" }
                        check(inst.uiAutomation.performGlobalAction(
                            android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                        ))
                        val previousDepth = depth
                        waitUntil("saf-lifetime-breadcrumb-pop") {
                            val current = pickerBreadcrumbCount()
                            current in 1 until previousDepth
                        }
                        depth = pickerBreadcrumbCount()
                        Log.i("DoctorActionsDeviceProbe", "saf-lifetime-breadcrumb-count=$depth")
                    }
                    check(pickerBreadcrumbCount() == 1) { "SAF picker root is not visible" }
                    check(inst.uiAutomation.performGlobalAction(
                        android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                    ))
                    ownsSafPicker = false
                    activity = returnMonitor.waitForActivityWithTimeout(10000)
                        as? SettingsActivity
                        ?: throw AssertionError(
                            "SAF lifetime SettingsActivity recreation timeout",
                        )
                    check(activity !== destroyedActivity) {
                        "SAF result reused the destroyed SettingsActivity"
                    }
                } finally {
                    inst.removeMonitor(returnMonitor)
                }
                waitUntil("saf-lifetime-cancel-result") {
                    // A missing restored handle has a different cancellation message.
                    textPresent("已取消匯出") &&
                        controller.status().tempBytes == 0L
                }
                return "Doctor SAF non-config destroy/cancel lifetime verified."
            }
            stage("initial-off")
            val initStart = SystemClock.elapsedRealtime()
            val initBefore = controller.status()
            val initCompleted = controller.awaitWriterIdleForTest(60000)
            Log.i("DoctorActionsDeviceProbe", "initialization-wait-ms=" +
                (SystemClock.elapsedRealtime() - initStart) + " completed=$initCompleted before=$initBefore after=" + controller.status())
            check(initCompleted) { "writer initialization timeout" }
            if (controller.status().enabled) { button("關閉 Doctor 診斷"); waitUntil("disable") { !controller.status().enabled } }
            stage("quiet-off-numeric")
            check(controller.awaitWriterIdleForTest(10000)) { "quiet-off writer idle timeout" }
            inst.runOnMainSync {
                controller.resetInstrumentationForTest()
                controller.setPerformanceSamplingForTest(false)
                controller.updateInputState(
                    com.hamanpaul.liukai.core.doctor.DoctorEditorState(
                        android.text.InputType.TYPE_CLASS_TEXT,
                        0,
                    ),
                    com.hamanpaul.liukai.core.doctor.DoctorEventMode.LIU,
                    true,
                )
                controller.recordCommit(false, 1L)
                controller.recordDispatch(SystemClock.elapsedRealtimeNanos())
                controller.recordHardware(SystemClock.elapsedRealtimeNanos())
                controller.updateInputState(null, com.hamanpaul.liukai.core.doctor.DoctorEventMode.NONE, false)
            }
            val quietOff = controller.numericSnapshot()
            check(
                quietOff.enabled == 0 &&
                    quietOff.queueDepth == 0L &&
                    quietOff.operationsStarted == 0L &&
                    quietOff.enqueued == 0L &&
                    quietOff.written == 0L &&
                    quietOff.dispatchOff.sum() == 0L &&
                    quietOff.dispatchOn.sum() == 0L &&
                    quietOff.hardwareOff.sum() == 0L &&
                    quietOff.hardwareOn.sum() == 0L &&
                    quietOff.commitAttempts == 0L &&
                    quietOff.commitSuccesses == 0L
            ) {
                "off ordinary callbacks leaked into daily metrics: $quietOff"
            }
            stage("preview-without-mark")
            button("預覽與匯出"); waitUntil("preview-no-mark-error") { textPresent("診斷操作未完成") }
            stage("consent-cancel")
            button("開啟 Doctor 診斷"); dialogButton("取消"); check(!controller.status().enabled)
            stage("enable")
            button("開啟 Doctor 診斷"); dialogButton("開啟"); waitUntil("enabled") { controller.status().enabled }
            stage("real-ime-service-chain")
            check(controller.awaitWriterIdleForTest(10000)) { "service-chain precondition idle timeout" }
            val beforeService = controller.numericSnapshot()
            lateinit var tryArea: EditText
            val keyboardPrefs = activity.getSharedPreferences("liukai", Context.MODE_PRIVATE)
            val hadIdleStrip = keyboardPrefs.contains(ImePrefs.IDLE_STRIP)
            val originalIdleStrip = keyboardPrefs.getString(ImePrefs.IDLE_STRIP, null)
            restoreIdleStrip = {
                keyboardPrefs.edit().let { edit ->
                    if (hadIdleStrip) {
                        edit.putString(ImePrefs.IDLE_STRIP, originalIdleStrip)
                    } else {
                        edit.remove(ImePrefs.IDLE_STRIP)
                    }
                    check(edit.commit()) { "could not restore idle_strip preference" }
                }
            }
            ImePrefs.set(activity, ImePrefs.IDLE_STRIP, "true")
            inst.runOnMainSync {
                tryArea = all(activity.window.decorView)
                    .filterIsInstance<EditText>()
                    .single { it.contentDescription?.toString() == "try_area" }
                tryArea.setText("")
                tryArea.requestFocus()
            }
            val imm = activity.getSystemService(InputMethodManager::class.java)
            // requestFocus returns before ViewRoot has established its served input connection.
            waitUntil("initial-service-field-served") {
                var served = false
                inst.runOnMainSync {
                    served = tryArea.isAttachedToWindow && tryArea.hasWindowFocus() &&
                        tryArea.hasFocus() && imm.isActive(tryArea)
                }
                served
            }
            var showAccepted = false
            inst.runOnMainSync {
                showAccepted = imm.showSoftInput(tryArea, InputMethodManager.SHOW_IMPLICIT)
            }
            check(showAccepted) {
                "IME did not accept the real try-area field after its served connection was ready"
            }
            try {
                waitUntil("initial-service-letter-mode") {
                    keyboardNode("key:homophone") != null || keyboardNode("key:shift") != null
                }
            } catch (failure: AssertionError) {
                try {
                    var focusDiagnostic = ""
                    inst.runOnMainSync {
                        focusDiagnostic = "finishing=${activity.isFinishing} destroyed=${activity.isDestroyed} " +
                            "decorAttached=${activity.window.decorView.isAttachedToWindow} " +
                            "windowFocus=${activity.hasWindowFocus()} fieldFocused=${tryArea.hasFocus()} " +
                            "fieldWindowFocus=${tryArea.hasWindowFocus()} fieldAttached=${tryArea.isAttachedToWindow}"
                    }
                    val system = shellText("dumpsys input_method")
                    val systemDiagnostic = "curLiuKai=${system.contains("mCurMethodId=com.hamanpaul.liukai/.ime.LiuKaiImeService")} " +
                        "bound=${system.contains("mBoundToMethod=true")} " +
                        "visibleBound=${system.contains("mVisibleBound=true")} " +
                        "inputShown=${system.contains("mInputShown=true")} " +
                        "methodNull=${system.contains("mCurMethod=null")}"
                    Log.e("DoctorActionsDeviceProbe", "initial-ime-timeout $focusDiagnostic $systemDiagnostic " +
                        "automationFlags=${inst.uiAutomation.serviceInfo.flags}")
                } catch (diagnosticFailure: Exception) {
                    Log.e("DoctorActionsDeviceProbe", "initial-ime-diagnostic-error=${diagnosticFailure.javaClass.simpleName}")
                }
                throw failure
            }
            if (keyboardNode("key:homophone") != null) {
                clickKeyboard("key:toggle_english")
            }
            waitUntil("english-soft-key") { keyboardNode("key:a") != null }
            waitUntil("english-mode-marker") { keyboardNode("key:shift") != null }
            clickKeyboard("key:a")
            var literal = ""
            waitUntil("literal-committed") {
                inst.runOnMainSync { literal = tryArea.text.toString() }
                literal == "a" || literal == "A"
            }
            if (keyboardNode("key:shift") != null) {
                clickKeyboard("key:toggle_english")
            }
            waitUntil("chinese-soft-key") { keyboardNode("key:homophone") != null }
            inst.sendKeyDownUpSync(KeyEvent.KEYCODE_Q)
            var candidateText = ""
            waitUntil("hardware-candidate") {
                val candidate = allWindowNodes().firstOrNull {
                    it.isClickable && it.isVisibleToUser &&
                        it.packageName?.toString() == inst.targetContext.packageName &&
                        it.contentDescription?.toString()?.startsWith("cand:") == true
                }
                if (candidate == null) return@waitUntil false
                candidateText = candidate.contentDescription.toString()
                    .removePrefix("cand:")
                    .substringAfter(':')
                check(candidate.performAction(AccessibilityNodeInfo.ACTION_CLICK)) {
                    "real candidate click failed"
                }
                true
            }
            var finalServiceText = ""
            waitUntil("service-chain-field") {
                inst.runOnMainSync { finalServiceText = tryArea.text.toString() }
                finalServiceText == literal + candidateText
            }
            check(controller.awaitWriterIdleForTest(10000)) {
                "service-chain pre-punctuation idle timeout"
            }
            val punctuationTouchCursor = doctorRecordCursor(activity)
            val beforePunctuationTouch = finalServiceText
            touchKeyboard("key:punct:!")
            waitUntil("punctuation-touch-field") {
                inst.runOnMainSync { finalServiceText = tryArea.text.toString() }
                finalServiceText == beforePunctuationTouch + "!"
            }
            check(controller.awaitWriterIdleForTest(10000)) {
                "punctuation touch writer idle timeout"
            }
            val punctuationTouchRecords =
                doctorRecordsSince(activity, punctuationTouchCursor)
            val punctuationTouchCommit = punctuationTouchRecords.singleOrNull {
                it.eventCode == DoctorEventCode.EDIT_APPLIED &&
                    it.phase == DoctorEventPhase.COMMIT
            }
            check(punctuationTouchCommit != null && punctuationTouchCommit.operationId > 0L) {
                "punctuation touch lost commit correlation: $punctuationTouchRecords"
            }
            val punctuationTouchOperation = punctuationTouchCommit.operationId
            check(
                punctuationTouchRecords.any {
                    it.eventCode == DoctorEventCode.TOUCH &&
                        it.operationId == punctuationTouchOperation &&
                        it.callbackSource == DoctorCallbackSource.DISPATCH
                } &&
                    punctuationTouchRecords.any {
                        it.eventCode == DoctorEventCode.TOUCH &&
                            it.operationId == punctuationTouchOperation &&
                            it.callbackSource == DoctorCallbackSource.KEY_ACTION
                    }
            ) {
                "punctuation touch was not linked to its real input operation: " +
                    punctuationTouchRecords
            }

            val punctuationAccessibilityCursor = doctorRecordCursor(activity)
            val beforePunctuationAccessibility = finalServiceText
            clickKeyboard("key:punct:?")
            waitUntil("punctuation-accessibility-field") {
                inst.runOnMainSync { finalServiceText = tryArea.text.toString() }
                finalServiceText == beforePunctuationAccessibility + "?"
            }
            check(controller.awaitWriterIdleForTest(10000)) {
                "punctuation accessibility writer idle timeout"
            }
            val punctuationAccessibilityCommit =
                doctorRecordsSince(activity, punctuationAccessibilityCursor).singleOrNull {
                    it.eventCode == DoctorEventCode.EDIT_APPLIED &&
                        it.phase == DoctorEventPhase.COMMIT
                }
            check(
                punctuationAccessibilityCommit != null &&
                    punctuationAccessibilityCommit.operationId > 0L &&
                    punctuationAccessibilityCommit.operationId != punctuationTouchOperation
            ) {
                "accessibility punctuation did not receive a new operation: " +
                    "$punctuationAccessibilityCommit"
            }
            check(controller.awaitWriterIdleForTest(10000)) { "service-chain writer idle timeout" }
            val afterService = controller.numericSnapshot()
            check(
                afterService.commitAttempts - beforeService.commitAttempts >= 2L &&
                    afterService.commitSuccesses - beforeService.commitSuccesses >= 2L &&
                    afterService.operationsStarted > beforeService.operationsStarted &&
                    afterService.written > beforeService.written &&
                    afterService.queueDepth == 0L
            ) {
                "real IME service Doctor chain mismatch: field=$finalServiceText " +
                    "before=$beforeService after=$afterService"
            }
            inst.runOnMainSync {
                activity.getSystemService(InputMethodManager::class.java)
                    .hideSoftInputFromWindow(tryArea.windowToken, 0)
            }
            waitUntil("real-ime-hidden") { keyboardNode("key:toggle_english") == null }
            restoreIdleStrip?.invoke()
            restoreIdleStrip = null
            stage("public-callback-privacy")
            inst.runOnMainSync {
                val ordinary=com.hamanpaul.liukai.core.doctor.DoctorEditorState(android.text.InputType.TYPE_CLASS_TEXT,0)
                val password=com.hamanpaul.liukai.core.doctor.DoctorEditorState(android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD,0)
                val privacyBaseline = controller.numericSnapshot()
                // Counters were reset while disabled and idle above. Resetting
                // again here can race writes already queued during Enable.
                controller.setPerformanceSamplingForTest(true)
                controller.updateInputState(ordinary,com.hamanpaul.liukai.core.doctor.DoctorEventMode.LIU,true)
                controller.recordInputStarted();val first=controller.beginOperation();check(first>0)
                check(controller.beginTouchChain(first,first,1,true))
                controller.withOperation(first,first,1,true) {
                    controller.recordTouch(first,first,1,true,com.hamanpaul.liukai.core.doctor.DoctorCallbackSource.KEY_ACTION,true,1)
                    controller.recordComposition(true,1);controller.recordCandidate(false,1);controller.recordCommit(true,1)
                    val rejecting=object : android.view.inputmethod.BaseInputConnection(View(activity),true) {
                        override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean = false
                    }
                    check(!commitTextWithDoctor(rejecting,controller,"x")) { "rejected commit was reported successful" }
                    controller.recordDispatch(SystemClock.elapsedRealtimeNanos());controller.recordHardware(SystemClock.elapsedRealtimeNanos())
                }
                controller.finishTouchChain(true,1)
                val privacyAfter = controller.numericSnapshot()
                check(
                    privacyAfter.commitAttempts - privacyBaseline.commitAttempts == 2L &&
                        privacyAfter.commitSuccesses - privacyBaseline.commitSuccesses == 1L &&
                        privacyAfter.lastCommitCommitted == 0
                ) {
                    "failed commit metrics were incorrect: " +
                        "baseline(commitAttempts=${privacyBaseline.commitAttempts}," +
                        "commitSuccesses=${privacyBaseline.commitSuccesses}," +
                        "epoch=${privacyBaseline.epoch},queueDepth=${privacyBaseline.queueDepth}," +
                        "enqueued=${privacyBaseline.enqueued},written=${privacyBaseline.written}," +
                        "writtenBytes=${privacyBaseline.writtenBytes}," +
                        "dropped=${privacyBaseline.dropped}) " +
                        "after(commitAttempts=${privacyAfter.commitAttempts}," +
                        "commitSuccesses=${privacyAfter.commitSuccesses}," +
                        "lastCommitCommitted=${privacyAfter.lastCommitCommitted}," +
                        "epoch=${privacyAfter.epoch},queueDepth=${privacyAfter.queueDepth}," +
                        "enqueued=${privacyAfter.enqueued},written=${privacyAfter.written}," +
                        "writtenBytes=${privacyAfter.writtenBytes}," +
                        "dropped=${privacyAfter.dropped})"
                }
                controller.updateInputState(password,com.hamanpaul.liukai.core.doctor.DoctorEventMode.NONE,true)
                check(controller.beginOperation()==0L)
                controller.recordTouch(first,first,1,true,com.hamanpaul.liukai.core.doctor.DoctorCallbackSource.DISPATCH,true,1)
                controller.recordCommit(true,1);controller.recordDispatch(SystemClock.elapsedRealtimeNanos());controller.recordHardware(SystemClock.elapsedRealtimeNanos())
                controller.updateInputState(null,com.hamanpaul.liukai.core.doctor.DoctorEventMode.NONE,false);check(controller.beginOperation()==0L)
                val restricted=controller.numericSnapshot()
                check(restricted.operationsStarted==privacyAfter.operationsStarted && restricted.enqueued==privacyAfter.enqueued && restricted.commitAttempts==privacyAfter.commitAttempts)
                check(restricted.dispatchOn==privacyAfter.dispatchOn && restricted.hardwareOn==privacyAfter.hardwareOn)
                controller.updateInputState(ordinary,com.hamanpaul.liukai.core.doctor.DoctorEventMode.ENGLISH,true)
                val after=controller.beginOperation();check(after==first+1) { "restricted callbacks allocated operation IDs" }
                controller.updateMode(com.hamanpaul.liukai.core.doctor.DoctorEventMode.LIU);controller.recordInputFinished()
                controller.setPerformanceSamplingForTest(false);controller.updateInputState(null,com.hamanpaul.liukai.core.doctor.DoctorEventMode.NONE,false)
            }
            check(controller.awaitWriterIdleForTest(10000)) { "callback metrics writer idle timeout" }
            // The periodic health writer can advance between two reads. Require
            // the same invariants at a quiescent cut rather than one racy read.
            var callbackMetricsDiagnostic = "not sampled"
            try {
                waitUntil("quiescent-callback-metrics") {
                    val numericAfterCallbacks = controller.numericSnapshot()
                    val statusAfterCallbacks = controller.status()
                    val checks = linkedMapOf(
                        "enabled" to (numericAfterCallbacks.enabled == 1),
                        "collecting" to (numericAfterCallbacks.collecting == 1),
                        "notInterrupted" to (numericAfterCallbacks.interrupted == 0),
                        "epochMatches" to (numericAfterCallbacks.epoch == statusAfterCallbacks.epoch),
                        "queueEmpty" to (numericAfterCallbacks.queueDepth == 0L),
                        "rawMatches" to (numericAfterCallbacks.rawBytes == statusAfterCallbacks.rawBytes),
                        "tempMatches" to (numericAfterCallbacks.tempBytes == statusAfterCallbacks.tempBytes),
                        "writtenPositive" to (numericAfterCallbacks.written > 0L),
                        "writtenMatchesEnqueued" to (numericAfterCallbacks.written == numericAfterCallbacks.enqueued),
                        "droppedMatches" to (numericAfterCallbacks.dropped == statusAfterCallbacks.dropped),
                        "writtenBytesPositive" to (numericAfterCallbacks.writtenBytes > 0L),
                    )
                    callbackMetricsDiagnostic = "checks=$checks numeric=" +
                        "enabled:${numericAfterCallbacks.enabled},collecting:${numericAfterCallbacks.collecting},interrupted:${numericAfterCallbacks.interrupted}," +
                        "epoch:${numericAfterCallbacks.epoch},queue:${numericAfterCallbacks.queueDepth},raw:${numericAfterCallbacks.rawBytes}," +
                        "temp:${numericAfterCallbacks.tempBytes},enqueued:${numericAfterCallbacks.enqueued},written:${numericAfterCallbacks.written}," +
                        "dropped:${numericAfterCallbacks.dropped},writtenBytes:${numericAfterCallbacks.writtenBytes} " +
                        "status=epoch:${statusAfterCallbacks.epoch},raw:${statusAfterCallbacks.rawBytes},temp:${statusAfterCallbacks.tempBytes},dropped:${statusAfterCallbacks.dropped}"
                    checks.values.all { it }
                }
            } catch (failure: AssertionError) {
                throw AssertionError("${failure.message}; $callbackMetricsDiagnostic", failure)
            }
            stage("mark-recent")
            button("標記剛才的問題"); dialogButton("標記"); waitUntil("mark-result") { textPresent("已標記問題時間") }
            stage("mark-invalid")
            button("標記剛才的問題")
            fun setMarkField(description: String, value: String) {
                waitUntil("mark-entry") { inst.uiAutomation.rootInActiveWindow?.let { r -> nodes(r).any { it.contentDescription?.toString()==description } } == true }
                val root=inst.uiAutomation.rootInActiveWindow ?: throw AssertionError("mark dialog missing")
                val entry=nodes(root).first { it.contentDescription?.toString()==description }
                val args=android.os.Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,value) }
                check(entry.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT,args))
            }
            setMarkField("問題前幾分鐘", ""); dialogButton("標記")
            waitUntil("invalid-mark-message") { inst.uiAutomation.rootInActiveWindow?.let { r -> nodes(r).any { it.text?.toString()=="請輸入有效的非負時間。" } } == true }
            setMarkField("問題前幾分鐘", "5"); setMarkField("問題後幾秒", ""); dialogButton("標記")
            waitUntil("invalid-after-message") { inst.uiAutomation.rootInActiveWindow?.let { r -> nodes(r).any { it.text?.toString()=="請輸入有效的非負時間。" } } == true }
            setMarkField("問題後幾秒", "30"); dialogButton("標記"); waitUntil("valid-mark") { textPresent("已標記問題時間") }
            stage("mark-specific")
            button("標記剛才的問題"); dialogButton("改選問題時間"); systemPositive(inst.targetContext.packageName, "DatePicker"); systemPositive(inst.targetContext.packageName, "TimePicker"); dialogButton("標記"); waitUntil("specific-mark") { textPresent("已標記問題時間") }
            stage("preview-recreate-cancel")
            button("預覽與匯出")
            waitUntil("preview-open") { inst.uiAutomation.rootInActiveWindow?.let { root -> nodes(root).any { it.text?.toString()=="診斷 ZIP 預覽" } } == true }
            val recreateMonitor=inst.addMonitor(SettingsActivity::class.java.name, null, false)
            inst.runOnMainSync { activity.recreate() }
            val recreated=recreateMonitor.waitForActivityWithTimeout(10000) as? SettingsActivity ?: throw AssertionError("recreate activity timeout")
            inst.removeMonitor(recreateMonitor); activity=recreated
            dialogButton("取消"); waitUntil("preview-cancel") { controller.awaitWriterIdleForTest(1000) }
            waitUntil("preview-cancel-visible-and-clean") { textPresent("已取消匯出") && controller.status().tempBytes == 0L }
            stage("preview-back-cancel")
            button("預覽與匯出")
            waitUntil("back-preview-open") { inst.uiAutomation.rootInActiveWindow?.let { root -> nodes(root).any { it.text?.toString()=="診斷 ZIP 預覽" } } == true }
            waitUntil("back-preview-window-focused") {
                inst.uiAutomation.windows.any { window ->
                    window.isFocused && window.root?.let { root ->
                        nodes(root).any { it.text?.toString() == "診斷 ZIP 預覽" }
                    } == true
                }
            }
            check(inst.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK))
            try {
                waitUntil("back-cancel-visible-and-clean") {
                    textPresent("已取消匯出") && controller.status().tempBytes == 0L
                }
            } catch (failure: AssertionError) {
                val status = controller.status()
                val numeric = controller.numericSnapshot()
                val knownMarkers = listOf(
                    "診斷 ZIP 預覽",
                    "已取消匯出",
                    "診斷操作未完成",
                    "預覽已失效，請重新建立預覽。",
                    "沒有檔案存取權限",
                )
                val visibleMarkers = inst.uiAutomation.rootInActiveWindow?.let { root ->
                    val visibleText = nodes(root).mapNotNull { it.text?.toString() }.toSet()
                    knownMarkers.filter(visibleText::contains)
                }.orEmpty()
                Log.e(
                    "DoctorActionsDeviceProbe",
                    "back-cancel-timeout package=${inst.uiAutomation.rootInActiveWindow?.packageName} " +
                        "markers=$visibleMarkers enabled=${status.enabled} " +
                        "collecting=${status.collecting} interrupted=${status.interrupted} " +
                        "pendingClear=${status.pendingClear} epoch=${status.epoch} " +
                        "rawBytes=${status.rawBytes} tempBytes=${status.tempBytes} " +
                        "sequenceGap=${status.sequenceGap} dropped=${status.dropped} " +
                        "queueDepth=${numeric.queueDepth} enqueued=${numeric.enqueued} " +
                        "written=${numeric.written}",
                )
                throw failure
            }
            stage("preview-finish-destroy-cancel")
            button("預覽與匯出")
            waitUntil("finish-preview-open") {
                inst.uiAutomation.rootInActiveWindow?.let { root ->
                    nodes(root).any { it.text?.toString() == "診斷 ZIP 預覽" }
                } == true
            }
            val savedBeforeFinish = android.os.Bundle()
            inst.runOnMainSync {
                inst.callActivityOnSaveInstanceState(activity, savedBeforeFinish)
            }
            val savedDoctor = checkNotNull(savedBeforeFinish.getBundle("doctor-export")) {
                "prepared preview was not saved by SettingsActivity"
            }
            val destroyedHandle = com.hamanpaul.liukai.doctor.DoctorExportHandle(
                savedDoctor.getLong("token"),
                savedDoctor.getLong("epoch"),
                savedDoctor.getLong("expires"),
            )
            inst.runOnMainSync { activity.finish() }
            check(controller.awaitWriterIdleForTest(10000)) { "destroy cancel writer idle timeout" }
            waitUntil("destroy-cancel-clean") { controller.status().tempBytes == 0L }
            val destroyResults = java.util.concurrent.LinkedBlockingQueue<com.hamanpaul.liukai.doctor.DoctorUiResult>()
            val destroyListener = object : com.hamanpaul.liukai.doctor.DoctorRuntimeListener {
                override fun onDoctorStatus(status: com.hamanpaul.liukai.doctor.DoctorRuntimeStatus) = Unit
                override fun onDoctorResult(result: com.hamanpaul.liukai.doctor.DoctorUiResult) {
                    destroyResults.add(result)
                }
            }

            val destroySubscription = controller.subscribe(destroyListener)
            try {
                controller.cancelExport(destroyedHandle, destroySubscription)
                val destroyedCancel = destroyResults.poll(10, java.util.concurrent.TimeUnit.SECONDS)
                    ?: throw AssertionError("destroyed preview cancel result timeout")
                check(
                    destroyedCancel.operation ==
                        com.hamanpaul.liukai.doctor.DoctorUiOperation.CANCEL_EXPORT &&
                        !destroyedCancel.success &&
                        destroyedCancel.failure ==
                        com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.INVALID_SNAPSHOT
                ) { "destroyed preview remained live: $destroyedCancel" }
            } finally {
                destroySubscription.detach()
            }
            val destroyRelaunchMonitor = inst.addMonitor(SettingsActivity::class.java.name, null, false)
            inst.targetContext.startActivity(
                Intent(inst.targetContext, SettingsActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            )
            activity = destroyRelaunchMonitor.waitForActivityWithTimeout(10000) as? SettingsActivity
                ?: throw AssertionError("settings relaunch after destroy timeout")
            inst.removeMonitor(destroyRelaunchMonitor)
            stage("preview-create-permission-failure")
            val protectedTemp=java.io.File(activity.filesDir, "doctor/temp")
            val protectedMode=android.system.Os.stat(protectedTemp.absolutePath).st_mode and 511
            try {
                android.system.Os.chmod(protectedTemp.absolutePath,320)
                button("預覽與匯出")
                waitUntil("preview-create-denied-visible") { textPresent("沒有檔案存取權限") }
                check(controller.status().tempBytes == 0L)
                Log.i("DoctorActionsDeviceProbe", "preview-create-failure=PERMISSION_DENIED temp-empty=true")
            } finally { android.system.Os.chmod(protectedTemp.absolutePath,protectedMode) }
            stage("preview-cancel-permission-failure")
            button("預覽與匯出")
            waitUntil("fault-preview-open") { inst.uiAutomation.rootInActiveWindow?.let { root -> nodes(root).any { it.text?.toString()=="診斷 ZIP 預覽" } } == true }
            val tempDir=java.io.File(activity.filesDir, "doctor/temp")
            val beforeCancel=tempDir.listFiles()?.filter { it.isFile } ?: emptyList()
            check(beforeCancel.isNotEmpty()) { "prepared export missing before fault" }
            val tempMode=android.system.Os.stat(tempDir.absolutePath).st_mode and 511
            try {
                android.system.Os.chmod(tempDir.absolutePath,320)
                dialogButton("取消")
                waitUntil("cancel-denied-visible") { textPresent("沒有檔案存取權限") }
                check(beforeCancel.any { it.exists() }) { "cancel unexpectedly removed protected export" }
                check(!textPresent("已取消匯出")) { "failure was reported as success" }
                Log.i("DoctorActionsDeviceProbe", "cancel-failure=PERMISSION_DENIED temp-preserved=true")
            } finally { android.system.Os.chmod(tempDir.absolutePath,tempMode) }
            stage("preview-cancel-permission-recovery")
            button("預覽與匯出"); dialogButton("取消")
            waitUntil("cancel-recovered") { textPresent("已取消匯出") && controller.status().tempBytes==0L }
            stage("picker-cancel")
            button("預覽與匯出"); dialogButton("選擇儲存位置")
            waitUntil("picker-open") { inst.uiAutomation.rootInActiveWindow?.packageName?.toString()=="com.google.android.documentsui" }
            check(inst.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK))
            SystemClock.sleep(400)
            if (inst.uiAutomation.rootInActiveWindow?.packageName?.toString()=="com.google.android.documentsui") {
                check(inst.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK))
            }
            waitUntil("picker-cancel-result") { textPresent("已取消匯出") }
            check(controller.awaitWriterIdleForTest(10000))
            stage("picker-clear-stale-return")
            button("預覽與匯出"); dialogButton("選擇儲存位置")
            waitUntil("clear-race-picker-open") {
                inst.uiAutomation.rootInActiveWindow?.packageName?.toString() ==
                    "com.google.android.documentsui"
            }
            val clearRaceResults =
                java.util.concurrent.LinkedBlockingQueue<com.hamanpaul.liukai.doctor.DoctorUiResult>()
            val clearRaceListener = object : com.hamanpaul.liukai.doctor.DoctorRuntimeListener {
                override fun onDoctorStatus(status: com.hamanpaul.liukai.doctor.DoctorRuntimeStatus) = Unit
                override fun onDoctorResult(result: com.hamanpaul.liukai.doctor.DoctorUiResult) {
                    clearRaceResults.add(result)
                }
            }

            val clearRaceSubscription = controller.subscribe(clearRaceListener)
            try {
                val clearEpoch = controller.status().epoch
                controller.clear(clearRaceSubscription)
                val clearResult = clearRaceResults.poll(10, java.util.concurrent.TimeUnit.SECONDS)
                    ?: throw AssertionError("clear while picker open timed out")
                check(
                    clearResult.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.CLEAR &&
                        clearResult.success
                ) { "clear while picker open failed: $clearResult" }
                check(controller.status().epoch > clearEpoch)
                check(inst.uiAutomation.performGlobalAction(
                    android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                ))
                SystemClock.sleep(400)
                if (inst.uiAutomation.rootInActiveWindow?.packageName?.toString() ==
                    "com.google.android.documentsui"
                ) {
                    check(inst.uiAutomation.performGlobalAction(
                        android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                    ))
                }
                waitUntil("stale-picker-return") {
                    textPresent("預覽已失效，請重新建立預覽。") &&
                        controller.status().tempBytes == 0L
                }
            } finally {
                clearRaceSubscription.detach()
            }
            stage("picker-clear-result-ok-stale")
            button("標記剛才的問題")
            dialogButton("標記")
            waitUntil("result-ok-stale-marked") {
                textPresent("已標記問題時間")
            }
            button("預覽與匯出")
            dialogButton("選擇儲存位置")
            ownsSafPicker = true
            waitUntil("result-ok-stale-picker-open") {
                inst.uiAutomation.rootInActiveWindow
                    ?.packageName
                    ?.toString() == "com.google.android.documentsui"
            }
            val staleResultQueue =
                java.util.concurrent.LinkedBlockingQueue<com.hamanpaul.liukai.doctor.DoctorUiResult>()
            val staleResultListener =
                object : com.hamanpaul.liukai.doctor.DoctorRuntimeListener {
                    override fun onDoctorStatus(
                        status: com.hamanpaul.liukai.doctor.DoctorRuntimeStatus,
                    ) = Unit

                    override fun onDoctorResult(
                        result: com.hamanpaul.liukai.doctor.DoctorUiResult,
                    ) {
                        staleResultQueue.add(result)
                    }
                }
            val staleResultSubscription = controller.subscribe(staleResultListener)
            try {
                val beforeResultOkClear = controller.status()
                check(
                    beforeResultOkClear.enabled &&
                        beforeResultOkClear.collecting &&
                        beforeResultOkClear.tempBytes > 0L
                ) {
                    "RESULT_OK stale: clear precondition changed status=$beforeResultOkClear"
                }
                controller.clear(staleResultSubscription)
                val staleResultClear =
                    staleResultQueue.poll(10, java.util.concurrent.TimeUnit.SECONDS)
                        ?: throw AssertionError("RESULT_OK stale clear result timeout")
                check(
                    staleResultClear.operation ==
                        com.hamanpaul.liukai.doctor.DoctorUiOperation.CLEAR &&
                        staleResultClear.success
                ) {
                    "RESULT_OK stale clear failed: $staleResultClear"
                }
                val clearedForResultOk = controller.status()
                check(
                    clearedForResultOk.epoch > beforeResultOkClear.epoch &&
                        clearedForResultOk.enabled &&
                        clearedForResultOk.collecting &&
                        clearedForResultOk.tempBytes == 0L
                ) {
                    "RESULT_OK stale clear state mismatch: before=$beforeResultOkClear " +
                        "after=$clearedForResultOk"
                }
                staleResultUri =
                    android.provider.DocumentsContract.buildDocumentUri(
                        "com.android.externalstorage.documents",
                        "primary:Download/$staleResultExportName",
                    )
                systemPositive(
                    "com.google.android.documentsui",
                    null,
                    staleResultExportName,
                )
                waitUntil("result-ok-stale-message") {
                    textPresent("預覽已失效，請重新建立預覽。") &&
                        controller.status().tempBytes == 0L
                }
                ownsSafPicker = false
                val afterResultOk = controller.status()
                check(
                    afterResultOk.enabled &&
                        afterResultOk.collecting &&
                        afterResultOk.epoch == clearedForResultOk.epoch &&
                        afterResultOk.tempBytes == 0L
                ) {
                    "RESULT_OK stale reversed clear or consent: cleared=$clearedForResultOk " +
                        "after=$afterResultOk"
                }
                check(
                    !textPresent("診斷 ZIP 已儲存")
                ) {
                    "RESULT_OK stale falsely reported a saved ZIP"
                }
                val ownedUri = checkNotNull(staleResultUri) {
                    "RESULT_OK stale lost its owned document URI"
                }
                val columns = arrayOf(
                    android.provider.OpenableColumns.SIZE,
                    android.provider.OpenableColumns.DISPLAY_NAME,
                )
                val cursor = activity.contentResolver.query(
                    ownedUri,
                    columns,
                    null,
                    null,
                    null,
                )
                val reportedSize = checkNotNull(
                    cursor?.use {
                        check(it.moveToFirst()) {
                            "RESULT_OK stale provider row missing: $ownedUri"
                        }
                        it.getLong(0)
                    },
                ) {
                    "RESULT_OK stale provider cursor missing: $ownedUri"
                }
                val firstByte = checkNotNull(
                    activity.contentResolver.openInputStream(ownedUri),
                ) {
                    "RESULT_OK stale provider stream missing: $ownedUri"
                }.use { it.read() }
                val descriptorSize = checkNotNull(
                    activity.contentResolver.openFileDescriptor(ownedUri, "r"),
                ) {
                    "RESULT_OK stale provider descriptor missing: $ownedUri"
                }.use { it.statSize }
                check(
                    reportedSize in setOf(-1L, 0L) &&
                        descriptorSize == 0L &&
                        firstByte == -1
                ) {
                    "RESULT_OK stale copied ZIP data: reportedSize=$reportedSize " +
                        "descriptorSize=$descriptorSize firstByte=$firstByte " +
                        "uri=$ownedUri"
                }
            } finally {
                staleResultSubscription.detach()
            }
            button("標記剛才的問題"); dialogButton("標記")
            waitUntil("post-clear-race-mark") { textPresent("已標記問題時間") }
            inst.runOnMainSync { controller.recordInputStarted() }
            check(controller.awaitWriterIdleForTest(10000)) { "post-clear-race record idle timeout" }
            waitUntil("post-clear-race-record") {
                java.io.File(activity.filesDir, "doctor/records").listFiles()
                    ?.any { it.isFile && it.length() > 0L } == true
            }
            stage("preview-save")
            button("預覽與匯出"); dialogButton("選擇儲存位置"); systemPositive("com.google.android.documentsui", null); exportCreated = true; waitUntil("save-result",20000) { textPresent("診斷 ZIP 已儲存") }
            stage("invalid-saf-target")
            val invalidTarget = com.hamanpaul.liukai.doctor.DoctorSafExportTarget(activity, android.net.Uri.parse("file:///not-a-document"))
            check(!invalidTarget.validate()) { "non-document URI was accepted" }
            stage("saf-permission-denied")
            val deniedUri = android.provider.DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download/not-granted-doctor-test.zip")
            val denied = com.hamanpaul.liukai.doctor.DoctorSafExportTarget(activity, deniedUri)
            check(!denied.validate()) { "ungranted document unexpectedly validated" }
            check(denied.open() == null) { "ungranted document unexpectedly opened" }
            stage("stale-prepare")
            val results = java.util.concurrent.LinkedBlockingQueue<com.hamanpaul.liukai.doctor.DoctorUiResult>()
            val listener = object : com.hamanpaul.liukai.doctor.DoctorRuntimeListener {
                override fun onDoctorStatus(status: com.hamanpaul.liukai.doctor.DoctorRuntimeStatus) = Unit
                override fun onDoctorResult(result: com.hamanpaul.liukai.doctor.DoctorUiResult) { results.add(result) }
            }
            val subscription = controller.subscribe(listener)
            controller.prepareExport(subscription)
            val prepared = results.poll(10, java.util.concurrent.TimeUnit.SECONDS)
                ?: throw AssertionError("stale prepare timeout")
            check(prepared.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.PREPARE && prepared.success && prepared.export != null)
            val oldHandle = prepared.export!!.handle
            stage("deleted-saf-target-save")
            val deletedTargetUri = checkNotNull(staleResultUri) {
                "deleted SAF target requires the RESULT_OK-owned document row"
            }
            check(
                android.provider.DocumentsContract.deleteDocument(
                    activity.contentResolver,
                    deletedTargetUri,
                )
            ) {
                "deleted SAF target could not delete its owned row: $deletedTargetUri"
            }
            staleResultUri = null
            waitUntil("deleted-saf-target-preview-temp") {
                controller.status().tempBytes > 0L
            }
            val deletedTargetResults =
                java.util.concurrent.LinkedBlockingQueue<com.hamanpaul.liukai.doctor.DoctorUiResult>()
            val deletedTargetListener =
                object : com.hamanpaul.liukai.doctor.DoctorRuntimeListener {
                    override fun onDoctorStatus(
                        status: com.hamanpaul.liukai.doctor.DoctorRuntimeStatus,
                    ) = Unit

                    override fun onDoctorResult(
                        result: com.hamanpaul.liukai.doctor.DoctorUiResult,
                    ) {
                        deletedTargetResults.add(result)
                    }
                }
            val deletedTargetSubscription =
                controller.subscribe(deletedTargetListener)
            var keepDeletedTargetPreview = false
            try {
                val tempBeforeDeletedSave = controller.status().tempBytes
                controller.saveExport(
                    oldHandle,
                    com.hamanpaul.liukai.doctor.DoctorSafExportTarget(
                        activity,
                        deletedTargetUri,
                    ),
                    deletedTargetSubscription,
                )
                val deletedTargetSave =
                    deletedTargetResults.poll(
                        10,
                        java.util.concurrent.TimeUnit.SECONDS,
                    ) ?: throw AssertionError(
                        "deleted SAF target save result timeout",
                    )
                check(
                    deletedTargetSave.operation ==
                        com.hamanpaul.liukai.doctor.DoctorUiOperation.SAVE &&
                        !deletedTargetSave.success &&
                        deletedTargetSave.failure in setOf(
                            com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.INVALID_REQUEST,
                            com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.IO_ERROR,
                            com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.PERMISSION_DENIED,
                        )
                ) {
                    "deleted SAF target did not fail through the public SAVE result: " +
                        deletedTargetSave
                }
                Log.i(
                    "DoctorActionsDeviceProbe",
                    "deleted-saf-target failure=${deletedTargetSave.failure} " +
                        "preview-temp-before=$tempBeforeDeletedSave",
                )
                check(
                    controller.status().tempBytes == tempBeforeDeletedSave &&
                        controller.status().tempBytes > 0L
                ) {
                    "deleted SAF target changed owned preview temp: " +
                        "before=$tempBeforeDeletedSave " +
                        "after=${controller.status().tempBytes}"
                }
                keepDeletedTargetPreview = true
            } finally {
                if (!keepDeletedTargetPreview) {
                    controller.cancelExport(oldHandle, null)
                    if (!controller.awaitWriterIdleForTest(10000)) {
                        Log.e(
                            "DoctorActionsDeviceProbe",
                            "deleted SAF target failure cleanup writer idle timeout",
                        )
                    }
                }
                deletedTargetSubscription.detach()
            }
            fun savedHash(): String {
                val fd = inst.uiAutomation.executeShellCommand("sha256sum /sdcard/Download/$exportName")
                return android.os.ParcelFileDescriptor.AutoCloseInputStream(fd).bufferedReader().use { it.readText() }.trim()
            }
            val hashBefore = savedHash(); check(hashBefore.matches(Regex("[a-f0-9]{64} .*"))); Log.i("DoctorActionsDeviceProbe", "saved=$hashBefore")
            stage("invalid-destination-rejected")
            controller.saveExport(oldHandle, invalidTarget, subscription)
            val invalidDestination = results.poll(10, java.util.concurrent.TimeUnit.SECONDS) ?: throw AssertionError("invalid destination timeout")
            check(invalidDestination.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.SAVE && !invalidDestination.success && invalidDestination.failure == com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.INVALID_REQUEST)
            check(savedHash() == hashBefore) { "invalid destination changed existing ZIP" }
            stage("invalid-handle-rejected")
            val documentUri = android.provider.DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download/$exportName")
            val wrongHandle = oldHandle.copy(token = oldHandle.token + 1)
            controller.saveExport(wrongHandle, com.hamanpaul.liukai.doctor.DoctorSafExportTarget(activity, documentUri), subscription)
            val invalidSave = results.poll(10, java.util.concurrent.TimeUnit.SECONDS) ?: throw AssertionError("invalid save timeout")
            check(invalidSave.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.SAVE && !invalidSave.success && invalidSave.failure == com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.INVALID_SNAPSHOT)
            controller.cancelExport(wrongHandle, subscription)
            val invalidCancel = results.poll(10, java.util.concurrent.TimeUnit.SECONDS) ?: throw AssertionError("invalid cancel timeout")
            check(invalidCancel.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.CANCEL_EXPORT && !invalidCancel.success && invalidCancel.failure == com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.INVALID_SNAPSHOT)
            check(savedHash() == hashBefore) { "invalid save changed existing ZIP" }
            stage("clear-cancel")
            button("清除診斷記錄"); dialogButton("取消"); check(controller.status().enabled)
            stage("clear-permission-failure")
            val doctorRoot=java.io.File(activity.filesDir, "doctor")
            val originalRootMode=android.system.Os.stat(doctorRoot.absolutePath).st_mode and 511
            val preservedRecords=java.io.File(doctorRoot,"records").listFiles()?.filter { it.isFile } ?: emptyList()
            check(preservedRecords.isNotEmpty()) { "record not present before clear fault" }
            try {
                android.system.Os.chmod(doctorRoot.absolutePath,320)
                button("清除診斷記錄");dialogButton("清除")
                waitUntil("clear-denied-visible") { textPresent("清除尚未完成") }
                check(preservedRecords.any { it.exists() }) { "failed clear removed old records" }
                val failed = controller.status()
                check(failed.enabled && !failed.collecting && failed.interrupted && failed.failure == com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.EPOCH_COMMIT_FAILED) { "unexpected failed clear state: $failed" }
                Log.i("DoctorActionsDeviceProbe", "clear-failure=visible records-preserved=true consent=true collecting=false epoch-failure=true")
            } finally { android.system.Os.chmod(doctorRoot.absolutePath,originalRootMode) }
            stage("clear-confirm")
            val epoch=controller.status().epoch
            button("清除診斷記錄"); dialogButton("清除"); waitUntil("clear-result") { controller.status().epoch > epoch }
            check(controller.status().enabled && controller.status().collecting) { "clear retry did not preserve consent" }
            Log.i("DoctorActionsDeviceProbe", "clear-retry=success consent=true collecting=true")
            stage("stale-save-after-clear")
            val uri = android.provider.DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download/$exportName")
            controller.saveExport(oldHandle, com.hamanpaul.liukai.doctor.DoctorSafExportTarget(activity, uri), subscription)
            val stale = results.poll(10, java.util.concurrent.TimeUnit.SECONDS) ?: throw AssertionError("stale save timeout")
            check(stale.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.SAVE && !stale.success && stale.failure == com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.EPOCH_MISMATCH) { "unexpected stale result: $stale" }
            check(savedHash() == hashBefore) { "stale save changed existing ZIP" }
            controller.cancelExport(oldHandle, subscription)
            val staleCancel = results.poll(10, java.util.concurrent.TimeUnit.SECONDS) ?: throw AssertionError("stale cancel timeout")
            check(staleCancel.operation == com.hamanpaul.liukai.doctor.DoctorUiOperation.CANCEL_EXPORT && !staleCancel.success && staleCancel.failure == com.hamanpaul.liukai.core.doctor.DoctorStoreFailure.EPOCH_MISMATCH)
            subscription.detach()
            stage("filesystem-failure")
            if (!controller.status().enabled) {
                button("開啟 Doctor 診斷"); dialogButton("開啟")
                waitUntil("reenable-after-clear-fault") { controller.status().enabled && controller.status().collecting }
            }
            inst.runOnMainSync { controller.recordInputStarted() }
            val recordsDir=java.io.File(activity.filesDir, "doctor/records")
            waitUntil("record-ready") { recordsDir.listFiles()?.any { it.name.startsWith("doctor-record-") && it.length()>0 } == true }
            val record=recordsDir.listFiles()!!.filter { it.name.startsWith("doctor-record-") }.maxByOrNull { it.name }!!
            val oldMode=android.system.Os.stat(record.absolutePath).st_mode and 511
            try {
                android.system.Os.chmod(record.absolutePath,256)
                inst.runOnMainSync { controller.recordInputStarted() }
                waitUntil("filesystem-interrupted") { controller.status().interrupted && !controller.status().collecting }
                Log.i("DoctorActionsDeviceProbe", "real-io-failure=${controller.status().failure}")
            } finally { android.system.Os.chmod(record.absolutePath,oldMode) }
            stage("filesystem-recovery")
            if (controller.status().enabled) { button("關閉 Doctor 診斷"); waitUntil("fault-disabled") { !controller.status().enabled } }
            button("開啟 Doctor 診斷"); dialogButton("開啟"); waitUntil("fault-reenabled") { controller.status().enabled && controller.status().collecting && !controller.status().interrupted }
            val writtenBefore=controller.numericSnapshot().written
            inst.runOnMainSync { controller.recordInputStarted() }
            waitUntil("write-recovered") { controller.numericSnapshot().written>writtenBefore }
            stage("disable")
            if (controller.status().enabled) { button("關閉 Doctor 診斷"); waitUntil("disabled") { !controller.status().enabled } }
            stage("complete")
            return "Doctor actions device probe verified: quiet off metrics, real soft/hardware/candidate IME service chain, consent/actions, public callback privacy and quiescent numeric invariants, recent/specific/invalid mark, preview recreation/destroy/back cancel, picker cancellation and clear-race stale rejection, RESULT_OK stale return and deleted SAF target rejection, real export permission failure/recovery, SAF save, invalid and ungranted SAF targets, clear cancel/real permission failure/confirm, invalid destination/handles and stale epoch rejected, real filesystem failure/recovery, disable."
        } catch (failure: Throwable) {
            actionFailure = failure
            throw failure
        } finally {
            try {
                uiAutomation.serviceInfo = uiAutomation.serviceInfo.apply {
                    flags = originalUiAutomationFlags
                }
                try {
                    if (
                        ownsSafPicker &&
                        inst.uiAutomation.rootInActiveWindow
                            ?.packageName
                            ?.toString() == "com.google.android.documentsui"
                    ) {
                        inst.uiAutomation.performGlobalAction(
                            android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                        )
                    }
                    originalAlwaysFinishActivities?.let { original ->
                        if (original == "null") {
                            shellText(
                                "settings delete global always_finish_activities",
                            )
                        } else {
                            shellText(
                                "settings put global always_finish_activities $original",
                            )
                        }
                        val restored =
                            shellText(
                                "settings get global always_finish_activities",
                            )
                        check(restored == original) {
                            "could not restore always_finish_activities: " +
                                "expected=$original actual=$restored"
                        }
                    }
                } finally {
                    try {
                        restoreIdleStrip?.invoke()
                    } finally {
                        try {
                            var staleCleanupFailure: Throwable? = null
                            try {
                                staleResultUri?.let { uri ->
                                    val exists = try {
                                        checkNotNull(
                                            activity.contentResolver.query(
                                                uri,
                                                arrayOf(android.provider.OpenableColumns.DISPLAY_NAME),
                                                null,
                                                null,
                                                null,
                                            ),
                                        ) { "owned stale document cleanup cursor missing: $uri" }
                                            .use { it.moveToFirst() }
                                    } catch (_: java.io.FileNotFoundException) {
                                        false
                                    }
                                    if (exists) {
                                        check(
                                            android.provider.DocumentsContract.deleteDocument(
                                                activity.contentResolver,
                                                uri,
                                            )
                                        ) {
                                            "failed to clean owned RESULT_OK stale document: $uri"
                                        }
                                        Log.i(
                                            "DoctorActionsDeviceProbe",
                                            "cleaned-owned-stale-export=$staleResultExportName",
                                        )
                                    }
                                }
                            } catch (failure: Throwable) {
                                staleCleanupFailure = failure
                                Log.e("DoctorActionsDeviceProbe", "owned stale document cleanup failed", failure)
                            }
                            try {
                                if (exportCreated) {
                                    check(exportName.matches(
                                        Regex("liu-kai-doctor-device-test-[0-9]+\\.zip"),
                                    ))
                                    val path = "/sdcard/Download/$exportName"
                                    val evidence = java.io.File(
                                        activity.filesDir,
                                        "doctor-device-test-evidence.zip",
                                    )
                                    val source = inst.uiAutomation.executeShellCommand(
                                        "cat $path",
                                    )
                                    android.os.ParcelFileDescriptor
                                        .AutoCloseInputStream(source)
                                        .use { input ->
                                            evidence.outputStream().use { input.copyTo(it) }
                                        }
                                    if (evidence.length() > 0L) {
                                        val hash = java.security.MessageDigest
                                            .getInstance("SHA-256")
                                            .digest(evidence.readBytes())
                                            .joinToString("") { "%02x".format(it) }
                                        Log.i(
                                            "DoctorActionsDeviceProbe",
                                            "saved-evidence=$hash bytes=${evidence.length()} " +
                                                "name=$exportName",
                                        )
                                    }
                                    val uri = android.provider.DocumentsContract
                                        .buildDocumentUri(
                                            "com.android.externalstorage.documents",
                                            "primary:Download/$exportName",
                                        )
                                    check(
                                        android.provider.DocumentsContract.deleteDocument(
                                            activity.contentResolver,
                                            uri,
                                        )
                                    ) { "failed to clean own test export" }
                                    Log.i(
                                        "DoctorActionsDeviceProbe",
                                        "cleaned-own-export=$exportName",
                                    )
                                }
                            } catch (failure: Throwable) {
                                staleCleanupFailure?.let { failure.addSuppressed(it) }
                                throw failure
                            }
                            staleCleanupFailure?.let { throw it }
                        } finally {
                            inst.runOnMainSync { activity.finish() }
                        }
                    }
                }
            } catch (cleanupFailure: Throwable) {
                val primary = actionFailure
                if (primary == null) throw cleanupFailure
                if (primary !== cleanupFailure) primary.addSuppressed(cleanupFailure)
            }
        }
    }
}
