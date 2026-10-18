package com.hamanpaul.liukai.ime

import android.view.MotionEvent
import android.view.View
import android.content.Context
import android.widget.FrameLayout

/** Release-source no-ops: inline call sites erase trace work, strings, and Android Log references. */
internal object TouchDiagnostics {
    internal inline fun dispatchEvent(event: MotionEvent, root: View, keyboard: View, keys: Map<String, View>) = Unit
    internal inline fun startDispatch(): Long = 0L
    internal inline fun endDispatch(event: MotionEvent, startedNs: Long, handled: Boolean) = Unit
    internal inline fun slotForward(
        key: String,
        slot: View,
        keyView: View,
        event: MotionEvent,
        startedNs: Long,
        handled: Boolean,
    ) = Unit
    internal inline fun beginKeyAction(key: String, blank: Boolean): Long = 0L
    internal inline fun endKeyAction(key: String, blank: Boolean, startedNs: Long, handled: Boolean) = Unit
    internal inline fun beginRebuild(layer: String, force: Boolean, touching: Boolean, pending: Boolean): Long = 0L
    internal inline fun endRebuild(layer: String, startedNs: Long, keyCount: Int, pending: Boolean) = Unit
    internal inline fun gesture(key: String, phase: String, detail: String) = Unit
    internal inline fun candidateTap(index: Int, target: View, scroller: android.widget.HorizontalScrollView) = Unit
    internal inline fun keyFrame(context: Context, key: String): FrameLayout = FrameLayout(context)
}
