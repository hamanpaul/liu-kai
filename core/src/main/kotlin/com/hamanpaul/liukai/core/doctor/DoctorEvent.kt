package com.hamanpaul.liukai.core.doctor

data class DoctorBuildMetadata(
    val version: String,
    val sourceCommit: String,
)

enum class DoctorEventCode(val wireValue: Int) {
    CREATED(1),
    ACTIVATE_BEGIN(2),
    ACTIVATE_END(3),
    KEY_SINK_ADVICE(4),
    FOCUS(5),
    TEST_DOWN(6),
    KEY_DOWN(7),
    EDIT_REQUEST(8),
    EDIT_APPLIED(9),
    DEACTIVATE(10),
    EXTENDED_INTERFACE_QUERY(11),
    SHIFT_TEST_UP(12),
    SHIFT_UP(13),
    TOUCH(64),
    HEALTH(65),
}

enum class DoctorCallbackSource(val wireValue: Int) {
    NONE(0),
    DISPATCH(1),
    SLOT_FORWARD(2),
    KEY_ACTION(3),
}

enum class DoctorEventMode(val wireValue: Int) {
    NONE(0),
    LIU(1),
    ENGLISH(2),
}

enum class DoctorEventPhase(val wireValue: Int) {
    NONE(0),
    TOUCH(1),
    COMPOSITION(2),
    CANDIDATE(3),
    COMMIT(4),
    HEALTH(5),
    LIFECYCLE(6),
}

enum class DoctorResultCode(val wireValue: Int) {
    NONE(0),
    OK(1),
    REJECTED(2),
    UNAVAILABLE(3),
    FAILED(4),
}

enum class DoctorEventFailure {
    UNKNOWN_FIELD,
    INVALID_TYPE,
    INVALID_JSON,
    INVALID_VALUE,
    UNSUPPORTED_SCHEMA,
    INVALID_BUILD_METADATA,
    INVALID_ENUM,
    TORN_LINE,
    RESTRICTED_INPUT_EVENT,
    RESTRICTED_CORRELATION,
    RESTRICTED_COUNT_FIELD,
    RESTRICTED_SEQUENCE_GAP,
}

data class DoctorEventRecord(
    val producerId: Long,
    val sessionId: Long,
    val epoch: Long,
    val operationId: Long,
    val sequence: Long,
    val monotonicNs: Long,
    val wallUtcMs: Long,
    val timezoneOffsetMinutes: Int,
    val eventCode: DoctorEventCode,
    val callbackSource: DoctorCallbackSource,
    val scope: DoctorInputScope,
    val mode: DoctorEventMode,
    val phase: DoctorEventPhase,
    val focus: Boolean,
    val handled: Boolean,
    val resultCode: DoctorResultCode,
    val durationUs: Long,
    val gestureToken: Long? = null,
    val pointerCount: Int? = null,
    val pressed: Boolean? = null,
    val dropCount: Long? = null,
    val gapFromSequence: Long? = null,
    val platformResultCode: Int? = null,
)

sealed interface DoctorEventEncode {
    data class Accepted(val line: String) : DoctorEventEncode
    data class Rejected(val reason: DoctorEventFailure) : DoctorEventEncode
}

data class DoctorEventEnvelope(
    val build: DoctorBuildMetadata,
    val event: DoctorEventRecord,
)

data class DoctorJsonlDecode(
    val events: List<DoctorEventEnvelope>,
    val failure: DoctorEventFailure?,
    val failureLine: Int?,
)

object DoctorEventCodec {
    const val SCHEMA_VERSION = 1

    private val buildVersionPattern = Regex("[0-9A-Za-z][0-9A-Za-z.+_-]{0,127}")
    private val sourceCommitPattern = Regex("[0-9a-fA-F]{7,40}")
    private val allowedFields = setOf(
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
        "platformResultCode",
    )
    private val requiredFields = setOf(
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
    private val restrictedEventCodes = setOf(
        DoctorEventCode.CREATED,
        DoctorEventCode.ACTIVATE_BEGIN,
        DoctorEventCode.ACTIVATE_END,
        DoctorEventCode.FOCUS,
        DoctorEventCode.DEACTIVATE,
        DoctorEventCode.HEALTH,
    )

    fun encodeLine(build: DoctorBuildMetadata, event: DoctorEventRecord): DoctorEventEncode {
        if (!validBuild(build)) {
            return DoctorEventEncode.Rejected(DoctorEventFailure.INVALID_BUILD_METADATA)
        }
        validateEvent(event)?.let { return DoctorEventEncode.Rejected(it) }

        return DoctorEventEncode.Accepted(
            buildString {
                append("{\"schemaVersion\":").append(SCHEMA_VERSION)
                append(",\"version\":\"").append(build.version).append('"')
                append(",\"sourceCommit\":\"").append(build.sourceCommit).append('"')
                append(",\"producerId\":").append(event.producerId)
                append(",\"sessionId\":").append(event.sessionId)
                append(",\"epoch\":").append(event.epoch)
                append(",\"operationId\":").append(event.operationId)
                append(",\"sequence\":").append(event.sequence)
                append(",\"monotonicNs\":").append(event.monotonicNs)
                append(",\"wallUtcMs\":").append(event.wallUtcMs)
                append(",\"timezoneOffsetMinutes\":").append(event.timezoneOffsetMinutes)
                append(",\"eventCode\":").append(event.eventCode.wireValue)
                append(",\"callbackSource\":").append(event.callbackSource.wireValue)
                append(",\"scope\":").append(scopeWireValue(event.scope))
                append(",\"mode\":").append(event.mode.wireValue)
                append(",\"phase\":").append(event.phase.wireValue)
                append(",\"focus\":").append(event.focus)
                append(",\"handled\":").append(event.handled)
                append(",\"resultCode\":").append(event.resultCode.wireValue)
                append(",\"durationUs\":").append(event.durationUs)
                event.gestureToken?.let { append(",\"gestureToken\":").append(it) }
                event.pointerCount?.let { append(",\"pointerCount\":").append(it) }
                event.pressed?.let { append(",\"pressed\":").append(it) }
                event.dropCount?.let { append(",\"dropCount\":").append(it) }
                event.gapFromSequence?.let { append(",\"gapFromSequence\":").append(it) }
                event.platformResultCode?.let { append(",\"platformResultCode\":").append(it) }
                append('}')
            },
        )
    }

    fun decodeJsonl(text: String): DoctorJsonlDecode {
        val events = mutableListOf<DoctorEventEnvelope>()
        var start = 0
        var lineNumber = 1
        while (start < text.length) {
            val newline = text.indexOf('\n', start)
            val end = if (newline >= 0) newline else text.length
            val nextStart = if (newline >= 0) newline + 1 else end
            val rawLine = text.substring(start, end).removeSuffix("\r")
            val decoded = decodeLine(rawLine)
            if (decoded is DecodedLine.Rejected) {
                val failure = if (
                    nextStart >= text.length &&
                    !rawLine.trimEnd().endsWith('}')
                ) {
                    DoctorEventFailure.TORN_LINE
                } else {
                    decoded.reason
                }
                return DoctorJsonlDecode(events, failure, lineNumber)
            }

            val envelope = (decoded as DecodedLine.Accepted).value
            events += envelope
            start = nextStart
            lineNumber += 1
        }
        return DoctorJsonlDecode(events, failure = null, failureLine = null)
    }

    private fun decodeLine(text: String): DecodedLine {
        val fields = try {
            DoctorJsonParser(text).parseObject()
        } catch (failure: DoctorJsonParseFailure) {
            return DecodedLine.Rejected(failure.reason)
        }
        val unknown = fields.keys.firstOrNull { it !in allowedFields }
        if (unknown != null) return DecodedLine.Rejected(DoctorEventFailure.UNKNOWN_FIELD)
        if (!requiredFields.all(fields::containsKey)) {
            return DecodedLine.Rejected(DoctorEventFailure.INVALID_TYPE)
        }

        return try {
            val schemaVersion = fields.int("schemaVersion")
            if (schemaVersion != SCHEMA_VERSION) {
                return DecodedLine.Rejected(DoctorEventFailure.UNSUPPORTED_SCHEMA)
            }
            val build = DoctorBuildMetadata(
                version = fields.string("version"),
                sourceCommit = fields.string("sourceCommit"),
            )
            if (!validBuild(build)) {
                return DecodedLine.Rejected(DoctorEventFailure.INVALID_BUILD_METADATA)
            }
            val event = DoctorEventRecord(
                producerId = fields.long("producerId"),
                sessionId = fields.long("sessionId"),
                epoch = fields.long("epoch"),
                operationId = fields.long("operationId"),
                sequence = fields.long("sequence"),
                monotonicNs = fields.long("monotonicNs"),
                wallUtcMs = fields.long("wallUtcMs"),
                timezoneOffsetMinutes = fields.int("timezoneOffsetMinutes"),
                eventCode = eventEnum<DoctorEventCode>(
                    fields.int("eventCode"),
                    DoctorEventCode.entries,
                    DoctorEventCode::wireValue,
                ),
                callbackSource = eventEnum<DoctorCallbackSource>(
                    fields.int("callbackSource"),
                    DoctorCallbackSource.entries,
                    DoctorCallbackSource::wireValue,
                ),
                scope = when (fields.int("scope")) {
                    0 -> DoctorInputScope.UNKNOWN
                    1 -> DoctorInputScope.ORDINARY
                    2 -> DoctorInputScope.SENSITIVE
                    else -> throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_ENUM)
                },
                mode = eventEnum<DoctorEventMode>(
                    fields.int("mode"),
                    DoctorEventMode.entries,
                    DoctorEventMode::wireValue,
                ),
                phase = eventEnum<DoctorEventPhase>(
                    fields.int("phase"),
                    DoctorEventPhase.entries,
                    DoctorEventPhase::wireValue,
                ),
                focus = fields.boolean("focus"),
                handled = fields.boolean("handled"),
                resultCode = eventEnum<DoctorResultCode>(
                    fields.int("resultCode"),
                    DoctorResultCode.entries,
                    DoctorResultCode::wireValue,
                ),
                durationUs = fields.long("durationUs"),
                gestureToken = fields.optionalLong("gestureToken"),
                pointerCount = fields.optionalInt("pointerCount"),
                pressed = fields.optionalBoolean("pressed"),
                dropCount = fields.optionalLong("dropCount"),
                gapFromSequence = fields.optionalLong("gapFromSequence"),
                platformResultCode = fields.optionalInt("platformResultCode"),
            )
            validateEvent(event)?.let {
                return DecodedLine.Rejected(it)
            }
            DecodedLine.Accepted(DoctorEventEnvelope(build, event))
        } catch (failure: DoctorJsonParseFailure) {
            DecodedLine.Rejected(failure.reason)
        }
    }

    private fun validateEvent(event: DoctorEventRecord): DoctorEventFailure? {
        if (event.scope != DoctorInputScope.ORDINARY) {
            if (
                event.eventCode !in restrictedEventCodes ||
                event.callbackSource != DoctorCallbackSource.NONE ||
                event.phase != if (event.eventCode == DoctorEventCode.HEALTH) {
                    DoctorEventPhase.HEALTH
                } else {
                    DoctorEventPhase.LIFECYCLE
                } ||
                event.gestureToken != null ||
                event.pointerCount != null ||
                event.pressed != null
            ) {
                return DoctorEventFailure.RESTRICTED_INPUT_EVENT
            }
            if (event.operationId != 0L) {
                return DoctorEventFailure.RESTRICTED_CORRELATION
            }
            if (event.dropCount != null || event.gapFromSequence != null) {
                return DoctorEventFailure.RESTRICTED_COUNT_FIELD
            }
        }

        if (
            event.producerId <= 0L ||
            event.sessionId <= 0L ||
            event.epoch < 0L ||
            event.operationId < 0L ||
            event.sequence < 0L ||
            event.monotonicNs < 0L ||
            event.wallUtcMs < 0L ||
            event.timezoneOffsetMinutes !in -12 * 60..14 * 60 ||
            event.durationUs < 0L ||
            event.gestureToken?.let { it < 0L } == true ||
            event.pointerCount?.let { it < 0 } == true ||
            event.dropCount?.let { it < 0L } == true ||
            event.gapFromSequence?.let { it < 0L } == true
        ) {
            return DoctorEventFailure.INVALID_VALUE
        }
        if (event.eventCode == DoctorEventCode.TOUCH) {
            if (
                event.gestureToken == null ||
                event.pointerCount == null ||
                event.pressed == null
            ) {
                return DoctorEventFailure.INVALID_TYPE
            }
        } else if (
            event.gestureToken != null ||
            event.pointerCount != null ||
            event.pressed != null
        ) {
            return DoctorEventFailure.INVALID_TYPE
        }
        return null
    }

    internal fun validBuild(build: DoctorBuildMetadata): Boolean =
        build.version.matches(buildVersionPattern) &&
            build.sourceCommit.matches(sourceCommitPattern)

    private fun scopeWireValue(scope: DoctorInputScope): Int =
        when (scope) {
            DoctorInputScope.UNKNOWN -> 0
            DoctorInputScope.ORDINARY -> 1
            DoctorInputScope.SENSITIVE -> 2
        }

    private inline fun <reified T : Enum<T>> eventEnum(
        wireValue: Int,
        values: List<T>,
        wire: (T) -> Int,
    ): T = values.firstOrNull { wire(it) == wireValue }
        ?: throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_ENUM)

    private sealed interface DecodedLine {
        data class Accepted(val value: DoctorEventEnvelope) : DecodedLine
        data class Rejected(val reason: DoctorEventFailure) : DecodedLine
    }
}

internal sealed interface DoctorJsonValue

internal data class DoctorJsonString(val value: String) : DoctorJsonValue

internal data class DoctorJsonLong(val value: Long) : DoctorJsonValue

private data class DoctorJsonBoolean(val value: Boolean) : DoctorJsonValue

internal class DoctorJsonParser(private val input: String) {
    private var offset = 0

    fun parseObject(): Map<String, DoctorJsonValue> {
        whitespace()
        expect('{')
        whitespace()
        val result = linkedMapOf<String, DoctorJsonValue>()
        if (take('}')) {
            whitespace()
            if (offset != input.length) throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
            return result
        }
        while (true) {
            whitespace()
            val key = parseString()
            whitespace()
            expect(':')
            whitespace()
            val value = parseValue()
            if (result.put(key, value) != null) {
                throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
            }
            whitespace()
            if (take('}')) break
            expect(',')
        }
        whitespace()
        if (offset != input.length) throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
        return result
    }

    private fun parseValue(): DoctorJsonValue =
        when {
            offset >= input.length -> throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
            input[offset] == '"' -> DoctorJsonString(parseString())
            input[offset] == 't' -> {
                literal("true")
                DoctorJsonBoolean(true)
            }
            input[offset] == 'f' -> {
                literal("false")
                DoctorJsonBoolean(false)
            }
            input[offset] == '{' || input[offset] == '[' ->
                throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)
            input[offset] == 'n' ->
                throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)
            else -> parseLong()
        }

    private fun parseLong(): DoctorJsonLong {
        val start = offset
        if (input[offset] == '-') offset += 1
        if (offset >= input.length) throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
        if (input[offset] == '0') {
            offset += 1
        } else {
            if (input[offset] !in '1'..'9') {
                throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)
            }
            while (offset < input.length && input[offset].isDigit()) offset += 1
        }
        if (
            offset < input.length &&
            input[offset] != ',' &&
            input[offset] != '}' &&
            !input[offset].isWhitespace()
        ) {
            throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)
        }
        val value = input.substring(start, offset).toLongOrNull()
            ?: throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)
        return DoctorJsonLong(value)
    }

    private fun parseString(): String {
        expect('"')
        val value = StringBuilder()
        while (offset < input.length) {
            val char = input[offset++]
            when (char) {
                '"' -> return value.toString()
                '\\' -> {
                    if (offset >= input.length) {
                        throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
                    }
                    when (val escaped = input[offset++]) {
                        '"', '\\', '/' -> value.append(escaped)
                        'b' -> value.append('\b')
                        'f' -> value.append('\u000C')
                        'n' -> value.append('\n')
                        'r' -> value.append('\r')
                        't' -> value.append('\t')
                        'u' -> value.append(unicodeEscape())
                        else -> throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
                    }
                }
                else -> {
                    if (char < ' ') {
                        throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
                    }
                    value.append(char)
                }
            }
        }
        throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
    }

    private fun unicodeEscape(): Char {
        if (offset + 4 > input.length) {
            throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
        }
        var value = 0
        repeat(4) {
            val digit = input[offset++].digitToIntOrNull(16)
                ?: throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
            value = (value shl 4) or digit
        }
        return value.toChar()
    }

    private fun literal(expected: String) {
        if (!input.startsWith(expected, offset)) {
            throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
        }
        offset += expected.length
    }

    private fun expect(expected: Char) {
        if (offset >= input.length || input[offset] != expected) {
            throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_JSON)
        }
        offset += 1
    }

    private fun take(expected: Char): Boolean {
        if (offset < input.length && input[offset] == expected) {
            offset += 1
            return true
        }
        return false
    }

    private fun whitespace() {
        while (offset < input.length && input[offset].isWhitespace()) offset += 1
    }
}

private fun Map<String, DoctorJsonValue>.long(name: String): Long =
    (get(name) as? DoctorJsonLong)?.value
        ?: throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)

private fun Map<String, DoctorJsonValue>.int(name: String): Int =
    long(name).let {
        if (it in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()) {
            it.toInt()
        } else {
            throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)
        }
    }

private fun Map<String, DoctorJsonValue>.string(name: String): String =
    (get(name) as? DoctorJsonString)?.value
        ?: throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)

private fun Map<String, DoctorJsonValue>.boolean(name: String): Boolean =
    (get(name) as? DoctorJsonBoolean)?.value
        ?: throw DoctorJsonParseFailure(DoctorEventFailure.INVALID_TYPE)

private fun Map<String, DoctorJsonValue>.optionalLong(name: String): Long? {
    if (name !in this) return null
    return long(name)
}

private fun Map<String, DoctorJsonValue>.optionalInt(name: String): Int? {
    if (name !in this) return null
    return int(name)
}

private fun Map<String, DoctorJsonValue>.optionalBoolean(name: String): Boolean? {
    if (name !in this) return null
    return boolean(name)
}

internal class DoctorJsonParseFailure(
    val reason: DoctorEventFailure,
) : RuntimeException()
