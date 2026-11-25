package com.hamanpaul.liukai.ime

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.os.Bundle
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.util.Log
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.FrameLayout
import com.hamanpaul.liukai.settings.SettingsActivity
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class TouchDiagnosticsFrameworkTest : Instrumentation() {
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        start()
    }

    override fun onStart() {
        super.onStart()
        try {
            exerciseFrameworkUnsetPressedCallback()
            finish(Activity.RESULT_OK, Bundle().apply { putString("stream", "Framework callback trace verified.") })
        } catch (failure: Throwable) {
            finish(
                Activity.RESULT_CANCELED,
                Bundle().apply {
                    putString("shortMsg", failure.message ?: failure.javaClass.name)
                    putString("stream", Log.getStackTraceString(failure))
                },
            )
        }
    }

    private fun exerciseFrameworkUnsetPressedCallback() {
        requireCondition(Looper.myLooper() != Looper.getMainLooper(), "stage=thread instrumentation body must not block main")
        val keyId = "framework-probe-" + SystemClock.uptimeMillis()
        stage(keyId, "verify_caller_format")
        val normalCaller = TouchDiagnostics.describeCaller(Throwable().stackTrace)
        val emptyStack = Throwable().apply { stackTrace = emptyArray() }.stackTrace
        requireCondition(normalCaller != "unknown" && normalCaller.contains("."), "run=$keyId normal stack was not formatted: $normalCaller")
        requireCondition(
            TouchDiagnostics.describeCaller(emptyStack) == "unknown",
            "run=$keyId empty stack fallback must be unknown",
        )

        stage(keyId, "launch_activity")
        val monitor = addMonitor(SettingsActivity::class.java.name, null, false)
        targetContext.startActivity(
            Intent(targetContext, SettingsActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
        val activity = monitor.waitForActivityWithTimeout(5000) as? SettingsActivity
        removeMonitor(monitor)
        val targetActivity = activity ?: throw AssertionError("run=$keyId stage=launch_activity timed out after 5000ms")

        lateinit var keyFrame: FrameLayout
        val attached = CountDownLatch(1)
        val dispatched = CountDownLatch(1)
        val callbackPassed = CountDownLatch(1)
        stage(keyId, "attach_frame")
        runOnMainSync {
            requireCondition(Looper.myLooper() == Looper.getMainLooper(), "run=$keyId stage=attach must run on main")
            keyFrame = TouchDiagnostics.keyFrame(targetActivity, keyId)
            keyFrame.isClickable = true
            keyFrame.addOnAttachStateChangeListener(object : android.view.View.OnAttachStateChangeListener {
                override fun onViewAttachedToWindow(view: android.view.View) {
                    attached.countDown()
                }

                override fun onViewDetachedFromWindow(view: android.view.View) = Unit
            })
            targetActivity.addContentView(keyFrame, ViewGroup.LayoutParams(120, 120))
        }

        stage(keyId, "await_attach")
        requireCondition(attached.await(3000, TimeUnit.MILLISECONDS), "run=$keyId stage=await_attach timed out after 3000ms")
        stage(keyId, "dispatch_down_up_down")
        keyFrame.post {
            requireCondition(Looper.myLooper() == Looper.getMainLooper(), "run=$keyId stage=dispatch must run on main")
            requireCondition(keyFrame.isAttachedToWindow, "run=$keyId stage=dispatch frame is detached")

            // View posts UnsetPressedState on UP. Keeping DOWN / UP / DOWN in one main-loop turn
            // makes Android run its own callback while the second touch is active.
            val firstDown = SystemClock.uptimeMillis()
            dispatch(keyId, keyFrame, MotionEvent.ACTION_DOWN, firstDown, firstDown)
            dispatch(keyId, keyFrame, MotionEvent.ACTION_UP, firstDown, firstDown + 1)
            val secondDown = firstDown + 2
            dispatch(keyId, keyFrame, MotionEvent.ACTION_DOWN, secondDown, secondDown)
            dispatched.countDown()
            keyFrame.post { callbackPassed.countDown() }
        }

        requireCondition(dispatched.await(3000, TimeUnit.MILLISECONDS), "run=$keyId stage=dispatch timed out after 3000ms")
        stage(keyId, "await_framework_callback")
        requireCondition(callbackPassed.await(3000, TimeUnit.MILLISECONDS), "run=$keyId stage=await_callback timed out after 3000ms")
        stage(keyId, "exercise_in_dispatch_reset")
        runOnMainSync {
            val thirdDown = SystemClock.uptimeMillis()
            dispatch(keyId, keyFrame, MotionEvent.ACTION_DOWN, thirdDown, thirdDown)
            requireCondition(keyFrame.isPressed, "run=$keyId stage=move expected pressed state")
            dispatch(keyId, keyFrame, MotionEvent.ACTION_MOVE, thirdDown, thirdDown + 1, 160f)
            requireCondition(!keyFrame.isPressed, "run=$keyId stage=move should clear pressed state")
            dispatch(keyId, keyFrame, MotionEvent.ACTION_CANCEL, thirdDown, thirdDown + 2)
        }

        stage(keyId, "verify_trace")
        val traceFd = uiAutomation.executeShellCommand("logcat -d -s LiuKaiTouch:D")
        val allTrace = ParcelFileDescriptor.AutoCloseInputStream(traceFd).bufferedReader().use { it.readText() }
        val trace = allTrace.lineSequence().filter { it.contains("key=$keyId ") }.joinToString("\n")
        val callbackLine = trace.lineSequence().firstOrNull {
            it.contains("phase=key_pressed") &&
                it.contains("action=ACTION_DOWN") &&
                it.contains("source=android.view.View\$UnsetPressedState.run")
        }
        val inDispatchLine = trace.lineSequence().firstOrNull {
            it.contains("phase=key_pressed") &&
                it.contains("action=ACTION_MOVE") &&
                it.contains("source=direct")
        }
        requireCondition(
            callbackLine != null,
            "run=$keyId stage=verify_trace expected this key's ACTION_DOWN UnsetPressedState callback; trace was:\n$trace",
        )
        requireCondition(
            inDispatchLine != null,
            "run=$keyId stage=verify_trace expected this key's in-dispatch ACTION_MOVE reset; trace was:\n$trace",
        )

        stage(keyId, "finish_activity")
        runOnMainSync { targetActivity.finish() }
        stage(keyId, "dump_coverage")
        repeat(2) { attempt ->
            val dump = uiAutomation.executeShellCommand(
                "am broadcast -a com.hamanpaul.liukai.DUMP_COVERAGE " +
                    "-n com.hamanpaul.liukai/.debug.CoverageDumpReceiver"
            )
            val output = ParcelFileDescriptor.AutoCloseInputStream(dump).bufferedReader().use { it.readText() }
            requireCondition(
                output.contains("result=1") && output.contains("OK "),
                "run=$keyId stage=dump_coverage attempt=$attempt failed: $output",
            )
        }
    }

    private fun dispatch(
        keyId: String,
        keyFrame: FrameLayout,
        action: Int,
        downTime: Long,
        eventTime: Long,
        x: Float = 60f,
        y: Float = 60f,
    ) {
        val event = MotionEvent.obtain(downTime, eventTime, action, x, y, 0)
        try {
            requireCondition(keyFrame.dispatchTouchEvent(event), "run=$keyId stage=dispatch action=$action not handled")
        } finally {
            event.recycle()
        }
    }

    private fun stage(keyId: String, name: String) = Log.i(TAG, "run=$keyId stage=$name")

    private fun requireCondition(condition: Boolean, message: String) {
        if (!condition) throw AssertionError(message)
    }

    private companion object {
        const val TAG = "TouchDiagTest"
    }
}
