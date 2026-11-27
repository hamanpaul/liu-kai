#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../tsf/doctor_transport.hpp"
#include "../doctor_event.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

using namespace boshiamy::doctor;
namespace fs = std::filesystem;

static_assert(std::is_trivially_copyable_v<EventRecord>);
static_assert(sizeof(EventRecord) == 64);
static_assert(static_cast<std::uint32_t>(Event::Created) == 1);
static_assert(static_cast<std::uint32_t>(Event::ActivateBegin) == 2);
static_assert(static_cast<std::uint32_t>(Event::ActivateEnd) == 3);
static_assert(static_cast<std::uint32_t>(Event::KeySinkAdvice) == 4);
static_assert(static_cast<std::uint32_t>(Event::Focus) == 5);
static_assert(static_cast<std::uint32_t>(Event::TestDown) == 6);
static_assert(static_cast<std::uint32_t>(Event::KeyDown) == 7);
static_assert(static_cast<std::uint32_t>(Event::EditRequest) == 8);
static_assert(static_cast<std::uint32_t>(Event::EditApplied) == 9);
static_assert(static_cast<std::uint32_t>(Event::Deactivate) == 10);
static_assert(static_cast<std::uint32_t>(Event::ExtendedInterfaceQuery) == 11);
static_assert(static_cast<std::uint32_t>(Event::ShiftTestUp) == 12);
static_assert(static_cast<std::uint32_t>(Event::ShiftUp) == 13);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::Created) == 1);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::ActivateBegin) == 2);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::ActivateEnd) == 3);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::KeySinkAdvice) == 4);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::Focus) == 5);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::TestDown) == 6);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::KeyDown) == 7);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::EditRequest) == 8);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::EditApplied) == 9);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::Deactivate) == 10);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::ExtendedInterfaceQuery) == 11);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::ShiftTestUp) == 12);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::ShiftUp) == 13);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::Touch) == 64);
static_assert(static_cast<std::uint32_t>(DoctorEventCode::Health) == 65);
static_assert(std::is_same_v<decltype(DoctorEventRecord::producer_id), std::int64_t>);
static_assert(std::is_same_v<decltype(DoctorEventRecord::session_id), std::int64_t>);
static_assert(std::is_same_v<decltype(DoctorEventRecord::epoch), std::int64_t>);
static_assert(std::is_same_v<decltype(DoctorEventRecord::operation_id), std::int64_t>);
static_assert(std::is_same_v<decltype(DoctorEventRecord::sequence), std::int64_t>);

static const DoctorBuildMetadata kBuild{
    "0.1.0",
    "e045f978eb52001abc8b632482a6f4536cbdf2ab",
};

static DoctorEventRecord event(
    std::int64_t producer_id,
    std::int64_t session_id,
    std::int64_t epoch,
    std::int64_t operation_id,
    std::int64_t sequence,
    std::int64_t monotonic_ns,
    std::int64_t wall_utc_ms,
    DoctorEventCode event_code,
    DoctorCallbackSource callback_source,
    DoctorInputScope scope,
    DoctorEventMode mode,
    DoctorEventPhase phase,
    bool focus,
    bool handled,
    DoctorResultCode result_code,
    std::uint32_t duration_us,
    std::optional<std::int64_t> gesture_token = std::nullopt,
    std::optional<std::uint32_t> pointer_count = std::nullopt,
    std::optional<bool> pressed = std::nullopt,
    std::optional<std::int64_t> drop_count = std::nullopt,
    std::optional<std::int64_t> gap_from_sequence = std::nullopt) {
    return DoctorEventRecord{
        producer_id,
        session_id,
        epoch,
        operation_id,
        sequence,
        monotonic_ns,
        wall_utc_ms,
        480,
        event_code,
        callback_source,
        scope,
        mode,
        phase,
        focus,
        handled,
        result_code,
        duration_us,
        gesture_token,
        pointer_count,
        pressed,
        drop_count,
        gap_from_sequence,
    };
}

static std::vector<DoctorEventRecord> ordinary_events() {
    return {
        event(
            9001, 27, 31, 7001, 1, 5'000'000'000, 1'764'057'600'000,
            DoctorEventCode::Touch, DoctorCallbackSource::SlotForward,
            DoctorInputScope::Ordinary, DoctorEventMode::Liu,
            DoctorEventPhase::Touch, true, true, DoctorResultCode::Ok, 14,
            123456, 1, true),
        event(
            9001, 27, 31, 7001, 2, 5'000'000'900, 1'764'057'600'001,
            DoctorEventCode::Touch, DoctorCallbackSource::KeyAction,
            DoctorInputScope::Ordinary, DoctorEventMode::Liu,
            DoctorEventPhase::Touch, true, true, DoctorResultCode::Ok, 9,
            123456, 1, false),
        event(
            9001, 27, 31, 0, 3, 5'000'005'000, 1'764'057'600'005,
            DoctorEventCode::Health, DoctorCallbackSource::None,
            DoctorInputScope::Ordinary, DoctorEventMode::Liu,
            DoctorEventPhase::Health, true, true, DoctorResultCode::Ok, 0,
            std::nullopt, std::nullopt, std::nullopt, 2, 4),
    };
}

static DoctorEventRecord lifecycle_event(
    std::int64_t producer_id,
    std::int64_t session_id,
    std::int64_t epoch,
    DoctorInputScope scope,
    DoctorEventCode event_code,
    DoctorEventPhase phase,
    std::int64_t sequence,
    std::int64_t monotonic_ns,
    std::int64_t wall_utc_ms) {
    return event(
        producer_id, session_id, epoch, 0, sequence, monotonic_ns,
        wall_utc_ms, event_code, DoctorCallbackSource::None, scope,
        DoctorEventMode::None, phase, false, true, DoctorResultCode::Ok, 0);
}

static std::vector<DoctorEventRecord> sensitive_events() {
    return {
        lifecycle_event(
            9002, 28, 32, DoctorInputScope::Sensitive,
            DoctorEventCode::Created, DoctorEventPhase::Lifecycle, 1,
            6'000'000'000, 1'764'057'660'000),
        lifecycle_event(
            9002, 28, 32, DoctorInputScope::Sensitive,
            DoctorEventCode::Health, DoctorEventPhase::Health, 2,
            6'000'010'000, 1'764'057'660'010),
    };
}

static std::vector<DoctorEventRecord> unknown_events() {
    return {
        lifecycle_event(
            9003, 29, 33, DoctorInputScope::Unknown,
            DoctorEventCode::Created, DoctorEventPhase::Lifecycle, 1,
            7'000'000'000, 1'764'057'720'000),
        lifecycle_event(
            9003, 29, 33, DoctorInputScope::Unknown,
            DoctorEventCode::Health, DoctorEventPhase::Health, 2,
            7'000'010'000, 1'764'057'720'010),
    };
}

static std::string read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

static fs::path fixture(const fs::path &repo_root, const std::string &name) {
    return repo_root / "shared" / "testdata" / "doctor-v1" / name;
}

static bool same_build(
    const DoctorBuildMetadata &left,
    const DoctorBuildMetadata &right) {
    return left.version == right.version &&
        left.source_commit == right.source_commit;
}

static bool same_event(
    const DoctorEventRecord &left,
    const DoctorEventRecord &right) {
    return left.producer_id == right.producer_id &&
        left.session_id == right.session_id &&
        left.epoch == right.epoch &&
        left.operation_id == right.operation_id &&
        left.sequence == right.sequence &&
        left.monotonic_ns == right.monotonic_ns &&
        left.wall_utc_ms == right.wall_utc_ms &&
        left.timezone_offset_minutes == right.timezone_offset_minutes &&
        left.event_code == right.event_code &&
        left.callback_source == right.callback_source &&
        left.scope == right.scope &&
        left.mode == right.mode &&
        left.phase == right.phase &&
        left.focus == right.focus &&
        left.handled == right.handled &&
        left.result_code == right.result_code &&
        left.duration_us == right.duration_us &&
        left.gesture_token == right.gesture_token &&
        left.pointer_count == right.pointer_count &&
        left.pressed == right.pressed &&
        left.drop_count == right.drop_count &&
        left.gap_from_sequence == right.gap_from_sequence;
}

static bool encode_all(
    const std::vector<DoctorEventRecord> &events,
    std::string *output) {
    output->clear();
    for (const auto &record : events) {
        const auto encoded = DoctorEventCodec::encode_line(kBuild, record);
        if (encoded.failure != DoctorEventFailure::None) return false;
        *output += encoded.line;
        *output += '\n';
    }
    return true;
}

static std::string replace_once(
    const std::string &source,
    const std::string &before,
    const std::string &after) {
    const auto position = source.find(before);
    if (position == std::string::npos) return {};
    std::string changed(source);
    changed.replace(position, before.size(), after);
    return changed;
}

static std::string add_field(const std::string &line, const std::string &field) {
    if (line.empty() || line.back() != '}') return {};
    std::string polluted(line);
    polluted.pop_back();
    polluted += ",\"" + field + "\":1}";
    return polluted;
}

static std::set<std::string> json_field_names(const std::string &text) {
    const std::regex field_pattern(R"field("([A-Za-z][A-Za-z0-9]*)"\s*:)field");
    std::set<std::string> fields;
    for (std::sregex_iterator current(text.begin(), text.end(), field_pattern),
        end; current != end; ++current) {
        fields.insert((*current)[1].str());
    }
    return fields;
}

static int test_exact_shared_goldens(const fs::path &repo_root) {
    const std::vector<std::pair<std::string, std::vector<DoctorEventRecord>>>
        cases{
            {"ordinary-touch.jsonl", ordinary_events()},
            {"sensitive-lifecycle.jsonl", sensitive_events()},
            {"unknown-lifecycle.jsonl", unknown_events()},
        };

    for (const auto &test : cases) {
        const auto expected_bytes = read_bytes(fixture(repo_root, test.first));
        CHECK(!expected_bytes.empty());
        std::string encoded;
        CHECK(encode_all(test.second, &encoded));
        CHECK(encoded == expected_bytes);

        const auto decoded = DoctorEventCodec::decode_jsonl(encoded);
        CHECK(decoded.failure == DoctorEventFailure::None);
        CHECK(decoded.failure_line == 0);
        CHECK(decoded.events.size() == test.second.size());
        for (std::size_t index = 0; index < test.second.size(); ++index) {
            CHECK(same_build(decoded.events[index].build, kBuild));
            CHECK(same_event(decoded.events[index].event, test.second[index]));
        }
    }
    return 0;
}

static int test_closed_allowlist(const fs::path &repo_root) {
    const auto ordinary = read_bytes(fixture(repo_root, "ordinary-touch.jsonl"));
    const auto expected_fields = std::set<std::string>{
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
    };
    CHECK(json_field_names(ordinary) == expected_fields);

    const auto valid_line = ordinary.substr(0, ordinary.find('\n'));
    const std::vector<std::string> forbidden_fields{
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
    };
    for (const auto &field : forbidden_fields) {
        const auto polluted = add_field(valid_line, field);
        CHECK(!polluted.empty());
        const auto decoded = DoctorEventCodec::decode_jsonl(polluted + "\n");
        CHECK(decoded.failure == DoctorEventFailure::UnknownField);
        CHECK(decoded.failure_line == 1);
    }

    const auto bad_schema = read_bytes(fixture(repo_root, "bad-schema.jsonl"));
    CHECK(DoctorEventCodec::decode_jsonl(bad_schema).failure ==
        DoctorEventFailure::UnknownField);
    const auto nested = replace_once(
        valid_line, "\"focus\":true", "\"focus\":{\"key\":1}");
    CHECK(!nested.empty());
    CHECK(DoctorEventCodec::decode_jsonl(nested + "\n").failure ==
        DoctorEventFailure::InvalidType);
    return 0;
}

static int test_schema_metadata_enum_and_types(const fs::path &repo_root) {
    const auto encoded = DoctorEventCodec::encode_line(kBuild, ordinary_events().front());
    CHECK(encoded.failure == DoctorEventFailure::None);
    const auto failures = std::vector<
        std::pair<std::string, DoctorEventFailure>>{
        {
            replace_once(encoded.line, "\"schemaVersion\":1", "\"schemaVersion\":2"),
            DoctorEventFailure::UnsupportedSchema,
        },
        {
            replace_once(
                encoded.line,
                "\"version\":\"0.1.0\"",
                "\"version\":\"../../doctor\""),
            DoctorEventFailure::InvalidBuildMetadata,
        },
        {
            replace_once(
                encoded.line,
                "\"sourceCommit\":\"e045f978eb52001abc8b632482a6f4536cbdf2ab\"",
                "\"sourceCommit\":\"untrusted\""),
            DoctorEventFailure::InvalidBuildMetadata,
        },
        {
            replace_once(encoded.line, "\"eventCode\":64", "\"eventCode\":999"),
            DoctorEventFailure::InvalidEnum,
        },
        {
            replace_once(encoded.line, "\"handled\":true", "\"handled\":\"true\""),
            DoctorEventFailure::InvalidType,
        },
        {
            replace_once(encoded.line, "\"scope\":1", "\"scope\":9"),
            DoctorEventFailure::InvalidEnum,
        },
    };
    for (const auto &test : failures) {
        CHECK(!test.first.empty());
        CHECK(DoctorEventCodec::decode_jsonl(test.first + "\n").failure ==
            test.second);
    }

    CHECK(DoctorEventCodec::decode_jsonl(
        "{\"schemaVersion\":1,}\n").failure ==
        DoctorEventFailure::InvalidJson);

    DoctorBuildMetadata bad_build = kBuild;
    bad_build.source_commit = "not-a-build-commit";
    CHECK(DoctorEventCodec::encode_line(bad_build, ordinary_events().front()).failure ==
        DoctorEventFailure::InvalidBuildMetadata);
    CHECK(fs::exists(fixture(repo_root, "ordinary-touch.jsonl")));
    return 0;
}

static int test_positive_signed_shared_ids() {
    auto record = ordinary_events().front();
    const auto validate_rejected = [&](DoctorEventRecord changed) {
        return DoctorEventCodec::encode_line(kBuild, changed).failure ==
            DoctorEventFailure::InvalidIdentifier;
    };

    record.producer_id = 0;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.session_id = 0;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.producer_id = -1;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.session_id = -1;
    CHECK(validate_rejected(record));

    record = ordinary_events().front();
    record.epoch = -1;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.sequence = -1;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.operation_id = -1;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.monotonic_ns = -1;
    CHECK(validate_rejected(record));
    record = ordinary_events().front();
    record.wall_utc_ms = -1;
    CHECK(validate_rejected(record));

    record = ordinary_events().front();
    record.epoch = 0;
    CHECK(DoctorEventCodec::encode_line(kBuild, record).failure ==
        DoctorEventFailure::None);
    record = ordinary_events().front();
    record.sequence = 0;
    CHECK(DoctorEventCodec::encode_line(kBuild, record).failure ==
        DoctorEventFailure::None);
    record = ordinary_events().front();
    record.monotonic_ns = 0;
    CHECK(DoctorEventCodec::encode_line(kBuild, record).failure ==
        DoctorEventFailure::None);
    record = ordinary_events().front();
    record.wall_utc_ms = 0;
    CHECK(DoctorEventCodec::encode_line(kBuild, record).failure ==
        DoctorEventFailure::None);

    record = ordinary_events().front();
    record.operation_id = 0;
    CHECK(DoctorEventCodec::encode_line(kBuild, record).failure ==
        DoctorEventFailure::None);
    return 0;
}

static int test_restricted_scope_side_channel_guards(const fs::path &repo_root) {
    auto restricted = sensitive_events().front();
    restricted.event_code = DoctorEventCode::KeyDown;
    restricted.callback_source = DoctorCallbackSource::KeyAction;
    restricted.operation_id = 77;
    restricted.phase = DoctorEventPhase::Composition;
    restricted.gesture_token = 91;
    restricted.pointer_count = 1;
    restricted.pressed = true;
    CHECK(DoctorEventCodec::encode_line(kBuild, restricted).failure ==
        DoctorEventFailure::RestrictedInputEvent);

    restricted = sensitive_events().front();
    restricted.operation_id = 77;
    CHECK(DoctorEventCodec::encode_line(kBuild, restricted).failure ==
        DoctorEventFailure::RestrictedCorrelation);

    restricted = unknown_events().back();
    restricted.drop_count = 1;
    restricted.gap_from_sequence = 1;
    CHECK(DoctorEventCodec::encode_line(kBuild, restricted).failure ==
        DoctorEventFailure::RestrictedCountField);

    const auto sensitive = read_bytes(fixture(
        repo_root, "sensitive-lifecycle.jsonl"));
    const auto jumped = replace_once(
        sensitive,
        "\"operationId\":0,\"sequence\":2",
        "\"operationId\":0,\"sequence\":10");
    CHECK(!jumped.empty());
    const auto decoded = DoctorEventCodec::decode_jsonl(jumped);
    CHECK(decoded.events.size() == 2);
    CHECK(decoded.failure == DoctorEventFailure::None);
    CHECK(decoded.failure_line == 0);

    auto ordinary_health = ordinary_events().back();
    ordinary_health.sequence = 40;
    ordinary_health.drop_count = 3;
    ordinary_health.gap_from_sequence = 41;
    CHECK(DoctorEventCodec::encode_line(kBuild, ordinary_health).failure ==
        DoctorEventFailure::None);
    std::string ordinary_gap;
    CHECK(encode_all(
        std::vector<DoctorEventRecord>{
            ordinary_events().front(),
            ordinary_health,
        },
        &ordinary_gap));
    const auto ordinary_decoded =
        DoctorEventCodec::decode_jsonl(ordinary_gap);
    CHECK(ordinary_decoded.failure == DoctorEventFailure::None);
    CHECK(ordinary_decoded.events.size() == 2);
    CHECK(ordinary_decoded.events.back().event.sequence == 40);
    CHECK(ordinary_decoded.events.back().event.drop_count == 3);

    std::string separate_restricted_stream;
    CHECK(encode_all(
        std::vector<DoctorEventRecord>{
            sensitive_events().front(),
            unknown_events().front(),
        },
        &separate_restricted_stream));
    const auto separate_restricted =
        DoctorEventCodec::decode_jsonl(separate_restricted_stream);
    CHECK(separate_restricted.failure == DoctorEventFailure::None);
    CHECK(separate_restricted.events.size() == 2);
    return 0;
}

static int test_torn_tail_preserves_valid_prefix(const fs::path &repo_root) {
    const auto decoded = DoctorEventCodec::decode_jsonl(
        read_bytes(fixture(repo_root, "torn-tail.jsonl")));
    CHECK(decoded.events.size() == 1);
    CHECK(same_event(decoded.events.front().event, ordinary_events().front()));
    CHECK(decoded.failure == DoctorEventFailure::TornLine);
    CHECK(decoded.failure_line == 2);
    return 0;
}

static int test_incomplete_eof_is_torn_even_with_line_feed() {
    const auto decoded = DoctorEventCodec::decode_jsonl(
        "{\"schemaVersion\":1\n");
    CHECK(decoded.events.empty());
    CHECK(decoded.failure == DoctorEventFailure::TornLine);
    CHECK(decoded.failure_line == 1);
    return 0;
}

static int test_platform_result_codes(const fs::path &repo_root) {
    const auto golden = read_bytes(fixture(repo_root, "platform-result.jsonl"));
    CHECK(!golden.empty());
    for (const auto &number : {"-2147024891", "0", "-2147483648", "2147483647"}) {
        const auto sample = replace_once(golden,
            "\"platformResultCode\":-2147024891",
            std::string("\"platformResultCode\":") + number);
        const auto decoded = DoctorEventCodec::decode_jsonl(sample);
        CHECK(decoded.failure == DoctorEventFailure::None);
        CHECK(decoded.events.size() == 1);
        const auto encoded = DoctorEventCodec::encode_line(kBuild, decoded.events.front().event);
        CHECK(encoded.failure == DoctorEventFailure::None);
        CHECK(encoded.line + "\n" == sample);
    }
    for (const auto &number : {"2147483648", "-2147483649", "\"failure\"", "true", "0.5", "1e3", "-2.5E-2", "{}"}) {
        const auto sample = replace_once(golden,
            "\"platformResultCode\":-2147024891",
            std::string("\"platformResultCode\":") + number);
        const auto failure = DoctorEventCodec::decode_jsonl(sample).failure;
        if (failure != DoctorEventFailure::InvalidType)
            std::cerr << "unexpected type failure for " << number << ": " << static_cast<int>(failure) << '\n';
        CHECK(failure == DoctorEventFailure::InvalidType);
    }
    return 0;
}

static int test_shared_event_value_boundaries() {
    const auto ordinary = ordinary_events().front();
    for (int missing = 0; missing != 3; ++missing) {
        auto changed = ordinary;
        if (missing == 0) changed.gesture_token.reset();
        if (missing == 1) changed.pointer_count.reset();
        if (missing == 2) changed.pressed.reset();
        CHECK(DoctorEventCodec::encode_line(kBuild, changed).failure == DoctorEventFailure::InvalidType);
    }
    auto changed = ordinary;
    changed.event_code = DoctorEventCode::KeyDown;
    changed.phase = DoctorEventPhase::Composition;
    CHECK(DoctorEventCodec::encode_line(kBuild, changed).failure == DoctorEventFailure::InvalidType);
    for (const auto offset : {-721, 841}) {
        changed = ordinary;
        changed.timezone_offset_minutes = offset;
        CHECK(DoctorEventCodec::encode_line(kBuild, changed).failure == DoctorEventFailure::InvalidIdentifier);
    }
    changed = unknown_events().front();
    changed.phase = changed.event_code == DoctorEventCode::Health ? DoctorEventPhase::Lifecycle : DoctorEventPhase::Health;
    CHECK(DoctorEventCodec::encode_line(kBuild, changed).failure == DoctorEventFailure::RestrictedInputEvent);
    return 0;
}

static int test_shared_build_metadata_boundaries() {
    for (const auto &build : std::vector<DoctorBuildMetadata>{
        {"0", "abcdef0"},
        {std::string(128, 'A'), "ABCDEF0123456789ABCDEF0123456789ABCDEF01"},
        {"0.1.1-dev+test_1", kBuild.source_commit}}) {
        const auto encoded = DoctorEventCodec::encode_line(build, ordinary_events().front());
        CHECK(encoded.failure == DoctorEventFailure::None);
        const auto decoded = DoctorEventCodec::decode_jsonl(encoded.line + "\n");
        CHECK(decoded.failure == DoctorEventFailure::None);
        CHECK(same_build(decoded.events.front().build, build));
    }
    for (const auto &build : std::vector<DoctorBuildMetadata>{
        {"", kBuild.source_commit}, {".0.1.0", kBuild.source_commit},
        {std::string(129, 'A'), kBuild.source_commit},
        {"0.1.1/", kBuild.source_commit}, {"0.1.1", "abcdef"},
        {"0.1.1", std::string(41, 'a')}, {"0.1.1", "abcdefg"}}) {
        CHECK(DoctorEventCodec::encode_line(build, ordinary_events().front()).failure ==
            DoctorEventFailure::InvalidBuildMetadata);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: doctor_event_tests <repo-root>\n";
        return 2;
    }
    const auto repo_root = fs::absolute(argv[1]).lexically_normal();
    if (const auto result = test_exact_shared_goldens(repo_root)) return result;
    if (const auto result = test_closed_allowlist(repo_root)) return result;
    if (const auto result = test_schema_metadata_enum_and_types(repo_root)) return result;
    if (const auto result = test_positive_signed_shared_ids()) return result;
    if (const auto result = test_restricted_scope_side_channel_guards(repo_root)) return result;
    if (const auto result = test_torn_tail_preserves_valid_prefix(repo_root)) return result;
    if (const auto result = test_platform_result_codes(repo_root)) return result;
    if (const auto result = test_shared_event_value_boundaries()) return result;
    if (const auto result = test_shared_build_metadata_boundaries()) return result;
    if (const auto result = test_incomplete_eof_is_torn_even_with_line_feed()) return result;
    std::cout << "Doctor event contract tests passed\n";
    return 0;
}
