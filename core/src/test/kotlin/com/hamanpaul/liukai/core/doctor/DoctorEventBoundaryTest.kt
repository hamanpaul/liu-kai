package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.fail

class DoctorEventBoundaryTest {
    @Test
    fun `build metadata 邊界在 encode 與 decode 都固定拒絕`() {
        val event = baseEvent
        val invalid = listOf(
            DoctorBuildMetadata("", build.sourceCommit),
            DoctorBuildMetadata(".0.1.0", build.sourceCommit),
            DoctorBuildMetadata("0.1.0/", build.sourceCommit),
            DoctorBuildMetadata("a".repeat(129), build.sourceCommit),
            DoctorBuildMetadata(build.version, "abcdef"),
            DoctorBuildMetadata(build.version, "a".repeat(41)),
            DoctorBuildMetadata(build.version, "g".repeat(7)),
        )
        invalid.forEach { metadata ->
            assertEquals(
                DoctorEventEncode.Rejected(DoctorEventFailure.INVALID_BUILD_METADATA),
                DoctorEventCodec.encodeLine(metadata, event),
            )
        }

        val validBoundaries = listOf(
            DoctorBuildMetadata("0", "abcdef0"),
            DoctorBuildMetadata(
                "A" + "z".repeat(127),
                "ABCDEF0123456789ABCDEF0123456789ABCDEF01",
            ),
        )
        validBoundaries.forEach { metadata ->
            val encoded = encode(metadata, event)
            val decoded = DoctorEventCodec.decodeJsonl("$encoded\n")
            assertNull(decoded.failure)
            assertEquals(metadata, decoded.events.single().build)
        }
    }

    @Test
    fun `事件數值接受 signed 邊界且逐欄拒絕負值或範圍外值`() {
        val zeroAndMaximum = baseEvent.copy(
            producerId = Long.MAX_VALUE,
            sessionId = Long.MAX_VALUE,
            epoch = 0,
            operationId = 0,
            sequence = 0,
            monotonicNs = Long.MAX_VALUE,
            wallUtcMs = Long.MAX_VALUE,
            timezoneOffsetMinutes = -12 * 60,
            durationUs = Long.MAX_VALUE,
            gestureToken = Long.MAX_VALUE,
            pointerCount = Int.MAX_VALUE,
            dropCount = Long.MAX_VALUE,
            gapFromSequence = Long.MAX_VALUE,
        )
        assertEquals(
            zeroAndMaximum,
            DoctorEventCodec.decodeJsonl("${encode(zeroAndMaximum)}\n").events.single().event,
        )
        val positiveTimezone = zeroAndMaximum.copy(timezoneOffsetMinutes = 14 * 60)
        assertEquals(
            positiveTimezone,
            DoctorEventCodec.decodeJsonl("${encode(positiveTimezone)}\n").events.single().event,
        )

        val invalid = listOf(
            baseEvent.copy(producerId = 0),
            baseEvent.copy(producerId = -1),
            baseEvent.copy(sessionId = 0),
            baseEvent.copy(sessionId = -1),
            baseEvent.copy(epoch = -1),
            baseEvent.copy(operationId = -1),
            baseEvent.copy(sequence = -1),
            baseEvent.copy(monotonicNs = -1),
            baseEvent.copy(wallUtcMs = -1),
            baseEvent.copy(timezoneOffsetMinutes = -12 * 60 - 1),
            baseEvent.copy(timezoneOffsetMinutes = 14 * 60 + 1),
            baseEvent.copy(durationUs = -1),
            baseEvent.copy(gestureToken = -1),
            baseEvent.copy(pointerCount = -1),
            baseEvent.copy(dropCount = -1),
            baseEvent.copy(gapFromSequence = -1),
        )
        invalid.forEach {
            assertEquals(
                DoctorEventEncode.Rejected(DoctorEventFailure.INVALID_VALUE),
                DoctorEventCodec.encodeLine(build, it),
            )
        }
    }

    @Test
    fun `touch 三欄齊全且非 touch 不可攜帶逐指欄`() {
        listOf(
            baseEvent.copy(gestureToken = null),
            baseEvent.copy(pointerCount = null),
            baseEvent.copy(pressed = null),
        ).forEach {
            assertEquals(
                DoctorEventEncode.Rejected(DoctorEventFailure.INVALID_TYPE),
                DoctorEventCodec.encodeLine(build, it),
            )
        }

        val ordinaryNonTouch = baseEvent.copy(
            eventCode = DoctorEventCode.EDIT_REQUEST,
            phase = DoctorEventPhase.COMPOSITION,
            gestureToken = null,
            pointerCount = null,
            pressed = null,
        )
        listOf(
            ordinaryNonTouch.copy(gestureToken = 1),
            ordinaryNonTouch.copy(pointerCount = 1),
            ordinaryNonTouch.copy(pressed = true),
        ).forEach {
            assertEquals(
                DoctorEventEncode.Rejected(DoctorEventFailure.INVALID_TYPE),
                DoctorEventCodec.encodeLine(build, it),
            )
        }
    }

    @Test
    fun `受限 scope 只接受固定生命週期 health 並拒絕關聯與 count`() {
        DoctorInputScope.entries.filter { it != DoctorInputScope.ORDINARY }.forEach { scope ->
            listOf(
                DoctorEventCode.CREATED,
                DoctorEventCode.ACTIVATE_BEGIN,
                DoctorEventCode.ACTIVATE_END,
                DoctorEventCode.FOCUS,
                DoctorEventCode.DEACTIVATE,
            ).forEach { code ->
                val event = restrictedEvent(scope, code, DoctorEventPhase.LIFECYCLE)
                assertRoundTrip(event)
            }
            assertRoundTrip(
                restrictedEvent(scope, DoctorEventCode.HEALTH, DoctorEventPhase.HEALTH),
            )
        }

        val health = restrictedEvent(
            DoctorInputScope.SENSITIVE,
            DoctorEventCode.HEALTH,
            DoctorEventPhase.HEALTH,
        )
        listOf(
            health.copy(eventCode = DoctorEventCode.KEY_DOWN),
            health.copy(callbackSource = DoctorCallbackSource.DISPATCH),
            health.copy(phase = DoctorEventPhase.LIFECYCLE),
            restrictedEvent(
                DoctorInputScope.UNKNOWN,
                DoctorEventCode.CREATED,
                DoctorEventPhase.HEALTH,
            ),
            health.copy(gestureToken = 1),
            health.copy(pointerCount = 1),
            health.copy(pressed = true),
        ).forEach {
            assertEquals(
                DoctorEventEncode.Rejected(DoctorEventFailure.RESTRICTED_INPUT_EVENT),
                DoctorEventCodec.encodeLine(build, it),
            )
        }
        assertEquals(
            DoctorEventEncode.Rejected(DoctorEventFailure.RESTRICTED_CORRELATION),
            DoctorEventCodec.encodeLine(build, health.copy(operationId = 1)),
        )
        listOf(
            health.copy(dropCount = 1),
            health.copy(gapFromSequence = 1),
        ).forEach {
            assertEquals(
                DoctorEventEncode.Rejected(DoctorEventFailure.RESTRICTED_COUNT_FIELD),
                DoctorEventCodec.encodeLine(build, it),
            )
        }
    }

    @Test
    fun `decode 對全部 required field 與公開 enum 型別使用固定失敗碼`() {
        val line = encodeLine()
        requiredFields.forEach { field ->
            assertEquals(
                DoctorEventFailure.INVALID_TYPE,
                decodeFailure(withoutField(line, field)),
                "missing $field",
            )
        }

        listOf(
            "schemaVersion" to "2147483648",
            "eventCode" to "0",
            "callbackSource" to "4",
            "scope" to "3",
            "mode" to "3",
            "phase" to "7",
            "resultCode" to "5",
        ).forEach { (field, value) ->
            val expected = when (field) {
                "schemaVersion" -> DoctorEventFailure.INVALID_TYPE
                else -> DoctorEventFailure.INVALID_ENUM
            }
            assertEquals(expected, decodeFailure(replaceField(line, field, value)), field)
        }

        listOf(
            "schemaVersion" to "\"1\"",
            "producerId" to "\"9001\"",
            "sessionId" to "true",
            "timezoneOffsetMinutes" to "2147483648",
            "eventCode" to "false",
            "focus" to "1",
            "handled" to "\"true\"",
            "version" to "1",
            "sourceCommit" to "false",
            "gestureToken" to "\"token\"",
            "pointerCount" to "2147483648",
            "pressed" to "0",
        ).forEach { (field, value) ->
            assertEquals(
                DoctorEventFailure.INVALID_TYPE,
                decodeFailure(replaceField(line, field, value)),
                field,
            )
        }
    }

    @Test
    fun `optional field 的缺席與錯誤 JSON 型別可區分`() {
        val line = encodeLine()
        listOf("gestureToken", "pointerCount", "pressed").forEach { field ->
            assertEquals(
                DoctorEventFailure.INVALID_TYPE,
                decodeFailure(withoutField(line, field)),
                field,
            )
        }

        val optional = encodeLine(
            baseEvent.copy(dropCount = 7, gapFromSequence = 3, platformResultCode = 9),
        )
        listOf(
            "dropCount" to "\"7\"",
            "gapFromSequence" to "true",
            "platformResultCode" to "{}",
        ).forEach { (field, value) ->
            assertEquals(
                DoctorEventFailure.INVALID_TYPE,
                decodeFailure(replaceField(optional, field, value)),
                field,
            )
        }
    }

    @Test
    fun `JSON 物件字串數字與 literal parser 邊界固定分類`() {
        val line = encodeLine()
        val invalidJson = listOf(
            "",
            "[${line}]",
            "{",
            "$line trailing",
            line.dropLast(1) + ",\"producerId\":9001}",
            line.replace("\"producerId\":9001,", "\"producerId\":9001"),
            line.dropLast(1) + ",}",
            line.replace("\"focus\":true", "\"focus\":tru"),
            line.replace("\"handled\":true", "\"handled\":fals"),
            line.replace("\"version\":\"0.1.0\"", "\"version\":\"unterminated"),
            line.replace("\"version\":\"0.1.0\"", "\"version\":\"bad\\x\""),
            line.replace("\"version\":\"0.1.0\"", "\"version\":\"bad\\u12\""),
            line.replace("\"version\":\"0.1.0\"", "\"version\":\"bad\\uZZZZ\""),
            "{\"x\":\"ends-with-backslash\\",
            line.replace("\"producerId\":9001", "\"producerId\":+1"),
            line.replace("\"producerId\":9001", "\"producerId\":01"),
            line.replace("\"producerId\":9001", "\"producerId\":9001e2"),
            line.replace("\"producerId\":9001", "\"producerId\":9223372036854775808"),
        )
        invalidJson.forEach { text ->
            val failure = DoctorEventCodec.decodeJsonl("$text\n").failure
            assertTrue(
                failure == DoctorEventFailure.INVALID_JSON ||
                    failure == DoctorEventFailure.INVALID_TYPE ||
                    failure == DoctorEventFailure.TORN_LINE,
                "unexpected $failure for $text",
            )
        }

        val typeValues = listOf(
            line.replace("\"focus\":true", "\"focus\":{}"),
            line.replace("\"focus\":true", "\"focus\":[]"),
            line.replace("\"focus\":true", "\"focus\":null"),
        )
        typeValues.forEach {
            assertEquals(DoctorEventFailure.INVALID_TYPE, decodeFailure(it))
        }

        assertEquals(DoctorEventFailure.TORN_LINE, DoctorEventCodec.decodeJsonl("{\"schemaVersion\":1").failure)
        assertNull(DoctorEventCodec.decodeJsonl("").failure)
        assertTrue(DoctorEventCodec.decodeJsonl("{}\n").events.isEmpty())
        assertEquals(DoctorEventFailure.INVALID_TYPE, decodeFailure("{}"))
    }

    @Test
    fun `JSON escape whitespace CRLF 與 signed long token 都能解析`() {
        val line = encodeLine()
        val escapedBuild = DoctorBuildMetadata("0.1.0", build.sourceCommit)
        val unicode = line.replace("\"version\":\"0.1.0\"", "\"version\":\"\\u0030.1.0\"")
        val decoded = DoctorEventCodec.decodeJsonl("\t $unicode \r\n")
        assertNull(decoded.failure)
        assertEquals(escapedBuild, decoded.events.single().build)

        listOf("\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t").forEach { escape ->
            val polluted = line.replace(
                "\"version\":\"0.1.0\"",
                "\"version\":\"a${escape}b\"",
            )
            assertEquals(
                DoctorEventFailure.INVALID_BUILD_METADATA,
                decodeFailure(polluted),
                escape,
            )
        }

        val negative = replaceField(line, "operationId", "-0")
        assertEquals(
            0L,
            DoctorEventCodec.decodeJsonl("$negative\n").events.single().event.operationId,
        )
        assertEquals(
            DoctorEventFailure.INVALID_VALUE,
            decodeFailure(replaceField(line, "operationId", Long.MIN_VALUE.toString())),
        )
    }

    private val build = DOCTOR_TEST_BUILD
    private val baseEvent by lazy {
        DoctorEventCodec.decodeJsonl(
            doctorFixture("ordinary-touch.jsonl"),
        ).events.first().event
    }

    private fun restrictedEvent(
        scope: DoctorInputScope,
        eventCode: DoctorEventCode,
        phase: DoctorEventPhase,
    ) = baseEvent.copy(
        operationId = 0,
        eventCode = eventCode,
        callbackSource = DoctorCallbackSource.NONE,
        scope = scope,
        mode = DoctorEventMode.NONE,
        phase = phase,
        gestureToken = null,
        pointerCount = null,
        pressed = null,
        dropCount = null,
        gapFromSequence = null,
    )

    private fun assertRoundTrip(event: DoctorEventRecord) {
        val encoded = encode(event)
        val decoded = DoctorEventCodec.decodeJsonl("$encoded\n")
        assertNull(decoded.failure)
        assertEquals(event, decoded.events.single().event)
    }

    private fun encodeLine(event: DoctorEventRecord = baseEvent): String = encode(build, event)

    private fun encode(metadata: DoctorBuildMetadata, event: DoctorEventRecord): String =
        when (val encoded = DoctorEventCodec.encodeLine(metadata, event)) {
            is DoctorEventEncode.Accepted -> encoded.line
            is DoctorEventEncode.Rejected -> fail("unexpected encode rejection: ${encoded.reason}")
        }

    private fun encode(event: DoctorEventRecord): String = encode(build, event)

    @Test
    fun `malformed complete and truncated parser inputs are distinguished from a final torn line`() {
        val valid = doctorFixture("ordinary-touch.jsonl").trimEnd('\n')
        val malformed = listOf(
            "{}x" to DoctorEventFailure.INVALID_JSON,
            "{\"x\":" to DoctorEventFailure.INVALID_JSON,
            "{\"x\":-" to DoctorEventFailure.INVALID_JSON,
            "{\"x\":x}" to DoctorEventFailure.INVALID_TYPE,
            "{\"x\":1x}" to DoctorEventFailure.INVALID_TYPE,
            "{\"x\":\"\u0001\"}" to DoctorEventFailure.INVALID_JSON,
            "{\"x\":\"\\u0" to DoctorEventFailure.INVALID_JSON,
            "{\"key" to DoctorEventFailure.INVALID_JSON,
        )
        malformed.forEach { (raw, expected) ->
            assertEquals(expected, DoctorEventCodec.decodeJsonl("$raw\n$valid\n").failure)
        }
        val spaced = valid.replace(",", " , ").replace("}", " }")
        assertEquals(null, DoctorEventCodec.decodeJsonl("$spaced\n").failure)
    }

    private fun decodeFailure(line: String): DoctorEventFailure? =
        DoctorEventCodec.decodeJsonl("$line\n").failure

    private fun replaceField(line: String, field: String, value: String): String =
        Regex("\"${Regex.escape(field)}\":[^,}]+").replace(line) { match ->
            "\"$field\":$value"
        }

    private fun withoutField(line: String, field: String): String =
        Regex("(,\"${Regex.escape(field)}\":[^,}]+|\"${Regex.escape(field)}\":[^,}]+,?)")
            .replaceFirst(line, "")

    private companion object {
        val requiredFields = listOf(
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
        )
    }
}
