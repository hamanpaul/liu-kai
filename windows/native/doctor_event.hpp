#ifndef BOSHIAMY_WINDOWS_DOCTOR_EVENT_HPP
#define BOSHIAMY_WINDOWS_DOCTOR_EVENT_HPP

#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace boshiamy::doctor {

inline constexpr std::uint32_t kDoctorSchemaVersion = 1;

struct DoctorBuildMetadata {
    std::string version;
    std::string source_commit;
};

enum class DoctorEventCode : std::uint32_t {
    Created = 1,
    ActivateBegin = 2,
    ActivateEnd = 3,
    KeySinkAdvice = 4,
    Focus = 5,
    TestDown = 6,
    KeyDown = 7,
    EditRequest = 8,
    EditApplied = 9,
    Deactivate = 10,
    ExtendedInterfaceQuery = 11,
    ShiftTestUp = 12,
    ShiftUp = 13,
    Touch = 64,
    Health = 65,
};

enum class DoctorCallbackSource : std::uint32_t {
    None = 0,
    Dispatch = 1,
    SlotForward = 2,
    KeyAction = 3,
};

enum class DoctorInputScope : std::uint32_t {
    Unknown = 0,
    Ordinary = 1,
    Sensitive = 2,
};

enum class DoctorEventMode : std::uint32_t {
    None = 0,
    Liu = 1,
    English = 2,
};

enum class DoctorEventPhase : std::uint32_t {
    None = 0,
    Touch = 1,
    Composition = 2,
    Candidate = 3,
    Commit = 4,
    Health = 5,
    Lifecycle = 6,
};

enum class DoctorResultCode : std::uint32_t {
    None = 0,
    Ok = 1,
    Rejected = 2,
    Unavailable = 3,
    Failed = 4,
};

struct DoctorEventRecord {
    std::int64_t producer_id = 0;
    std::int64_t session_id = 0;
    std::int64_t epoch = 0;
    std::int64_t operation_id = 0;
    std::int64_t sequence = 0;
    std::int64_t monotonic_ns = 0;
    std::int64_t wall_utc_ms = 0;
    std::int32_t timezone_offset_minutes = 0;
    DoctorEventCode event_code = DoctorEventCode::Created;
    DoctorCallbackSource callback_source = DoctorCallbackSource::None;
    DoctorInputScope scope = DoctorInputScope::Unknown;
    DoctorEventMode mode = DoctorEventMode::None;
    DoctorEventPhase phase = DoctorEventPhase::Lifecycle;
    bool focus = false;
    bool handled = false;
    DoctorResultCode result_code = DoctorResultCode::None;
    std::int64_t duration_us = 0;
    std::optional<std::int64_t> gesture_token = std::nullopt;
    std::optional<std::int32_t> pointer_count = std::nullopt;
    std::optional<bool> pressed = std::nullopt;
    std::optional<std::int64_t> drop_count = std::nullopt;
    std::optional<std::int64_t> gap_from_sequence = std::nullopt;
    std::optional<std::int32_t> platform_result_code = std::nullopt;
};

enum class DoctorEventFailure : std::uint32_t {
    None = 0,
    UnknownField = 1,
    InvalidType = 2,
    InvalidJson = 3,
    UnsupportedSchema = 4,
    InvalidBuildMetadata = 5,
    InvalidEnum = 6,
    TornLine = 7,
    RestrictedInputEvent = 8,
    RestrictedCorrelation = 9,
    RestrictedCountField = 10,
    RestrictedSequenceGap = 11,
    InvalidIdentifier = 12,
};

struct DoctorEventEncodeResult {
    std::string line;
    DoctorEventFailure failure = DoctorEventFailure::None;
};

struct DoctorEventEnvelope {
    DoctorBuildMetadata build;
    DoctorEventRecord event;
};

struct DoctorEventDecodeResult {
    std::vector<DoctorEventEnvelope> events;
    DoctorEventFailure failure = DoctorEventFailure::None;
    std::size_t failure_line = 0;
};

namespace doctor_event_detail {

inline bool valid_version(std::string_view value) {
    const auto alphanumeric = [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z');
    };
    if (value.empty() || value.size() > 128 || !alphanumeric(value.front())) return false;
    for (const char ch : value) {
        if (!(alphanumeric(ch) || ch == '.' || ch == '+' || ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

inline bool valid_source_commit(std::string_view value) {
    if (value.size() < 7 || value.size() > 40) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

inline bool valid_build(const DoctorBuildMetadata &build) {
    return valid_version(build.version) &&
        valid_source_commit(build.source_commit);
}

inline bool valid_event_code(DoctorEventCode value) {
    switch (value) {
    case DoctorEventCode::Created:
    case DoctorEventCode::ActivateBegin:
    case DoctorEventCode::ActivateEnd:
    case DoctorEventCode::KeySinkAdvice:
    case DoctorEventCode::Focus:
    case DoctorEventCode::TestDown:
    case DoctorEventCode::KeyDown:
    case DoctorEventCode::EditRequest:
    case DoctorEventCode::EditApplied:
    case DoctorEventCode::Deactivate:
    case DoctorEventCode::ExtendedInterfaceQuery:
    case DoctorEventCode::ShiftTestUp:
    case DoctorEventCode::ShiftUp:
    case DoctorEventCode::Touch:
    case DoctorEventCode::Health:
        return true;
    }
    return false;
}

template<class Enum, std::uint32_t Max>
inline bool valid_enum(Enum value) {
    return static_cast<std::uint32_t>(value) <= Max;
}

inline bool restricted_event_code(DoctorEventCode value) {
    return value == DoctorEventCode::Created ||
        value == DoctorEventCode::ActivateBegin ||
        value == DoctorEventCode::ActivateEnd ||
        value == DoctorEventCode::Focus ||
        value == DoctorEventCode::Deactivate ||
        value == DoctorEventCode::Health;
}

inline DoctorEventFailure validate_record(const DoctorEventRecord &record) {
    if (record.producer_id <= 0 || record.session_id <= 0)
        return DoctorEventFailure::InvalidIdentifier;
    if (record.epoch < 0 || record.operation_id < 0 ||
        record.sequence < 0 || record.monotonic_ns < 0 ||
        record.wall_utc_ms < 0 || record.duration_us < 0 ||
        record.timezone_offset_minutes < -12 * 60 ||
        record.timezone_offset_minutes > 14 * 60)
        return DoctorEventFailure::InvalidIdentifier;
    if ((record.gesture_token && *record.gesture_token < 0) ||
        (record.pointer_count && *record.pointer_count < 0) ||
        (record.drop_count && *record.drop_count < 0) ||
        (record.gap_from_sequence && *record.gap_from_sequence < 0))
        return DoctorEventFailure::InvalidIdentifier;

    if (!valid_event_code(record.event_code) ||
        !valid_enum<DoctorCallbackSource, 3>(record.callback_source) ||
        !valid_enum<DoctorInputScope, 2>(record.scope) ||
        !valid_enum<DoctorEventMode, 2>(record.mode) ||
        !valid_enum<DoctorEventPhase, 6>(record.phase) ||
        !valid_enum<DoctorResultCode, 4>(record.result_code))
        return DoctorEventFailure::InvalidEnum;

    if (record.scope == DoctorInputScope::Ordinary) {
        const bool has_any_touch = record.gesture_token || record.pointer_count || record.pressed;
        const bool has_all_touch = record.gesture_token && record.pointer_count && record.pressed;
        if (record.event_code == DoctorEventCode::Touch ? !has_all_touch : has_any_touch)
            return DoctorEventFailure::InvalidType;
        return DoctorEventFailure::None;
    }
    if (!restricted_event_code(record.event_code))
        return DoctorEventFailure::RestrictedInputEvent;
    if (record.operation_id != 0 ||
        record.callback_source != DoctorCallbackSource::None ||
        record.phase != (record.event_code == DoctorEventCode::Health
            ? DoctorEventPhase::Health : DoctorEventPhase::Lifecycle))
        return record.operation_id != 0
            ? DoctorEventFailure::RestrictedCorrelation
            : DoctorEventFailure::RestrictedInputEvent;
    if (record.gesture_token || record.pointer_count || record.pressed ||
        record.drop_count || record.gap_from_sequence)
        return DoctorEventFailure::RestrictedCountField;
    return DoctorEventFailure::None;
}

inline std::string integer(std::int64_t value) {
    return std::to_string(value);
}

inline std::string integer(std::int32_t value) {
    return std::to_string(value);
}

inline std::string enum_integer(DoctorEventCode value) {
    return std::to_string(static_cast<std::uint32_t>(value));
}

inline std::string enum_integer(DoctorCallbackSource value) {
    return std::to_string(static_cast<std::uint32_t>(value));
}

inline std::string enum_integer(DoctorInputScope value) {
    return std::to_string(static_cast<std::uint32_t>(value));
}

inline std::string enum_integer(DoctorEventMode value) {
    return std::to_string(static_cast<std::uint32_t>(value));
}

inline std::string enum_integer(DoctorEventPhase value) {
    return std::to_string(static_cast<std::uint32_t>(value));
}

inline std::string enum_integer(DoctorResultCode value) {
    return std::to_string(static_cast<std::uint32_t>(value));
}

enum class JsonType : std::uint8_t {
    String,
    Integer,
    Boolean,
    Other,
};

struct JsonValue {
    JsonType type = JsonType::Other;
    std::string_view text;
};

inline bool skip_space(std::string_view text, std::size_t *cursor) {
    while (*cursor < text.size() &&
        (text[*cursor] == ' ' || text[*cursor] == '\t' ||
            text[*cursor] == '\r')) {
        ++*cursor;
    }
    return *cursor < text.size();
}

inline bool parse_string(
    std::string_view text,
    std::size_t *cursor,
    std::string *output) {
    if (*cursor >= text.size() || text[*cursor] != '"') return false;
    ++*cursor;
    output->clear();
    const auto begin = *cursor;
    while (*cursor < text.size() && text[*cursor] != '"') {
        const auto value = static_cast<unsigned char>(text[*cursor]);
        if (value < 0x20 || text[*cursor] == '\\') return false;
        ++*cursor;
    }
    if (*cursor >= text.size() || text[*cursor] != '"') return false;
    output->append(text.data() + begin, *cursor - begin);
    ++*cursor;
    return true;
}

inline bool parse_number(
    std::string_view text,
    std::size_t *cursor,
    std::string_view *output,
    bool *is_integer) {
    const auto begin = *cursor;
    if (*cursor < text.size() && text[*cursor] == '-') ++*cursor;
    const auto digit_begin = *cursor;
    if (*cursor < text.size() && text[*cursor] == '0') {
        ++*cursor;
    } else {
        if (*cursor >= text.size() ||
            text[*cursor] < '1' || text[*cursor] > '9')
            return false;
        while (*cursor < text.size() &&
            text[*cursor] >= '0' && text[*cursor] <= '9')
            ++*cursor;
    }
    if (*cursor == digit_begin ||
        (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9'))
        return false;
    *is_integer = true;
    if (*cursor < text.size() && text[*cursor] == '.') {
        *is_integer = false;
        const auto fraction_begin = ++*cursor;
        while (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9') ++*cursor;
        if (*cursor == fraction_begin) return false;
    }
    if (*cursor < text.size() && (text[*cursor] == 'e' || text[*cursor] == 'E')) {
        *is_integer = false;
        ++*cursor;
        if (*cursor < text.size() && (text[*cursor] == '+' || text[*cursor] == '-')) ++*cursor;
        const auto exponent_begin = *cursor;
        while (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9') ++*cursor;
        if (*cursor == exponent_begin) return false;
    }
    *output = text.substr(begin, *cursor - begin);
    return true;
}

inline bool parse_object(
    std::string_view text,
    std::vector<std::pair<std::string, JsonValue>> *fields,
    DoctorEventFailure *failure) {
    std::size_t cursor = 0;
    if (!skip_space(text, &cursor) || text[cursor] != '{') return false;
    ++cursor;
    std::set<std::string> seen;
    while (true) {
        if (!skip_space(text, &cursor)) return false;
        if (text[cursor] == '}') {
            ++cursor;
            skip_space(text, &cursor);
            return cursor == text.size();
        }
        std::string key;
        if (!parse_string(text, &cursor, &key) ||
            !seen.insert(key).second ||
            !skip_space(text, &cursor) || text[cursor] != ':')
            return false;
        ++cursor;
        if (!skip_space(text, &cursor)) return false;

        JsonValue value;
        if (text[cursor] == '"') {
            value.type = JsonType::String;
            std::string ignored;
            const auto begin = cursor;
            if (!parse_string(text, &cursor, &ignored)) return false;
            value.text = text.substr(begin + 1, ignored.size());
        } else if (text[cursor] == '-' ||
            (text[cursor] >= '0' && text[cursor] <= '9')) {
            bool is_integer = false;
            if (!parse_number(text, &cursor, &value.text, &is_integer)) return false;
            value.type = is_integer ? JsonType::Integer : JsonType::Other;
        } else if (text.compare(cursor, 4, "true") == 0) {
            value.type = JsonType::Boolean;
            value.text = text.substr(cursor, 4);
            cursor += 4;
        } else if (text.compare(cursor, 5, "false") == 0) {
            value.type = JsonType::Boolean;
            value.text = text.substr(cursor, 5);
            cursor += 5;
        } else {
            value.type = JsonType::Other;
            if (text[cursor] == '{' || text[cursor] == '[') {
                *failure = DoctorEventFailure::InvalidType;
                return false;
            }
            return false;
        }

        fields->emplace_back(std::move(key), value);
        if (!skip_space(text, &cursor)) return false;
        if (text[cursor] == ',') {
            ++cursor;
            std::size_t after_comma = cursor;
            if (skip_space(text, &after_comma) &&
                text[after_comma] == '}')
                return false;
            continue;
        }
        if (text[cursor] != '}') return false;
    }
}

template<class Integer>
inline bool parse_signed(std::string_view text, Integer *value) {
    if (text.empty()) return false;
    bool negative = false;
    std::size_t cursor = 0;
    if (text[0] == '-') {
        negative = true;
        cursor = 1;
        if (text.size() == 1) return false;
    }
    std::uint64_t magnitude = 0;
    for (; cursor < text.size(); ++cursor) {
        const auto ch = text[cursor];
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (magnitude >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        magnitude = magnitude * 10 + digit;
    }
    using Unsigned = typename std::make_unsigned<Integer>::type;
    if (negative) {
        using WideSigned = std::intmax_t;
        const auto minimum = static_cast<WideSigned>(
            std::numeric_limits<Integer>::min());
        const auto min_limit = static_cast<std::uint64_t>(
            std::numeric_limits<Integer>::max()) + 1;
        if (magnitude > min_limit) return false;
        if (magnitude == min_limit) {
            *value = std::numeric_limits<Integer>::min();
        } else {
            *value = static_cast<Integer>(
                -static_cast<WideSigned>(magnitude));
        }
    } else {
        if (magnitude > static_cast<std::uint64_t>(
            std::numeric_limits<Integer>::max()))
            return false;
        *value = static_cast<Integer>(
            static_cast<Unsigned>(magnitude));
    }
    return true;
}

inline bool parse_uint32(std::string_view text, std::uint32_t *value) {
    std::int64_t signed_value = 0;
    if (!parse_signed(text, &signed_value) || signed_value < 0 ||
        signed_value > static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max()))
        return false;
    *value = static_cast<std::uint32_t>(signed_value);
    return true;
}

inline bool allowed_field(std::string_view field) {
    static const char *const fields[] = {
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
    };
    for (const auto allowed : fields)
        if (field == allowed) return true;
    return false;
}

inline bool is_required(std::string_view field) {
    return field != "gestureToken" &&
        field != "pointerCount" &&
        field != "pressed" &&
        field != "dropCount" &&
        field != "gapFromSequence" && field != "platformResultCode";
}

inline bool parse_bool(const JsonValue &value, bool *output) {
    if (value.type != JsonType::Boolean) return false;
    *output = value.text == "true";
    return true;
}

template<class Integer>
inline bool parse_integer_field(
    const JsonValue &value,
    Integer *output) {
    return value.type == JsonType::Integer &&
        parse_signed(value.text, output);
}

inline DoctorEventDecodeResult decode_line(
    std::string_view text,
    std::size_t line_number) {
    DoctorEventDecodeResult output;
    output.failure_line = line_number;
    if (text.empty() || text.back() != '}') {
        output.failure = DoctorEventFailure::TornLine;
        return output;
    }

    std::vector<std::pair<std::string, JsonValue>> fields;
    DoctorEventFailure parse_failure = DoctorEventFailure::InvalidJson;
    if (!parse_object(text, &fields, &parse_failure)) {
        output.failure = parse_failure;
        if (parse_failure == DoctorEventFailure::InvalidJson &&
            text.front() != '{')
            output.failure = DoctorEventFailure::InvalidJson;
        return output;
    }

    std::set<std::string> field_names;
    for (const auto &field : fields) {
        if (!allowed_field(field.first)) {
            output.failure = DoctorEventFailure::UnknownField;
            return output;
        }
        field_names.insert(field.first);
    }
    static const char *const required_fields[] = {
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
    };
    for (const auto required : required_fields) {
        if (field_names.count(required) == 0) {
            output.failure = DoctorEventFailure::InvalidJson;
            return output;
        }
    }

    DoctorEventEnvelope envelope;
    bool schema_seen = false;
    std::uint32_t schema_version = 0;
    for (const auto &field : fields) {
        const auto &name = field.first;
        const auto &value = field.second;
        if (name == "schemaVersion") {
            schema_seen = parse_uint32(value.text, &schema_version) &&
                value.type == JsonType::Integer;
            if (!schema_seen) {
                output.failure = DoctorEventFailure::InvalidType;
                return output;
            }
            if (schema_version != kDoctorSchemaVersion) {
                output.failure = DoctorEventFailure::UnsupportedSchema;
                return output;
            }
        } else if (name == "version" || name == "sourceCommit") {
            if (value.type != JsonType::String) {
                output.failure = DoctorEventFailure::InvalidType;
                return output;
            }
            if (name == "version") {
                envelope.build.version.assign(
                    value.text.data(), value.text.size());
            } else {
                envelope.build.source_commit.assign(
                    value.text.data(), value.text.size());
            }
        } else if (name == "focus") {
            if (!parse_bool(value, &envelope.event.focus)) {
                output.failure = DoctorEventFailure::InvalidType;
                return output;
            }
        } else if (name == "handled") {
            if (!parse_bool(value, &envelope.event.handled)) {
                output.failure = DoctorEventFailure::InvalidType;
                return output;
            }
        } else if (name == "pressed") {
            bool pressed = false;
            if (!parse_bool(value, &pressed)) {
                output.failure = DoctorEventFailure::InvalidType;
                return output;
            }
            envelope.event.pressed = pressed;
        } else {
            std::int64_t number = 0;
            if (!parse_integer_field(value, &number)) {
                output.failure = DoctorEventFailure::InvalidType;
                return output;
            }
            if (name == "producerId") envelope.event.producer_id = number;
            else if (name == "sessionId") envelope.event.session_id = number;
            else if (name == "epoch") envelope.event.epoch = number;
            else if (name == "operationId") envelope.event.operation_id = number;
            else if (name == "sequence") envelope.event.sequence = number;
            else if (name == "monotonicNs") envelope.event.monotonic_ns = number;
            else if (name == "wallUtcMs") envelope.event.wall_utc_ms = number;
            else if (name == "durationUs") envelope.event.duration_us = number;
            else if (name == "gestureToken") envelope.event.gesture_token = number;
            else if (name == "dropCount") envelope.event.drop_count = number;
            else if (name == "gapFromSequence")
                envelope.event.gap_from_sequence = number;
            else if (name == "platformResultCode") {
                if (number < std::numeric_limits<std::int32_t>::min() ||
                    number > std::numeric_limits<std::int32_t>::max()) {
                    output.failure = DoctorEventFailure::InvalidType;
                    return output;
                }
                envelope.event.platform_result_code = static_cast<std::int32_t>(number);
            }
            else if (name == "pointerCount") {
                if (number < std::numeric_limits<std::int32_t>::min() ||
                    number > std::numeric_limits<std::int32_t>::max()) {
                    output.failure = DoctorEventFailure::InvalidType;
                    return output;
                }
                envelope.event.pointer_count =
                    static_cast<std::int32_t>(number);
            } else if (name == "timezoneOffsetMinutes") {
                if (number < std::numeric_limits<std::int32_t>::min() ||
                    number > std::numeric_limits<std::int32_t>::max()) {
                    output.failure = DoctorEventFailure::InvalidType;
                    return output;
                }
                envelope.event.timezone_offset_minutes =
                    static_cast<std::int32_t>(number);
            } else {
                if (number < 0 ||
                    number > std::numeric_limits<std::uint32_t>::max()) {
                    output.failure = DoctorEventFailure::InvalidEnum;
                    return output;
                }
                const auto wire = static_cast<std::uint32_t>(number);
                if (name == "eventCode")
                    envelope.event.event_code =
                        static_cast<DoctorEventCode>(wire);
                else if (name == "callbackSource")
                    envelope.event.callback_source =
                        static_cast<DoctorCallbackSource>(wire);
                else if (name == "scope")
                    envelope.event.scope =
                        static_cast<DoctorInputScope>(wire);
                else if (name == "mode")
                    envelope.event.mode =
                        static_cast<DoctorEventMode>(wire);
                else if (name == "phase")
                    envelope.event.phase =
                        static_cast<DoctorEventPhase>(wire);
                else if (name == "resultCode")
                    envelope.event.result_code =
                        static_cast<DoctorResultCode>(wire);
            }
        }
    }

    if (!valid_build(envelope.build)) {
        output.failure = DoctorEventFailure::InvalidBuildMetadata;
        return output;
    }
    output.failure = validate_record(envelope.event);
    if (output.failure == DoctorEventFailure::None) {
        output.events.push_back(std::move(envelope));
        output.failure_line = 0;
    }
    return output;
}

} // namespace doctor_event_detail

struct DoctorEventCodec {
    static DoctorEventEncodeResult encode_line(
        const DoctorBuildMetadata &build,
        const DoctorEventRecord &record) {
        DoctorEventEncodeResult output;
        if (!doctor_event_detail::valid_build(build)) {
            output.failure = DoctorEventFailure::InvalidBuildMetadata;
            return output;
        }
        output.failure = doctor_event_detail::validate_record(record);
        if (output.failure != DoctorEventFailure::None) return output;

        using namespace doctor_event_detail;
        output.line.reserve(640);
        output.line += "{\"schemaVersion\":1,\"version\":\"";
        output.line += build.version;
        output.line += "\",\"sourceCommit\":\"";
        output.line += build.source_commit;
        output.line += "\",\"producerId\":";
        output.line += integer(record.producer_id);
        output.line += ",\"sessionId\":";
        output.line += integer(record.session_id);
        output.line += ",\"epoch\":";
        output.line += integer(record.epoch);
        output.line += ",\"operationId\":";
        output.line += integer(record.operation_id);
        output.line += ",\"sequence\":";
        output.line += integer(record.sequence);
        output.line += ",\"monotonicNs\":";
        output.line += integer(record.monotonic_ns);
        output.line += ",\"wallUtcMs\":";
        output.line += integer(record.wall_utc_ms);
        output.line += ",\"timezoneOffsetMinutes\":";
        output.line += integer(record.timezone_offset_minutes);
        output.line += ",\"eventCode\":";
        output.line += enum_integer(record.event_code);
        output.line += ",\"callbackSource\":";
        output.line += enum_integer(record.callback_source);
        output.line += ",\"scope\":";
        output.line += enum_integer(record.scope);
        output.line += ",\"mode\":";
        output.line += enum_integer(record.mode);
        output.line += ",\"phase\":";
        output.line += enum_integer(record.phase);
        output.line += ",\"focus\":";
        output.line += record.focus ? "true" : "false";
        output.line += ",\"handled\":";
        output.line += record.handled ? "true" : "false";
        output.line += ",\"resultCode\":";
        output.line += enum_integer(record.result_code);
        output.line += ",\"durationUs\":";
        output.line += integer(record.duration_us);
        if (record.gesture_token) {
            output.line += ",\"gestureToken\":";
            output.line += integer(*record.gesture_token);
        }
        if (record.pointer_count) {
            output.line += ",\"pointerCount\":";
            output.line += integer(*record.pointer_count);
        }
        if (record.pressed) {
            output.line += ",\"pressed\":";
            output.line += *record.pressed ? "true" : "false";
        }
        if (record.drop_count) {
            output.line += ",\"dropCount\":";
            output.line += integer(*record.drop_count);
        }
        if (record.gap_from_sequence) {
            output.line += ",\"gapFromSequence\":";
            output.line += integer(*record.gap_from_sequence);
        }
        if (record.platform_result_code) {
            output.line += ",\"platformResultCode\":";
            output.line += integer(*record.platform_result_code);
        }
        output.line += '}';
        return output;
    }

    static DoctorEventDecodeResult decode_jsonl(std::string_view text) {
        DoctorEventDecodeResult output;
        std::size_t cursor = 0;
        std::size_t line_number = 1;
        while (cursor < text.size()) {
            const auto newline = text.find('\n', cursor);
            const auto end = newline == std::string_view::npos
                ? text.size() : newline;
            auto line = text.substr(cursor, end - cursor);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty()) {
                output.failure = DoctorEventFailure::InvalidJson;
                output.failure_line = line_number;
                return output;
            }
            const auto decoded =
                doctor_event_detail::decode_line(line, line_number);
            if (decoded.failure != DoctorEventFailure::None) {
                output.failure = decoded.failure;
                output.failure_line = line_number;
                return output;
            }
            output.events.insert(
                output.events.end(),
                decoded.events.begin(),
                decoded.events.end());
            if (newline == std::string_view::npos) break;
            cursor = newline + 1;
            ++line_number;
        }
        return output;
    }
};

} // namespace boshiamy::doctor

#endif
