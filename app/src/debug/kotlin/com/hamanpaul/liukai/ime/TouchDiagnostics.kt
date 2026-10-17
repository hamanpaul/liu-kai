package com.hamanpaul.liukai.ime

import android.content.Context
import android.os.SystemClock
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout

/** Debug-build-only touch trace. It deliberately records key ids and geometry, never composing text. */
internal object TouchDiagnostics {
    private const val TAG = "LiuKaiTouch"
    private var lastDispatchEventTimeMs = 0L

    internal fun dispatchEvent(event: MotionEvent, root: View, keyboard: View, keys: Map<String, View>) =
        recordDispatch(event, root, keyboard, keys)

    internal fun startDispatch(): Long = SystemClock.elapsedRealtimeNanos()

    internal fun endDispatch(event: MotionEvent, startedNs: Long, handled: Boolean) =
        recordDispatchEnd(event, startedNs, handled)

    internal fun slotForward(
        key: String,
        slot: View,
        keyView: View,
        event: MotionEvent,
        startedNs: Long,
        handled: Boolean,
    ) = recordSlotForward(key, slot, keyView, event, startedNs, handled)

    internal fun beginKeyAction(key: String, blank: Boolean): Long = recordKeyStart(key, blank)

    internal fun endKeyAction(key: String, blank: Boolean, startedNs: Long, handled: Boolean) =
        recordKeyEnd(key, blank, startedNs, handled)

    internal fun beginRebuild(layer: String, force: Boolean, touching: Boolean, pending: Boolean): Long =
        recordRebuildStart(layer, force, touching, pending)

    internal fun endRebuild(layer: String, startedNs: Long, keyCount: Int, pending: Boolean) =
        recordRebuildEnd(layer, startedNs, keyCount, pending)

    internal fun gesture(key: String, phase: String, detail: String) = recordGesture(key, phase, detail)

    internal fun candidateTap(index: Int, target: View, scroller: android.widget.HorizontalScrollView) =
        recordCandidateTap(index, target, scroller)

    internal fun keyFrame(context: Context, key: String): FrameLayout = TracedKeyFrame(context, key)

    @PublishedApi
    internal fun recordKeyViewTouch(
        key: String,
        action: String,
        actionIndex: Int,
        eventTimeMs: Long,
        pointerSummary: String,
        pressedBefore: Boolean,
        pressedAfter: Boolean,
        focusable: Boolean,
        focusableInTouchMode: Boolean,
        handled: Boolean,
    ) {
        Log.d(
            TAG,
            "phase=key_touch key=$key action=$action actionIndex=$actionIndex eventTimeMs=$eventTimeMs " +
                "uptimeMs=${SystemClock.uptimeMillis()} pointers=[$pointerSummary] " +
                "pressedBefore=$pressedBefore pressedAfter=$pressedAfter focusable=$focusable " +
                "focusableInTouchMode=$focusableInTouchMode handled=$handled",
        )
    }

    @PublishedApi
    internal fun recordKeyPressed(
        key: String,
        from: Boolean,
        requested: Boolean,
        to: Boolean,
        action: String,
        eventTimeMs: Long,
        pointerIds: String,
        source: String,
    ) {
        Log.d(
            TAG,
            "phase=key_pressed key=$key from=$from requested=$requested to=$to " +
                "action=$action eventTimeMs=$eventTimeMs " +
                "pointerIds=[$pointerIds] source=$source uptimeMs=${SystemClock.uptimeMillis()}",
        )
    }

    @PublishedApi
    internal fun recordDispatch(event: MotionEvent, root: View, keyboard: View, keys: Map<String, View>) {
        val now = SystemClock.uptimeMillis()
        lastDispatchEventTimeMs = event.eventTime
        val rootBounds = bounds(root)
        val keyboardBounds = bounds(keyboard)
        val pointers = (0 until event.pointerCount).joinToString(";") { index ->
            val rawX = event.getRawX(index)
            val rawY = event.getRawY(index)
            val hit = hit(rawX, rawY, keyboardBounds, keys)
            "id=${event.getPointerId(index)} local=(${event.getX(index)},${event.getY(index)}) " +
                "raw=($rawX,$rawY) visual$hit"
        }
        Log.d(
            TAG,
            "phase=dispatch action=${MotionEvent.actionToString(event.actionMasked)} actionIndex=${event.actionIndex} " +
                "eventTimeMs=${event.eventTime} uptimeMs=$now eventAgeMs=${now - event.eventTime} " +
                "pointerCount=${event.pointerCount} window=$rootBounds keyboard=$keyboardBounds pointers=[$pointers]",
        )
    }

private class TracedKeyFrame(context: Context, key: String) : FrameLayout(context) {
    private var diagnosticKey: String? = key
    private var lastAction = "NONE"
    private var lastEventTimeMs = 0L
    private var lastPointerIds = ""
    private var activeTouch = false
    private var dispatchingTouch = false

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> activeTouch = true
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> activeTouch = false
        }
        val previousDispatching = dispatchingTouch
        dispatchingTouch = true
        val keyId = diagnosticKey
        val pressedBefore = isPressed
        lastAction = MotionEvent.actionToString(event.actionMasked)
        lastEventTimeMs = event.eventTime
        val pointers = (0 until event.pointerCount).joinToString(";") { index ->
            "id=${event.getPointerId(index)} local=(${event.getX(index)},${event.getY(index)})"
        }
        lastPointerIds = (0 until event.pointerCount).joinToString(";") { index -> event.getPointerId(index).toString() }
        val handled = try {
            super.dispatchTouchEvent(event)
        } finally {
            dispatchingTouch = previousDispatching
        }
        if (keyId != null) {
            TouchDiagnostics.recordKeyViewTouch(
                keyId,
                lastAction,
                event.actionIndex,
                event.eventTime,
                pointers,
                pressedBefore,
                isPressed,
                isFocusable,
                isFocusableInTouchMode,
                handled,
            )
        }
        return handled
    }

    override fun setPressed(pressed: Boolean) {
        val keyId = diagnosticKey
        val before = isPressed
        val traceCaller = if (!pressed && activeTouch && !dispatchingTouch) {
            Throwable().stackTrace.drop(1).firstOrNull()?.let { "${it.className}.${it.methodName}" } ?: "unknown"
        } else {
            "direct"
        }
        super.setPressed(pressed)
        val after = isPressed
        if (keyId != null && before != after) {
            TouchDiagnostics.recordKeyPressed(
                keyId,
                before,
                pressed,
                after,
                lastAction,
                lastEventTimeMs,
                lastPointerIds,
                traceCaller,
            )
        }
    }
}

    @PublishedApi
    internal fun recordDispatchEnd(event: MotionEvent, startedNs: Long, handled: Boolean) {
        val elapsedMs = (SystemClock.elapsedRealtimeNanos() - startedNs) / 1_000_000.0
        Log.d(
            TAG,
            "phase=dispatch_result action=${MotionEvent.actionToString(event.actionMasked)} " +
                "eventTimeMs=${event.eventTime} handled=$handled dispatchMs=$elapsedMs",
        )
    }

    @PublishedApi
    internal fun recordSlotForward(
        key: String,
        slot: View,
        keyView: View,
        event: MotionEvent,
        startedNs: Long,
        handled: Boolean,
    ) {
        val elapsedMs = (SystemClock.elapsedRealtimeNanos() - startedNs) / 1_000_000.0
        val slotBounds = bounds(slot)
        val keyBounds = bounds(keyView)
        val pointers = (0 until event.pointerCount).joinToString(";") { index ->
            "id=${event.getPointerId(index)} slotLocal=(${event.getX(index)},${event.getY(index)}) " +
                "keyLocal=(${event.getX(index) - keyView.left},${event.getY(index) - keyView.top}) " +
                "raw=(${event.getRawX(index)},${event.getRawY(index)})"
        }
        Log.d(
            TAG,
            "phase=slot_forward target=key:$key action=${MotionEvent.actionToString(event.actionMasked)} " +
                "actionIndex=${event.actionIndex} eventTimeMs=${event.eventTime} pointers=[$pointers] " +
                "slot=$slotBounds key=$keyBounds handled=$handled slotDispatchMs=$elapsedMs",
        )
    }

    @PublishedApi
    internal fun recordKeyStart(key: String, blank: Boolean): Long {
        val started = SystemClock.elapsedRealtimeNanos()
        Log.d(TAG, "phase=click key=$key lastDispatchEventTimeMs=$lastDispatchEventTimeMs uptimeMs=${SystemClock.uptimeMillis()} blank=$blank")
        return started
    }

    @PublishedApi
    internal fun recordKeyEnd(key: String, blank: Boolean, startedNs: Long, handled: Boolean) {
        val elapsedMs = (SystemClock.elapsedRealtimeNanos() - startedNs) / 1_000_000.0
        Log.d(
            TAG,
            "phase=key_action key=$key lastDispatchEventTimeMs=$lastDispatchEventTimeMs blank=$blank handled=$handled actionMs=$elapsedMs",
        )
    }

    @PublishedApi
    internal fun recordRebuildStart(layer: String, force: Boolean, touching: Boolean, pending: Boolean): Long {
        val started = SystemClock.elapsedRealtimeNanos()
        Log.d(
            TAG,
            "phase=rebuild_begin layer=$layer force=$force touching=$touching pending=$pending " +
                "uptimeMs=${SystemClock.uptimeMillis()}",
        )
        return started
    }

    @PublishedApi
    internal fun recordRebuildEnd(layer: String, startedNs: Long, keyCount: Int, pending: Boolean) {
        val elapsedMs = (SystemClock.elapsedRealtimeNanos() - startedNs) / 1_000_000.0
        Log.d(
            TAG,
            "phase=rebuild_end layer=$layer keyCount=$keyCount pending=$pending rebuildMs=$elapsedMs " +
                "uptimeMs=${SystemClock.uptimeMillis()}",
        )
    }

    @PublishedApi
    internal fun recordGesture(key: String, phase: String, detail: String) {
        Log.d(
            TAG,
            "phase=key_gesture key=$key gesture=$phase lastDispatchEventTimeMs=$lastDispatchEventTimeMs " +
                "uptimeMs=${SystemClock.uptimeMillis()} $detail",
        )
    }

    @PublishedApi
    internal fun recordCandidateTap(index: Int, target: View, scroller: android.widget.HorizontalScrollView) {
        Log.d(
            TAG,
            "phase=candidate_tap index=$index bounds=${bounds(target)} scrollBounds=${bounds(scroller)} " +
                "scrollX=${scroller.scrollX} " +
                "canScrollLeft=${scroller.canScrollHorizontally(-1)} canScrollRight=${scroller.canScrollHorizontally(1)} " +
                "uptimeMs=${SystemClock.uptimeMillis()}",
        )
    }

    private fun bounds(view: View): Bounds {
        val location = IntArray(2)
        view.getLocationOnScreen(location)
        return Bounds(location[0], location[1], location[0] + view.width, location[1] + view.height)
    }

    private fun hit(x: Float, y: Float, keyboard: Bounds, keys: Map<String, View>): String {
        val boxes = keys.map { (id, view) -> KeyBounds(id, bounds(view)) }
        val target = boxes.firstOrNull { it.bounds.contains(x, y) }
        if (target != null) return "Hit=key:${target.id} bounds=${target.bounds}"
        if (!keyboard.contains(x, y)) return "Hit=outside-keyboard bounds=$keyboard"

        val row = boxes.filter { it.bounds.top <= y && y < it.bounds.bottom }
        if (row.isEmpty()) return "Hit=keyboard-gap bounds=$keyboard"
        val left = row.minBy { it.bounds.left }
        val right = row.maxBy { it.bounds.right }
        return when {
            x < left.bounds.left -> "Hit=screen-side-left bounds=${left.id}${left.bounds}"
            x >= right.bounds.right -> "Hit=screen-side-right bounds=${right.id}${right.bounds}"
            else -> {
                val before = row.filter { it.bounds.right <= x }.maxBy { it.bounds.right }
                val after = row.filter { it.bounds.left > x }.minBy { it.bounds.left }
                "Hit=inner-key-gap bounds=${before.id}${before.bounds}|${after.id}${after.bounds}"
            }
        }
    }

    private data class Bounds(val left: Int, val top: Int, val right: Int, val bottom: Int) {
        fun contains(x: Float, y: Float): Boolean = x >= left && x < right && y >= top && y < bottom
        override fun toString(): String = "[$left,$top,$right,$bottom]"
    }

    private data class KeyBounds(val id: String, val bounds: Bounds)
}
