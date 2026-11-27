package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail

class DoctorEventContractTest {
    private val build = DoctorBuildMetadata(
        version = "0.1.0",
        sourceCommit = "e045f978eb52001abc8b632482a6f4536cbdf2ab",
    )

    @Test
    fun `ordinary touch 與 health 逐 byte 符合共享 golden`() {
        assertEquals(
            fixture("ordinary-touch.jsonl"),
            ordinaryEvents.joinToString(separator = "\n", postfix = "\n", transform = ::encodeLine),
        )

        val decoded = DoctorEventCodec.decodeJsonl(fixture("ordinary-touch.jsonl"))
        assertNull(decoded.failure)
        assertEquals(ordinaryEvents, decoded.events.map { it.event })
        assertTrue(decoded.events.all { it.build == build })
    }

    @Test
    fun `敏感與未知 golden 只暴露生命週期及 health 且沒有逐鍵關聯側信道`() {
        listOf(
            "sensitive-lifecycle.jsonl" to sensitiveEvents,
            "unknown-lifecycle.jsonl" to unknownEvents,
        ).forEach { (name, expected) ->
            assertEquals(
                fixture(name),
                expected.joinToString(separator = "\n", postfix = "\n", transform = ::encodeLine),
            )
            val decoded = DoctorEventCodec.decodeJsonl(fixture(name))
            assertNull(decoded.failure)
            assertEquals(expected, decoded.events.map { it.event })
            assertEquals(listOf(1L, 2L), decoded.events.map { it.event.sequence })
            assertTrue(decoded.events.all { it.event.operationId == 0L })
            assertTrue(decoded.events.all { it.event.dropCount == null })
            assertTrue(decoded.events.all { it.event.gapFromSequence == null })
            assertTrue(decoded.events.all { it.event.eventCode in restrictedEventCodes })
            assertTrue(decoded.events.all { it.event.gestureToken == null })
            assertTrue(decoded.events.all { it.event.pointerCount == null })
            assertTrue(decoded.events.all { it.event.pressed == null })
        }

        assertRestrictedRejected(
            sensitiveEvents.first().copy(
                eventCode = DoctorEventCode.KEY_DOWN,
                callbackSource = DoctorCallbackSource.KEY_ACTION,
                operationId = 77,
                phase = DoctorEventPhase.COMPOSITION,
                gestureToken = 91,
                pointerCount = 1,
                pressed = true,
            ),
            DoctorEventFailure.RESTRICTED_INPUT_EVENT,
        )
        assertRestrictedRejected(
            sensitiveEvents.first().copy(operationId = 77),
            DoctorEventFailure.RESTRICTED_CORRELATION,
        )
        assertRestrictedRejected(
            unknownEvents.last().copy(dropCount = 1, gapFromSequence = 1),
            DoctorEventFailure.RESTRICTED_COUNT_FIELD,
        )
    }

    @Test
    fun `受限健康訊號的合法缺號仍保留且不誤判為敏感按鍵`() {
        val jumped = fixture("sensitive-lifecycle.jsonl")
            .replace(
                oldValue = "\"operationId\":0,\"sequence\":2",
                newValue = "\"operationId\":0,\"sequence\":10",
            )
        val decoded = DoctorEventCodec.decodeJsonl(jumped)

        assertEquals(2, decoded.events.size)
        assertNull(decoded.failure)
        assertNull(decoded.failureLine)
    }

    @Test
    fun `P0 Windows 事件值保持原值且 Android 使用獨立的小範圍`() {
        assertEquals(1, DoctorEventCode.CREATED.wireValue)
        assertEquals(7, DoctorEventCode.KEY_DOWN.wireValue)
        assertEquals(8, DoctorEventCode.EDIT_REQUEST.wireValue)
        assertEquals(9, DoctorEventCode.EDIT_APPLIED.wireValue)
        assertEquals(12, DoctorEventCode.SHIFT_TEST_UP.wireValue)
        assertEquals(13, DoctorEventCode.SHIFT_UP.wireValue)
        assertEquals(64, DoctorEventCode.TOUCH.wireValue)
        assertEquals(65, DoctorEventCode.HEALTH.wireValue)
    }

    @Test
    fun `golden 欄位是封閉白名單且每個禁用資料欄都固定拒絕`() {
        val ordinary = fixture("ordinary-touch.jsonl")
        assertEquals(
            setOf(
                "schemaVersion",
                "version",
                "sourceCommit",
                "producerId",
                "sessionId",
                "epoch",
                "operationId",
                "sequence",
                "monotonicNs",
                "wallUtcMs",
                "timezoneOffsetMinutes",
                "eventCode",
                "callbackSource",
                "scope",
                "mode",
                "phase",
                "focus",
                "handled",
                "resultCode",
                "durationUs",
                "gestureToken",
                "pointerCount",
                "pressed",
                "dropCount",
                "gapFromSequence",
            ),
            jsonFieldNames(ordinary),
        )

        val forbiddenFields = listOf(
            "text",
            "rootCode",
            "hash",
            "key",
            "keyCode",
            "keyId",
            "coordinates",
            "coords",
            "candidate",
            "candidateText",
            "surroundingText",
            "path",
            "title",
            "clipboard",
            "exceptionMessage",
        )
        val valid = ordinary.lineSequence().first()
        forbiddenFields.forEach { field ->
            val polluted = valid.dropLast(1) + ",\"$field\":1}"
            assertEquals(
                DoctorEventFailure.UNKNOWN_FIELD,
                DoctorEventCodec.decodeJsonl(polluted + "\n").failure,
                field,
            )
        }

        val nested = valid.replace("\"focus\":true", "\"focus\":{\"key\":1}")
        assertEquals(
            DoctorEventFailure.INVALID_TYPE,
            DoctorEventCodec.decodeJsonl(nested + "\n").failure,
        )
        assertEquals(
            DoctorEventFailure.UNKNOWN_FIELD,
            DoctorEventCodec.decodeJsonl(fixture("bad-schema.jsonl")).failure,
        )
    }

    @Test
    fun `schema enum type 與建置 metadata 錯誤使用固定失敗碼`() {
        val valid = ordinaryEvents.first()
        val line = encodeLine(valid)
        val failures = listOf(
            line.replace("\"schemaVersion\":1", "\"schemaVersion\":2") to
                DoctorEventFailure.UNSUPPORTED_SCHEMA,
            line.replace("\"version\":\"0.1.0\"", "\"version\":\"../../doctor\"") to
                DoctorEventFailure.INVALID_BUILD_METADATA,
            line.replace(
                "\"sourceCommit\":\"e045f978eb52001abc8b632482a6f4536cbdf2ab\"",
                "\"sourceCommit\":\"untrusted\"",
            ) to DoctorEventFailure.INVALID_BUILD_METADATA,
            line.replace("\"eventCode\":64", "\"eventCode\":999") to
                DoctorEventFailure.INVALID_ENUM,
            line.replace("\"handled\":true", "\"handled\":\"true\"") to
                DoctorEventFailure.INVALID_TYPE,
        )

        failures.forEach { (invalid, expected) ->
            assertEquals(expected, DoctorEventCodec.decodeJsonl(invalid + "\n").failure)
        }
    }

    @Test
    fun `最後不完整列保留有效前綴並回報 torn line`() {
        val decoded = DoctorEventCodec.decodeJsonl(fixture("torn-tail.jsonl"))

        assertEquals(listOf(ordinaryEvents.first()), decoded.events.map { it.event })
        assertEquals(DoctorEventFailure.TORN_LINE, decoded.failure)
        assertEquals(2, decoded.failureLine)
    }

    private val ordinaryEvents by lazy {
        listOf(
            event(
                producerId = 9001,
                sessionId = 27,
                epoch = 31,
                operationId = 7001,
                sequence = 1,
                monotonicNs = 5_000_000_000,
                wallUtcMs = 1_764_057_600_000,
                eventCode = DoctorEventCode.TOUCH,
                callbackSource = DoctorCallbackSource.SLOT_FORWARD,
                scope = DoctorInputScope.ORDINARY,
                mode = DoctorEventMode.LIU,
                phase = DoctorEventPhase.TOUCH,
                focus = true,
                handled = true,
                resultCode = DoctorResultCode.OK,
                durationUs = 14,
                gestureToken = 123456,
                pointerCount = 1,
                pressed = true,
            ),
            event(
                producerId = 9001,
                sessionId = 27,
                epoch = 31,
                operationId = 7001,
                sequence = 2,
                monotonicNs = 5_000_000_900,
                wallUtcMs = 1_764_057_600_001,
                eventCode = DoctorEventCode.TOUCH,
                callbackSource = DoctorCallbackSource.KEY_ACTION,
                scope = DoctorInputScope.ORDINARY,
                mode = DoctorEventMode.LIU,
                phase = DoctorEventPhase.TOUCH,
                focus = true,
                handled = true,
                resultCode = DoctorResultCode.OK,
                durationUs = 9,
                gestureToken = 123456,
                pointerCount = 1,
                pressed = false,
            ),
            event(
                producerId = 9001,
                sessionId = 27,
                epoch = 31,
                operationId = 0,
                sequence = 3,
                monotonicNs = 5_000_005_000,
                wallUtcMs = 1_764_057_600_005,
                eventCode = DoctorEventCode.HEALTH,
                callbackSource = DoctorCallbackSource.NONE,
                scope = DoctorInputScope.ORDINARY,
                mode = DoctorEventMode.LIU,
                phase = DoctorEventPhase.HEALTH,
                focus = true,
                handled = true,
                resultCode = DoctorResultCode.OK,
                durationUs = 0,
                dropCount = 2,
                gapFromSequence = 4,
            ),
        )
    }

    private val sensitiveEvents by lazy {
        listOf(
            lifecycleEvent(
                producerId = 9002,
                sessionId = 28,
                epoch = 32,
                scope = DoctorInputScope.SENSITIVE,
                eventCode = DoctorEventCode.CREATED,
                phase = DoctorEventPhase.LIFECYCLE,
                sequence = 1,
                monotonicNs = 6_000_000_000,
                wallUtcMs = 1_764_057_660_000,
            ),
            lifecycleEvent(
                producerId = 9002,
                sessionId = 28,
                epoch = 32,
                scope = DoctorInputScope.SENSITIVE,
                eventCode = DoctorEventCode.HEALTH,
                phase = DoctorEventPhase.HEALTH,
                sequence = 2,
                monotonicNs = 6_000_010_000,
                wallUtcMs = 1_764_057_660_010,
            ),
        )
    }

    private val unknownEvents by lazy {
        listOf(
            lifecycleEvent(
                producerId = 9003,
                sessionId = 29,
                epoch = 33,
                scope = DoctorInputScope.UNKNOWN,
                eventCode = DoctorEventCode.CREATED,
                phase = DoctorEventPhase.LIFECYCLE,
                sequence = 1,
                monotonicNs = 7_000_000_000,
                wallUtcMs = 1_764_057_720_000,
            ),
            lifecycleEvent(
                producerId = 9003,
                sessionId = 29,
                epoch = 33,
                scope = DoctorInputScope.UNKNOWN,
                eventCode = DoctorEventCode.HEALTH,
                phase = DoctorEventPhase.HEALTH,
                sequence = 2,
                monotonicNs = 7_000_010_000,
                wallUtcMs = 1_764_057_720_010,
            ),
        )
    }

    private fun lifecycleEvent(
        producerId: Long,
        sessionId: Long,
        epoch: Long,
        scope: DoctorInputScope,
        eventCode: DoctorEventCode,
        phase: DoctorEventPhase,
        sequence: Long,
        monotonicNs: Long,
        wallUtcMs: Long,
    ) = event(
        producerId = producerId,
        sessionId = sessionId,
        epoch = epoch,
        operationId = 0,
        sequence = sequence,
        monotonicNs = monotonicNs,
        wallUtcMs = wallUtcMs,
        eventCode = eventCode,
        callbackSource = DoctorCallbackSource.NONE,
        scope = scope,
        mode = DoctorEventMode.NONE,
        phase = phase,
        focus = false,
        handled = true,
        resultCode = DoctorResultCode.OK,
        durationUs = 0,
    )

    private fun event(
        producerId: Long,
        sessionId: Long,
        epoch: Long,
        operationId: Long,
        sequence: Long,
        monotonicNs: Long,
        wallUtcMs: Long,
        eventCode: DoctorEventCode,
        callbackSource: DoctorCallbackSource,
        scope: DoctorInputScope,
        mode: DoctorEventMode,
        phase: DoctorEventPhase,
        focus: Boolean,
        handled: Boolean,
        resultCode: DoctorResultCode,
        durationUs: Long,
        gestureToken: Long? = null,
        pointerCount: Int? = null,
        pressed: Boolean? = null,
        dropCount: Long? = null,
        gapFromSequence: Long? = null,
    ) = DoctorEventRecord(
        producerId = producerId,
        sessionId = sessionId,
        epoch = epoch,
        operationId = operationId,
        sequence = sequence,
        monotonicNs = monotonicNs,
        wallUtcMs = wallUtcMs,
        timezoneOffsetMinutes = 480,
        eventCode = eventCode,
        callbackSource = callbackSource,
        scope = scope,
        mode = mode,
        phase = phase,
        focus = focus,
        handled = handled,
        resultCode = resultCode,
        durationUs = durationUs,
        gestureToken = gestureToken,
        pointerCount = pointerCount,
        pressed = pressed,
        dropCount = dropCount,
        gapFromSequence = gapFromSequence,
    )

    private fun encodeLine(event: DoctorEventRecord): String =
        when (val result = DoctorEventCodec.encodeLine(build, event)) {
            is DoctorEventEncode.Accepted -> result.line
            is DoctorEventEncode.Rejected -> fail("event rejected: ${result.reason}")
        }

    private fun assertRestrictedRejected(event: DoctorEventRecord, expected: DoctorEventFailure) {
        when (val result = DoctorEventCodec.encodeLine(build, event)) {
            is DoctorEventEncode.Accepted -> fail("restricted event was accepted: ${result.line}")
            is DoctorEventEncode.Rejected -> assertEquals(expected, result.reason)
        }
    }

    private fun fixture(name: String): String =
        requireNotNull(javaClass.getResource("/doctor-v1/$name")) {
            "missing fixture doctor-v1/$name"
        }.readText()

    private fun jsonFieldNames(json: String): Set<String> =
        Regex("\"([A-Za-z][A-Za-z0-9]*)\"\\s*:")
            .findAll(json)
            .map { it.groupValues[1] }
            .toSet()

    private companion object {
        val restrictedEventCodes = setOf(
            DoctorEventCode.CREATED,
            DoctorEventCode.ACTIVATE_BEGIN,
            DoctorEventCode.ACTIVATE_END,
            DoctorEventCode.FOCUS,
            DoctorEventCode.DEACTIVATE,
            DoctorEventCode.HEALTH,
        )
    }
}
