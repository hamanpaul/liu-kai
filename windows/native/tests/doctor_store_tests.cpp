#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../doctor_event.hpp"
#include "../doctor_store.hpp"

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

using namespace boshiamy::doctor;
namespace fs = std::filesystem;

static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::None) == 0);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::Disabled) == 1);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::PrivacyRejected) == 2);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::InvalidSchema) == 3);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::TornTail) == 4);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::EpochMismatch) == 5);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::EpochCommitFailed) == 6);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::ClearIncomplete) == 7);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::DiskFull) == 8);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::PermissionDenied) == 9);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::IoError) == 10);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::ExportExists) == 11);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::ExportExpired) == 12);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::ExportLimit) == 13);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::TotalLimit) == 14);
static_assert(static_cast<std::uint32_t>(DoctorStoreFailure::RecordTooLarge) == 15);

static constexpr std::int64_t kStartWallMs = 1'764'057'600'000;
static constexpr std::int64_t kStartMonotonicNs = 9'000'000'000;

static const DoctorBuildMetadata kBuild{
    "0.1.0",
    "e045f978eb52001abc8b632482a6f4536cbdf2ab",
};

class TempDirectory {
public:
    TempDirectory() {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary))
            throw std::runtime_error("GetTempPathW failed");
        std::error_code error;
        base_ = fs::weakly_canonical(fs::path(temporary), error);
        if (error || !base_.is_absolute()) throw std::runtime_error("Invalid test temp root");
        for (unsigned attempt = 0; attempt != 32; ++attempt) {
            const auto candidate = base_ /
                (L"liu-kai-doctor-store-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(counter()));
            if (fs::create_directory(candidate, error)) { path_ = candidate; return; }
            error.clear();
        }
        throw std::runtime_error("Cannot create an owned test temp directory");
    }

    ~TempDirectory() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(path_, error);
        if (error || !resolved.is_absolute() || resolved.parent_path() != base_ ||
            resolved.filename().wstring().rfind(L"liu-kai-doctor-store-", 0) != 0) return;
        fs::remove_all(resolved, error);
    }

    const fs::path &path() const noexcept { return path_; }

private:
    static std::uint64_t counter() noexcept {
        static volatile LONG64 next = 0;
        return static_cast<std::uint64_t>(InterlockedIncrement64(&next));
    }

    fs::path base_, path_;
};

class ManualDoctorClock final : public DoctorClock {
public:
    ManualDoctorClock(
        std::int64_t wall_ms = kStartWallMs,
        std::int64_t monotonic_ns = kStartMonotonicNs)
        : wall_ms_(wall_ms), monotonic_ns_(monotonic_ns) {}

    std::int64_t wall_utc_ms() const override { return wall_ms_; }
    std::int64_t monotonic_ns() const override { return monotonic_ns_; }

    void set_wall_utc_ms(std::int64_t value) noexcept { wall_ms_ = value; }
    void advance(std::int64_t milliseconds) noexcept {
        wall_ms_ += milliseconds;
        monotonic_ns_ += milliseconds * 1'000'000;
    }
    void advance_monotonic(std::int64_t milliseconds) noexcept {
        monotonic_ns_ += milliseconds * 1'000'000;
    }
    void reset_monotonic() noexcept { monotonic_ns_ = 0; }

private:
    std::int64_t wall_ms_ = 0;
    std::int64_t monotonic_ns_ = 0;
};

class ScriptedDoctorIoFaults final : public DoctorIoFaults {
public:
    void fail_next(
        DoctorIoOperation operation,
        DoctorStoreFailure failure,
        std::int32_t platform_result_code = 0) {
        pending_[operation].push_back(
            DoctorIoFault{failure, platform_result_code});
    }

    std::optional<DoctorIoFault> before(
        DoctorIoOperation operation) override {
        auto found = pending_.find(operation);
        if (found == pending_.end() || found->second.empty())
            return std::nullopt;
        const auto result = found->second.front();
        found->second.pop_front();
        return result;
    }

private:
    std::map<DoctorIoOperation, std::deque<DoctorIoFault>> pending_;
};

static DoctorEventRecord event_record(
    const DoctorStore &store,
    std::int64_t sequence,
    const ManualDoctorClock &clock,
    std::int64_t wall_ms = -1,
    DoctorInputScope scope = DoctorInputScope::Ordinary) {
    const bool now = wall_ms < 0;
    return DoctorEventRecord{
        42,
        7,
        store.status().epoch,
        1'000 + sequence,
        sequence,
        clock.monotonic_ns() + sequence,
        now ? clock.wall_utc_ms() : wall_ms,
        480,
        DoctorEventCode::Touch,
        DoctorCallbackSource::KeyAction,
        scope,
        DoctorEventMode::Liu,
        DoctorEventPhase::Touch,
        true,
        true,
        DoctorResultCode::Ok,
        1,
        500 + sequence,
        1,
        true,
        std::nullopt,
        std::nullopt,
    };
}

static DoctorEventRecord health_record(
    const DoctorStore &store,
    std::int64_t sequence,
    const ManualDoctorClock &clock,
    DoctorInputScope scope) {
    return DoctorEventRecord{
        42,
        7,
        store.status().epoch,
        0,
        sequence,
        clock.monotonic_ns() + sequence,
        clock.wall_utc_ms(),
        480,
        DoctorEventCode::Health,
        DoctorCallbackSource::None,
        scope,
        DoctorEventMode::None,
        DoctorEventPhase::Health,
        false,
        true,
        DoctorResultCode::Ok,
        0,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
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

static std::string read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

static bool write_bytes(const fs::path &path, const std::string &bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

static bool append_bytes(const fs::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

static std::vector<fs::path> matching_files(
    const fs::path &directory,
    const std::function<bool(const fs::path &)> &predicate) {
    std::vector<fs::path> paths;
    std::error_code error;
    if (!fs::is_directory(directory, error)) return paths;
    for (fs::directory_iterator current(directory, error), end;
        !error && current != end; current.increment(error)) {
        if (predicate(current->path())) paths.push_back(current->path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

static bool is_doctor_record_name(const fs::path &path) {
    const auto name = path.filename().string();
    constexpr const char prefix[] = "doctor-record-";
    constexpr const char suffix[] = ".jsonl";
    if (name.size() <= std::size(prefix) - 1 + std::size(suffix) - 1 ||
        name.rfind(prefix, 0) != 0 ||
        name.substr(name.size() - (std::size(suffix) - 1)) != suffix)
        return false;
    const auto ordinal = name.substr(
        std::size(prefix) - 1,
        name.size() - (std::size(prefix) - 1) - (std::size(suffix) - 1));
    return !ordinal.empty() && std::all_of(
        ordinal.begin(), ordinal.end(),
        [](char value) { return value >= '0' && value <= '9'; });
}

static bool is_doctor_export_name(const fs::path &path) {
    const auto name = path.filename().string();
    constexpr const char prefix[] = "doctor-export-";
    constexpr const char suffix[] = ".zip";
    return name.size() > std::size(prefix) - 1 + std::size(suffix) - 1 &&
        name.rfind(prefix, 0) == 0 &&
        name.substr(name.size() - (std::size(suffix) - 1)) == suffix;
}

static std::vector<fs::path> doctor_record_files(const fs::path &root) {
    return matching_files(root / "records", is_doctor_record_name);
}

static std::vector<fs::path> doctor_export_files(const fs::path &root) {
    return matching_files(root / "temp", is_doctor_export_name);
}

static std::uint64_t files_size(const std::vector<fs::path> &paths) {
    std::uint64_t result = 0;
    for (const auto &path : paths) {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (!error) result += static_cast<std::uint64_t>(size);
    }
    return result;
}

struct StoreContext {
    StoreContext(
        fs::path root_path,
        DoctorStoreLimits configured_limits = DoctorStoreLimits::defaults())
        : root(std::move(root_path)), limits(configured_limits) {}

    std::unique_ptr<DoctorStore> open() {
        return DoctorStore::open(root, kBuild, clock, limits, &faults);
    }

    fs::path root;
    ManualDoctorClock clock;
    DoctorStoreLimits limits;
    ScriptedDoctorIoFaults faults;
};

static int test_cached_retained_range_follows_expiry_and_clear() {
    TempDirectory temporary;
    auto limits = DoctorStoreLimits::defaults();
    limits.max_age_ms = 2'000;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(!store->status().has_retained_range);
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    context.clock.advance(1'000);
    CHECK(store->append(event_record(*store, 2, context.clock)).ok());
    CHECK(store->status().has_retained_range);
    CHECK(store->status().oldest_retained_ms == kStartWallMs);
    CHECK(store->status().newest_retained_ms == kStartWallMs + 1'000);
    context.clock.advance(1'500);
    CHECK(store->read_records().value.size() == 1);
    CHECK(store->status().oldest_retained_ms == kStartWallMs + 1'000);
    store.reset();
    store = context.open();
    CHECK(store->status().has_retained_range);
    CHECK(store->status().oldest_retained_ms == kStartWallMs + 1'000);
    CHECK(store->clear().ok());
    CHECK(!store->status().has_retained_range);
    CHECK(store->status().oldest_retained_ms == 0 && store->status().newest_retained_ms == 0);
    return 0;
}

static int test_defaults_consent_and_lifecycle() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store);
    CHECK(!store->status().enabled);
    CHECK(!store->status().collecting);
    CHECK(store->append(event_record(*store, 1, context.clock)).failure ==
        DoctorStoreFailure::Disabled);

    CHECK(store->enable().ok());
    const auto first = event_record(*store, 1, context.clock);
    CHECK(store->append(first).ok());
    store.reset();

    auto reopened = context.open();
    CHECK(reopened->status().enabled);
    CHECK(reopened->status().collecting);
    auto retained = reopened->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.size() == 1);
    CHECK(same_event(retained.value.front(), first));

    CHECK(reopened->disable().ok());
    CHECK(reopened->append(event_record(*reopened, 2, context.clock)).failure ==
        DoctorStoreFailure::Disabled);
    reopened.reset();

    auto stopped = context.open();
    CHECK(!stopped->status().enabled);
    CHECK(!stopped->status().collecting);
    retained = stopped->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.size() == 1);
    CHECK(same_event(retained.value.front(), first));
    return 0;
}

static int test_fixed_failures_and_real_io() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    context.faults.fail_next(
        DoctorIoOperation::PersistControl,
        DoctorStoreFailure::PermissionDenied,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));
    auto store = context.open();
    CHECK(store->enable().failure == DoctorStoreFailure::PermissionDenied);
    CHECK(store->status().failure ==
        DoctorStoreFailure::PermissionDenied);
    CHECK(store->status().platform_result_code ==
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));
    CHECK(!store->status().enabled);
    CHECK(store->enable().ok());

    const auto first = event_record(*store, 1, context.clock);
    CHECK(store->append(first).ok());
    context.faults.fail_next(
        DoctorIoOperation::AppendRecord,
        DoctorStoreFailure::DiskFull,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
    const auto failed = store->append(event_record(*store, 2, context.clock));
    CHECK(failed.failure == DoctorStoreFailure::DiskFull);
    CHECK(failed.platform_result_code ==
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
    CHECK(store->status().failure == DoctorStoreFailure::DiskFull);
    const auto retained = store->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.size() == 1);
    CHECK(same_event(retained.value.front(), first));

    std::error_code error;
    fs::remove_all(context.root / "records", error);
    CHECK(!error);
    CHECK(write_bytes(context.root / "records", "not a directory"));
    const auto actual_io = store->append(
        event_record(*store, 2, context.clock));
    CHECK(actual_io.failure == DoctorStoreFailure::IoError);
    CHECK(actual_io.platform_result_code != 0);
    return 0;
}

static int test_rotation_limits_and_record_bound() {
    auto limits = DoctorStoreLimits::defaults();
    limits.max_raw_bytes = 2'200;
    limits.max_record_bytes = 700;
    TempDirectory temporary;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(store->enable().ok());

    std::vector<DoctorEventRecord> events;
    for (std::int64_t sequence = 1; sequence <= 8; ++sequence) {
        context.clock.advance(10);
        auto record = event_record(*store, sequence, context.clock);
        CHECK(store->append(record).ok());
        events.push_back(record);
    }

    const auto files = doctor_record_files(context.root);
    CHECK(files.size() > 1);
    const auto raw_bytes = files_size(files);
    CHECK(raw_bytes <= limits.max_raw_bytes);
    CHECK(std::all_of(files.begin(), files.end(), [&](const fs::path &path) {
        std::error_code error;
        return fs::file_size(path, error) <= limits.max_record_bytes;
    }));
    CHECK(store->status().raw_bytes == raw_bytes);
    const auto retained = store->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.size() < events.size());
    CHECK(same_event(retained.value.back(), events.back()));
    CHECK(store->status().sequence_gap > 0);

    auto tiny_limits = DoctorStoreLimits::defaults();
    tiny_limits.max_record_bytes = 64;
    TempDirectory tiny_temporary;
    StoreContext tiny_context(tiny_temporary.path(), tiny_limits);
    auto tiny = tiny_context.open();
    CHECK(tiny->enable().ok());
    CHECK(tiny->append(event_record(*tiny, 1, tiny_context.clock)).failure ==
        DoctorStoreFailure::RecordTooLarge);
    CHECK(doctor_record_files(tiny_context.root).empty());
    return 0;
}

static int test_rotation_fault_is_at_real_boundary() {
    auto limits = DoctorStoreLimits::defaults();
    limits.max_raw_bytes = 8'000;
    limits.max_record_bytes = 700;
    TempDirectory temporary;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto first = event_record(*store, 1, context.clock);
    CHECK(store->append(first).ok());
    context.faults.fail_next(
        DoctorIoOperation::RotateRecord,
        DoctorStoreFailure::PermissionDenied,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));
    CHECK(store->append(event_record(*store, 2, context.clock)).failure ==
        DoctorStoreFailure::PermissionDenied);
    const auto retained = store->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.size() == 1);
    CHECK(same_event(retained.value.front(), first));
    return 0;
}

static int test_age_cleanup_at_read_and_startup() {
    auto limits = DoctorStoreLimits::defaults();
    limits.max_age_ms = 1'000;

    TempDirectory read_temporary;
    StoreContext read_context(read_temporary.path(), limits);
    auto store = read_context.open();
    CHECK(store->enable().ok());
    const auto original = event_record(*store, 1, read_context.clock);
    CHECK(store->append(original).ok());
    read_context.clock.set_wall_utc_ms(kStartWallMs + 1'000);
    auto retained = store->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.size() == 1);
    CHECK(same_event(retained.value.front(), original));
    read_context.clock.advance(1);
    retained = store->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.empty());
    CHECK(store->status().raw_bytes == 0);
    CHECK(doctor_record_files(read_context.root).empty());
    store.reset();

    TempDirectory append_temporary;
    StoreContext append_context(append_temporary.path(), limits);
    auto appending = append_context.open();
    CHECK(appending->enable().ok());
    CHECK(appending->append(
        event_record(*appending, 1, append_context.clock)).ok());
    append_context.clock.advance(1'001);
    const auto newest = event_record(*appending, 2, append_context.clock);
    CHECK(appending->append(newest).ok());
    const auto after_append = appending->read_records();
    CHECK(after_append.ok());
    CHECK(after_append.value.size() == 1);
    CHECK(same_event(after_append.value.front(), newest));
    CHECK(appending->status().sequence_gap > 0);
    appending.reset();

    TempDirectory startup_temporary;
    StoreContext startup_context(startup_temporary.path(), limits);
    auto before = startup_context.open();
    CHECK(before->enable().ok());
    CHECK(before->append(event_record(*before, 1, startup_context.clock)).ok());
    before.reset();
    startup_context.clock.advance(1'001);
    auto restarted = startup_context.open();
    retained = restarted->read_records();
    CHECK(retained.ok());
    CHECK(retained.value.empty());
    CHECK(restarted->status().raw_bytes == 0);
    CHECK(doctor_record_files(startup_context.root).empty());
    return 0;
}

static int test_clock_jump_preserves_sequence_order() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());

    context.clock.set_wall_utc_ms(
        kStartWallMs - 10 * 24 * 60 * 60 * 1'000);
    context.clock.advance_monotonic(1);
    CHECK(store->append(event_record(*store, 2, context.clock)).ok());
    store.reset();

    context.clock.reset_monotonic();
    auto reopened = context.open();
    context.clock.advance_monotonic(1);
    CHECK(reopened->append(
        event_record(*reopened, 3, context.clock)).ok());
    const auto records = reopened->read_records();
    CHECK(records.ok());
    CHECK(records.value.size() == 3);
    CHECK(records.value[0].sequence == 1);
    CHECK(records.value[1].sequence == 2);
    CHECK(records.value[2].sequence == 3);
    return 0;
}

static int test_torn_and_malformed_records_are_visible(
    const fs::path &repo_root) {
    const auto torn_tail_fixture = read_bytes(
        repo_root / "shared" / "testdata" / "doctor-v1" / "torn-tail.jsonl");
    const auto torn_tail = torn_tail_fixture.substr(
        torn_tail_fixture.find('\n') + 1);

    TempDirectory torn_temporary;
    StoreContext torn_context(torn_temporary.path());
    auto torn_store = torn_context.open();
    CHECK(torn_store->enable().ok());
    const auto valid = event_record(*torn_store, 1, torn_context.clock);
    CHECK(torn_store->append(valid).ok());
    CHECK(!doctor_record_files(torn_context.root).empty());
    CHECK(append_bytes(
        doctor_record_files(torn_context.root).front(), torn_tail));
    torn_store.reset();

    auto torn_reopened = torn_context.open();
    const auto torn_records = torn_reopened->read_records();
    CHECK(torn_records.ok());
    CHECK(torn_records.value.size() == 1);
    CHECK(same_event(torn_records.value.front(), valid));
    CHECK(torn_reopened->status().failure == DoctorStoreFailure::TornTail);
    CHECK(torn_reopened->status().ignored_lines == 1);
    CHECK(torn_reopened->status().sequence_gap > 0);

    TempDirectory invalid_temporary;
    StoreContext invalid_context(invalid_temporary.path());
    auto invalid_store = invalid_context.open();
    CHECK(invalid_store->enable().ok());
    const auto prefix = event_record(*invalid_store, 1, invalid_context.clock);
    CHECK(invalid_store->append(prefix).ok());
    const auto bad_schema = read_bytes(
        repo_root / "shared" / "testdata" / "doctor-v1" / "bad-schema.jsonl");
    CHECK(append_bytes(
        doctor_record_files(invalid_context.root).front(), bad_schema));
    invalid_store.reset();

    auto invalid_reopened = invalid_context.open();
    const auto invalid_records = invalid_reopened->read_records();
    CHECK(invalid_records.ok());
    CHECK(invalid_records.value.size() == 1);
    CHECK(same_event(invalid_records.value.front(), prefix));
    CHECK(invalid_reopened->status().failure ==
        DoctorStoreFailure::InvalidSchema);
    CHECK(invalid_reopened->status().ignored_lines == 1);
    CHECK(invalid_reopened->status().sequence_gap > 0);
    return 0;
}

static int test_privacy_gate_and_current_epoch() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto health = health_record(
        *store, 1, context.clock, DoctorInputScope::Sensitive);
    CHECK(store->append(health).ok());
    const auto raw_before = store->status().raw_bytes;
    const auto gap_before = store->status().sequence_gap;

    for (std::int64_t index = 1; index <= 25; ++index) {
        const auto sensitive = event_record(
            *store, index + 1, context.clock, -1,
            DoctorInputScope::Sensitive);
        const auto unknown = event_record(
            *store, index + 1, context.clock, -1,
            DoctorInputScope::Unknown);
        CHECK(store->append(sensitive).failure ==
            DoctorStoreFailure::PrivacyRejected);
        CHECK(store->append(unknown).failure ==
            DoctorStoreFailure::PrivacyRejected);
    }

    CHECK(store->status().raw_bytes == raw_before);
    CHECK(store->status().sequence_gap == gap_before);
    const auto unknown_health = health_record(
        *store, 2, context.clock, DoctorInputScope::Unknown);
    CHECK(store->append(unknown_health).ok());

    auto wrong_epoch = event_record(*store, 3, context.clock);
    wrong_epoch.epoch = store->status().epoch + 1;
    CHECK(store->append(wrong_epoch).failure ==
        DoctorStoreFailure::EpochMismatch);
    wrong_epoch = event_record(*store, 3, context.clock);
    wrong_epoch.producer_id = 0;
    CHECK(store->append(wrong_epoch).failure ==
        DoctorStoreFailure::InvalidSchema);
    wrong_epoch = event_record(*store, 3, context.clock);
    wrong_epoch.session_id = 0;
    CHECK(store->append(wrong_epoch).failure ==
        DoctorStoreFailure::InvalidSchema);

    const auto records = store->read_records();
    CHECK(records.ok());
    CHECK(records.value.size() == 2);
    CHECK(same_event(records.value[0], health));
    CHECK(same_event(records.value[1], unknown_health));
    return 0;
}

static int test_restricted_health_io_gap_recovers_without_rewriting_sequence() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto first = health_record(
        *store, 1, context.clock, DoctorInputScope::Unknown);
    CHECK(store->append(first).ok());
    context.faults.fail_next(
        DoctorIoOperation::AppendRecord,
        DoctorStoreFailure::DiskFull,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
    CHECK(store->append(health_record(
        *store, 2, context.clock, DoctorInputScope::Unknown)).failure ==
        DoctorStoreFailure::DiskFull);
    const auto resumed = health_record(
        *store, 3, context.clock, DoctorInputScope::Unknown);
    CHECK(store->append(resumed).ok());
    store.reset();

    auto reopened = context.open();
    const auto records = reopened->read_records();
    CHECK(records.ok());
    CHECK(records.value.size() == 2);
    CHECK(same_event(records.value[0], first));
    CHECK(same_event(records.value[1], resumed));
    CHECK(records.value[0].sequence == 1);
    CHECK(records.value[1].sequence == 3);
    CHECK(reopened->status().sequence_gap > 0);
    return 0;
}

static int test_clear_commits_epoch_before_delete_and_retries() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    const auto old_epoch = store->status().epoch;
    const auto destination = context.root / "old-export.zip";
    context.faults.fail_next(
        DoctorIoOperation::DeleteOldRecord,
        DoctorStoreFailure::PermissionDenied,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));

    const auto clear = store->clear();
    CHECK(clear.failure == DoctorStoreFailure::ClearIncomplete);
    CHECK(store->status().epoch == old_epoch + 1);
    CHECK(store->status().enabled);
    CHECK(!store->status().collecting);
    CHECK(store->status().pending_clear);
    CHECK(store->status().failure == DoctorStoreFailure::PermissionDenied);
    CHECK(store->read_records(old_epoch).failure ==
        DoctorStoreFailure::EpochMismatch);
    CHECK(store->preview_export(snapshot.value).failure ==
        DoctorStoreFailure::EpochMismatch);
    CHECK(store->save_export(snapshot.value, destination).failure ==
        DoctorStoreFailure::EpochMismatch);
    CHECK(!fs::exists(destination));
    store.reset();

    auto resumed = context.open();
    CHECK(resumed->status().epoch == old_epoch + 1);
    CHECK(!resumed->status().pending_clear);
    CHECK(resumed->status().collecting);
    CHECK(resumed->status().failure == DoctorStoreFailure::None);
    const auto records = resumed->read_records();
    CHECK(records.ok());
    CHECK(records.value.empty());
    CHECK(doctor_record_files(context.root).empty());
    CHECK(!fs::exists(snapshot.value.archive));
    CHECK(resumed->append(event_record(*resumed, 1, context.clock)).ok());
    return 0;
}

static int test_epoch_commit_failure_fails_closed_and_persists() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto old_epoch = store->status().epoch;
    context.faults.fail_next(
        DoctorIoOperation::CommitEpoch,
        DoctorStoreFailure::DiskFull,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_DISK_FULL)));

    const auto failed = store->clear();
    CHECK(failed.failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->status().epoch == old_epoch);
    CHECK(!store->status().enabled);
    CHECK(!store->status().collecting);
    CHECK(!store->status().pending_clear);
    CHECK(store->status().failure ==
        DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->append(event_record(*store, 2, context.clock)).failure ==
        DoctorStoreFailure::Disabled);
    store.reset();

    auto reopened = context.open();
    CHECK(!reopened->status().enabled);
    CHECK(!reopened->status().collecting);
    CHECK(reopened->status().failure ==
        DoctorStoreFailure::EpochCommitFailed);
    CHECK(reopened->clear().ok());
    CHECK(reopened->status().epoch == old_epoch + 1);
    CHECK(reopened->status().failure == DoctorStoreFailure::None);
    const auto records = reopened->read_records();
    CHECK(records.ok());
    CHECK(records.value.empty());
    return 0;
}

static int test_allowlist_and_reparse_never_read_arbitrary_files() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(write_bytes(context.root / "records" / "foreign.jsonl",
        "arbitrary user data"));
    CHECK(write_bytes(context.root / "records" / "doctor-record-999.jsonl.bak",
        "arbitrary user data"));
    CHECK(write_bytes(context.root / "temp" / "doctor-export-old.zip.bak",
        "arbitrary user data"));

    const auto linked_record = event_record(*store, 99, context.clock);
    const auto encoded = DoctorEventCodec::encode_line(kBuild, linked_record);
    CHECK(encoded.failure == DoctorEventFailure::None);
    const auto outside = temporary.path() / "outside-user-record.jsonl";
    CHECK(write_bytes(outside, encoded.line + "\n"));
    const auto link = temporary.path() / "records" /
        "doctor-record-999999.jsonl";
    const BOOL linked = CreateSymbolicLinkW(
        link.c_str(), outside.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (linked) {
        const auto records = store->read_records();
        CHECK(records.ok());
        CHECK(records.value.empty());
        const auto attributes = GetFileAttributesW(link.c_str());
        CHECK(attributes != INVALID_FILE_ATTRIBUTES);
        CHECK(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    const auto records = doctor_record_files(context.root);
    CHECK(std::none_of(records.begin(), records.end(), [&](const fs::path &path) {
        return path.filename() == "foreign.jsonl" ||
            path.filename() == "doctor-record-999.jsonl.bak";
    }));
    CHECK(store->status().raw_bytes == 0);
    CHECK(store->read_records().value.empty());

    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    std::error_code removal_error;
    CHECK(fs::remove(snapshot.value.archive, removal_error));
    CHECK(!removal_error);
    const BOOL export_linked = CreateSymbolicLinkW(
        snapshot.value.archive.c_str(), outside.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (export_linked) {
        const auto leaked_preview = store->preview_export(snapshot.value);
        CHECK(leaked_preview.failure == DoctorStoreFailure::IoError);
        const auto leaked_destination = context.root / "leaked-export.zip";
        const auto leaked_save = store->save_export(
            snapshot.value, leaked_destination);
        CHECK(leaked_save.failure == DoctorStoreFailure::IoError);
        CHECK(!fs::exists(leaked_destination));
        CHECK(read_bytes(outside) == encoded.line + "\n");
        CHECK(store->cancel_export(snapshot.value).ok());
        CHECK(fs::exists(outside));
    } else {
        CHECK(store->cancel_export(snapshot.value).ok());
    }
    return 0;
}

static std::uint16_t read_u16(
    const std::string &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset]) |
        (static_cast<unsigned char>(bytes[offset + 1]) << 8));
}

static std::uint32_t read_u32(
    const std::string &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(bytes[offset])) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

static std::uint32_t crc32(const std::string &bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

struct ParsedZipEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::string data;
};

static bool parse_zip(
    const std::string &bytes,
    std::map<std::string, ParsedZipEntry> *entries) {
    entries->clear();
    if (bytes.size() < 22) return false;
    std::size_t end = bytes.size() - 22;
    while (end > 0 && read_u32(bytes, end) != 0x06054b50U) --end;
    if (read_u32(bytes, end) != 0x06054b50U) return false;
    if (read_u16(bytes, end + 4) != 0 ||
        read_u16(bytes, end + 6) != 0) return false;
    const auto total_entries = read_u16(bytes, end + 10);
    const auto central_size = read_u32(bytes, end + 12);
    const auto central_offset = read_u32(bytes, end + 16);
    if (central_offset + central_size > end) return false;
    if (read_u16(bytes, end + 8) != total_entries) return false;

    std::size_t cursor = central_offset;
    for (std::uint16_t index = 0; index < total_entries; ++index) {
        if (cursor + 46 > bytes.size() ||
            read_u32(bytes, cursor) != 0x02014b50U) return false;
        const auto method = read_u16(bytes, cursor + 10);
        const auto crc = read_u32(bytes, cursor + 16);
        const auto compressed_size = read_u32(bytes, cursor + 20);
        const auto uncompressed_size = read_u32(bytes, cursor + 24);
        const auto name_size = read_u16(bytes, cursor + 28);
        const auto extra_size = read_u16(bytes, cursor + 30);
        const auto comment_size = read_u16(bytes, cursor + 32);
        const auto local_offset = read_u32(bytes, cursor + 42);
        if (cursor + 46 + name_size + extra_size + comment_size >
            bytes.size()) return false;
        const auto name = bytes.substr(cursor + 46, name_size);
        if (local_offset + 30 > bytes.size() ||
            read_u32(bytes, local_offset) != 0x04034b50U) return false;
        const auto local_method = read_u16(bytes, local_offset + 8);
        const auto local_name_size = read_u16(bytes, local_offset + 26);
        const auto local_extra_size = read_u16(bytes, local_offset + 28);
        const auto data_offset =
            static_cast<std::size_t>(local_offset) +
            30 + local_name_size + local_extra_size;
        if (data_offset + compressed_size > bytes.size()) return false;
        if (bytes.substr(local_offset + 30, local_name_size) != name ||
            local_method != method) return false;
        ParsedZipEntry entry;
        entry.name = name;
        entry.method = method;
        entry.crc = crc;
        entry.compressed_size = compressed_size;
        entry.uncompressed_size = uncompressed_size;
        entry.data = bytes.substr(data_offset, compressed_size);
        if (method != 0 || compressed_size != uncompressed_size ||
            crc32(entry.data) != crc) return false;
        if (!entries->emplace(name, std::move(entry)).second) return false;
        cursor += 46 + name_size + extra_size + comment_size;
    }
    return cursor == central_offset + central_size &&
        entries->size() == total_entries;
}

static std::string encoded_line(const DoctorEventRecord &record) {
    const auto encoded = DoctorEventCodec::encode_line(kBuild, record);
    return encoded.failure == DoctorEventFailure::None
        ? encoded.line + "\n" : std::string();
}

static int test_export_snapshot_window_bytes_and_allowlist() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto selected = kStartWallMs;
    const std::vector<DoctorEventRecord> events{
        event_record(*store, 1, context.clock, selected - 300'001),
        event_record(*store, 2, context.clock, selected - 300'000),
        event_record(*store, 3, context.clock, selected),
        event_record(*store, 4, context.clock, selected + 30'000),
        event_record(*store, 5, context.clock, selected + 30'001),
    };
    for (const auto &record : events) CHECK(store->append(record).ok());
    CHECK(write_bytes(context.root / "unrelated.txt", "not doctor data"));
    CHECK(write_bytes(context.root / "records" / "foreign.jsonl",
        encoded_line(event_record(*store, 99, context.clock))));
    context.clock.set_wall_utc_ms(selected + 60'000);

    const auto mark = store->mark_issue(selected);
    CHECK(mark.ok());
    CHECK(mark.value.action_time_ms == selected + 60'000);
    CHECK(mark.value.selected_issue_time_ms == selected);
    CHECK(mark.value.window.before_ms == 300'000);
    CHECK(mark.value.window.after_ms == 30'000);
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    const auto archive_path = snapshot.value.archive;
    CHECK(is_doctor_export_name(archive_path));
    const auto fixed_zip_bytes = read_bytes(archive_path);
    CHECK(fixed_zip_bytes.size() >= 4);
    CHECK(read_u32(fixed_zip_bytes, 0) == 0x04034b50U);

    std::map<std::string, ParsedZipEntry> entries;
    CHECK(parse_zip(fixed_zip_bytes, &entries));
    const auto expected_names = std::set<std::string>{
        "doctor/manifest-v1.json",
        "doctor/mark-v1.json",
        "doctor/events-v1.jsonl",
    };
    std::set<std::string> names;
    for (const auto &entry : entries) names.insert(entry.first);
    CHECK(names == expected_names);
    CHECK(entries.at("doctor/events-v1.jsonl").data ==
        encoded_line(events[1]) + encoded_line(events[2]) +
        encoded_line(events[3]));
    CHECK(entries.at("doctor/mark-v1.json").data.find(
        "\"actionTimeMs\":" + std::to_string(mark.value.action_time_ms)) !=
        std::string::npos);
    CHECK(entries.at("doctor/mark-v1.json").data.find(
        "\"selectedIssueTimeMs\":" + std::to_string(selected)) !=
        std::string::npos);
    CHECK(entries.at("doctor/mark-v1.json").data.find(
        "\"beforeMs\":300000") != std::string::npos);
    CHECK(entries.at("doctor/mark-v1.json").data.find(
        "\"afterMs\":30000") != std::string::npos);
    for (const auto &entry : entries) {
        CHECK(entry.second.data.find("foreign") == std::string::npos);
        CHECK(entry.second.data.find("unrelated") == std::string::npos);
        CHECK(entry.second.data.find(temporary.path().string()) ==
            std::string::npos);
    }
    const auto preview = store->preview_export(snapshot.value);
    CHECK(preview.ok());
    CHECK(preview.value.epoch == store->status().epoch);
    CHECK(preview.value.record_count == 3);
    CHECK(preview.value.missing_before_ms == 0);
    CHECK(preview.value.missing_after_ms == 0);
    CHECK(read_bytes(archive_path) == fixed_zip_bytes);
    CHECK(store->read_records().value.size() == 5);

    const auto destination = context.root / "saved-doctor.zip";
    CHECK(store->save_export(snapshot.value, destination).ok());
    CHECK(fs::exists(destination));
    CHECK(read_bytes(destination) == fixed_zip_bytes);
    CHECK(!fs::exists(archive_path));
    CHECK(store->status().temp_bytes == 0);

    const auto custom = store->mark_issue(
        selected, DoctorExportWindow{1'000, 1'000});
    CHECK(custom.ok());
    const auto custom_snapshot = store->prepare_export(custom.value);
    CHECK(custom_snapshot.ok());
    const auto custom_preview = store->preview_export(custom_snapshot.value);
    CHECK(custom_preview.ok());
    CHECK(custom_preview.value.record_count == 1);
    CHECK(store->cancel_export(custom_snapshot.value).ok());
    CHECK(!fs::exists(custom_snapshot.value.archive));
    return 0;
}

static int test_export_preview_reports_unavailable_selection_edges() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto selected = context.clock.wall_utc_ms();
    CHECK(store->append(event_record(
        *store, 1, context.clock, selected)).ok());
    const auto mark = store->mark_issue(selected);
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    const auto preview = store->preview_export(snapshot.value);
    CHECK(preview.ok());
    CHECK(preview.value.record_count == 1);
    CHECK(preview.value.missing_before_ms > 0);
    CHECK(preview.value.missing_after_ms > 0);
    CHECK(preview.value.sequence_gap == 0);
    CHECK(store->cancel_export(snapshot.value).ok());
    return 0;
}

static int test_one_snapshot_cancel_expiry_and_unique_paths() {
    auto limits = DoctorStoreLimits::defaults();
    limits.export_ttl_ms = 1'000;
    TempDirectory temporary;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto first = store->prepare_export(mark.value);
    CHECK(first.ok());
    const auto first_bytes = read_bytes(first.value.archive);
    CHECK(store->prepare_export(mark.value).failure ==
        DoctorStoreFailure::ExportExists);
    CHECK(store->preview_export(first.value).ok());
    CHECK(read_bytes(first.value.archive) == first_bytes);
    CHECK(store->cancel_export(first.value).ok());
    CHECK(!fs::exists(first.value.archive));
    CHECK(store->status().temp_bytes == 0);

    const auto second = store->prepare_export(mark.value);
    CHECK(second.ok());
    CHECK(first.value.archive != second.value.archive);
    context.clock.advance(1'001);
    CHECK(store->preview_export(second.value).failure ==
        DoctorStoreFailure::ExportExpired);
    CHECK(!fs::exists(second.value.archive));
    const auto expired_destination = context.root / "expired-doctor.zip";
    CHECK(store->save_export(second.value, expired_destination).failure ==
        DoctorStoreFailure::ExportExpired);
    CHECK(!fs::exists(expired_destination));
    CHECK(store->status().temp_bytes == 0);
    CHECK(doctor_export_files(context.root).empty());
    return 0;
}

static int test_save_failure_and_disabled_same_epoch_snapshot() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    const auto epoch = snapshot.value.epoch;
    const auto destination = context.root / "picker-target.zip";
    context.faults.fail_next(
        DoctorIoOperation::SaveExport,
        DoctorStoreFailure::PermissionDenied,
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));

    CHECK(store->disable().ok());
    store.reset();
    auto stopped = context.open();
    const auto failed = stopped->save_export(snapshot.value, destination);
    CHECK(failed.failure == DoctorStoreFailure::PermissionDenied);
    CHECK(failed.platform_result_code ==
        static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));
    CHECK(fs::exists(snapshot.value.archive));
    CHECK(!fs::exists(destination));
    CHECK(stopped->save_export(snapshot.value, destination).ok());
    CHECK(stopped->status().epoch == epoch);
    CHECK(!fs::exists(snapshot.value.archive));
    CHECK(fs::exists(destination));
    CHECK(stopped->status().temp_bytes == 0);
    CHECK(stopped->read_records().value.size() == 1);
    return 0;
}

static int test_export_create_fault_cleans_partial_temp() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    context.faults.fail_next(
        DoctorIoOperation::CreateExport,
        DoctorStoreFailure::IoError,
        static_cast<std::int32_t>(E_FAIL));
    const auto failed = store->prepare_export(mark.value);
    CHECK(failed.failure == DoctorStoreFailure::IoError);
    CHECK(store->status().temp_bytes == 0);
    CHECK(doctor_export_files(context.root).empty());
    return 0;
}

static int test_default_capacity_and_small_limits() {
    const auto defaults = DoctorStoreLimits::defaults();
    CHECK(defaults.max_raw_bytes == 32ULL * 1024 * 1024);
    CHECK(defaults.max_age_ms == 7LL * 24 * 60 * 60 * 1'000);
    CHECK(defaults.max_export_bytes == 32ULL * 1024 * 1024);
    CHECK(defaults.max_raw_and_temp_bytes == 64ULL * 1024 * 1024);

    auto export_limits = DoctorStoreLimits::defaults();
    export_limits.max_export_bytes = 128;
    TempDirectory export_temporary;
    StoreContext export_context(export_temporary.path(), export_limits);
    auto export_store = export_context.open();
    CHECK(export_store->enable().ok());
    for (std::int64_t sequence = 1; sequence <= 3; ++sequence)
        CHECK(export_store->append(
            event_record(*export_store, sequence, export_context.clock)).ok());
    const auto export_mark = export_store->mark_issue(
        export_context.clock.wall_utc_ms());
    CHECK(export_mark.ok());
    CHECK(export_store->prepare_export(export_mark.value).failure ==
        DoctorStoreFailure::ExportLimit);
    CHECK(export_store->status().temp_bytes == 0);
    CHECK(doctor_export_files(export_context.root).empty());

    TempDirectory total_temporary;
    StoreContext unrestricted_context(total_temporary.path());
    auto unrestricted = unrestricted_context.open();
    CHECK(unrestricted->enable().ok());
    CHECK(unrestricted->append(
        event_record(*unrestricted, 1, unrestricted_context.clock)).ok());
    CHECK(unrestricted->append(
        event_record(*unrestricted, 2, unrestricted_context.clock)).ok());
    const auto raw_bytes = unrestricted->status().raw_bytes;
    CHECK(raw_bytes > 0);
    unrestricted.reset();

    auto total_limits = DoctorStoreLimits::defaults();
    total_limits.max_raw_and_temp_bytes = raw_bytes + 1;
    StoreContext constrained_context(total_temporary.path(), total_limits);
    auto constrained = constrained_context.open();
    const auto total_mark = constrained->mark_issue(
        constrained_context.clock.wall_utc_ms());
    CHECK(total_mark.ok());
    CHECK(constrained->prepare_export(total_mark.value).failure ==
        DoctorStoreFailure::TotalLimit);
    CHECK(constrained->status().temp_bytes == 0);
    CHECK(doctor_export_files(constrained_context.root).empty());
    return 0;
}

static int test_export_cleanup_removes_expired_raw_before_selection() {
    auto limits = DoctorStoreLimits::defaults();
    limits.max_age_ms = 1'000;
    TempDirectory temporary;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    context.clock.advance(1'001);
    const auto mark = store->mark_issue(kStartWallMs);
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    CHECK(store->preview_export(snapshot.value).value.record_count == 0);
    CHECK(store->status().raw_bytes == 0);
    CHECK(doctor_record_files(context.root).empty());
    std::map<std::string, ParsedZipEntry> entries;
    CHECK(parse_zip(read_bytes(snapshot.value.archive), &entries));
    CHECK(entries.at("doctor/events-v1.jsonl").data.empty());
    CHECK(store->cancel_export(snapshot.value).ok());
    return 0;
}

static bool native_symbolic_link(const fs::path &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return false;
    HANDLE file = CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool read = GetFileInformationByHandleEx(
        file, FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE;
    CloseHandle(file);
    return read && tag.ReparseTag == IO_REPARSE_TAG_SYMLINK;
}

static int test_clear_removes_owned_record_staging_before_success(
    bool require_link = false) {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto stage = context.root / "records" / "doctor-record-91.jsonl.tmp";
    const auto foreign = context.root / "records" / "foreign.txt";
    CHECK(write_bytes(stage, "old anonymous events"));
    CHECK(write_bytes(foreign, "unrelated user data"));
    const auto outside = temporary.path() / "outside.txt";
    const auto link = context.root / "records" / "doctor-record-92.jsonl.tmp";
    CHECK(write_bytes(outside, "outside sentinel"));
    const bool linked = CreateSymbolicLinkW(link.c_str(), outside.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    const DWORD link_error = linked ? ERROR_SUCCESS : GetLastError();
    if (require_link && !linked) {
        std::cerr << "Required symlink fixture unavailable; Win32 error "
            << link_error << '\n';
        return 1;
    }
    if (linked) CHECK(native_symbolic_link(link));
    const auto old_epoch = store->status().epoch;
    CHECK(store->clear().ok());
    CHECK(store->status().epoch == old_epoch + 1);
    CHECK(!fs::exists(stage));
    CHECK(read_bytes(foreign) == "unrelated user data");
    CHECK(read_bytes(outside) == "outside sentinel");
    // Some MinGW filesystem implementations report a real Windows symbolic
    // link as a regular file. Verify the native reparse tag without following it.
    if (linked) {
        CHECK(native_symbolic_link(link));
        std::cout << "clear-staging native symlink preservation exercised\n";
    } else {
        std::cout << "SKIP clear-staging symlink fixture; Win32 error "
            << link_error << '\n';
    }
    return 0;
}

static int test_numeric_segment_order_after_tenth_rotation() {
    TempDirectory temporary;
    auto limits = DoctorStoreLimits::defaults();
    limits.max_record_bytes = 600;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(store->enable().ok());
    for (int sequence = 1; sequence <= 12; ++sequence)
        CHECK(store->append(event_record(*store, sequence, context.clock)).ok());
    CHECK(doctor_record_files(context.root).size() >= 12);
    auto records = store->read_records();
    CHECK(records.ok() && records.value.size() == 12);
    for (int index = 0; index != 12; ++index)
        CHECK(records.value[index].sequence == index + 1);
    CHECK(store->status().sequence_gap == 0);
    CHECK(store->append(event_record(*store, 13, context.clock)).ok());
    store.reset();
    store = context.open();
    records = store->read_records();
    CHECK(records.ok() && records.value.size() == 13);
    for (int index = 0; index != 13; ++index)
        CHECK(records.value[index].sequence == index + 1);
    CHECK(store->status().sequence_gap == 0);
    return 0;
}

static int test_pending_clear_blocks_operations_and_retries_same_epoch() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    context.faults.fail_next(DoctorIoOperation::DeleteOldRecord, DoctorStoreFailure::PermissionDenied);
    CHECK(store->clear().failure == DoctorStoreFailure::ClearIncomplete);
    const auto epoch = store->status().epoch;
    CHECK(store->read_records().failure == DoctorStoreFailure::ClearIncomplete);
    CHECK(store->mark_issue(context.clock.wall_utc_ms()).failure == DoctorStoreFailure::ClearIncomplete);
    CHECK(store->enable().failure == DoctorStoreFailure::ClearIncomplete);
    CHECK(store->disable().failure == DoctorStoreFailure::ClearIncomplete);
    CHECK(store->clear().ok());
    CHECK(store->status().epoch == epoch);
    CHECK(store->read_records().ok());
    CHECK(store->read_records().value.empty());
    return 0;
}

static int test_failed_epoch_cannot_be_cleared_by_enable_or_disable() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    context.faults.fail_next(DoctorIoOperation::CommitEpoch, DoctorStoreFailure::DiskFull);
    CHECK(store->clear().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->enable().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->disable().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(!store->status().collecting);
    CHECK(!store->status().enabled);
    return 0;
}

static int test_clear_double_write_failure_blocks_history_after_reopen() {
    TempDirectory temporary;
    ManualDoctorClock clock;
    struct ClearFaults final : DoctorIoFaults {
        fs::path control;
        bool armed = false;
        bool made_read_only = false;
        ~ClearFaults() override { SetFileAttributesW(control.c_str(), FILE_ATTRIBUTE_NORMAL); }
        std::optional<DoctorIoFault> before(DoctorIoOperation operation) override {
            if (armed && operation == DoctorIoOperation::CommitEpoch) {
                armed = false;
                made_read_only = SetFileAttributesW(control.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
                return DoctorIoFault{DoctorStoreFailure::DiskFull,
                    static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_DISK_FULL))};
            }
            return std::nullopt;
        }
    } faults;
    faults.control = temporary.path() / L"doctor-control-v1.bin";
    auto store = DoctorStore::open(temporary.path(), kBuild, clock, DoctorStoreLimits::defaults(), &faults);
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, clock)).ok());
    faults.armed = true;
    const auto failed = store->clear();
    CHECK(faults.made_read_only);
    CHECK(SetFileAttributesW(faults.control.c_str(), FILE_ATTRIBUTE_NORMAL));
    CHECK(failed.failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->read_records().failure == DoctorStoreFailure::EpochCommitFailed);
    store.reset();
    auto reopened = DoctorStore::open(temporary.path(), kBuild, clock);
    CHECK(!reopened->status().enabled);
    CHECK(reopened->read_records().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(reopened->clear().ok());
    CHECK(reopened->status().epoch == 1);
    CHECK(reopened->read_records().ok() && reopened->read_records().value.empty());
    return 0;
}

static int test_save_rejects_snapshot_file_and_case_alias() {
    for (bool upper_case : {false, true}) {
        TempDirectory temporary;
        StoreContext context(temporary.path());
        auto store = context.open();
        CHECK(store->enable().ok());
        CHECK(store->append(event_record(*store, 1, context.clock)).ok());
        const auto mark = store->mark_issue(context.clock.wall_utc_ms());
        CHECK(mark.ok());
        const auto snapshot = store->prepare_export(mark.value);
        CHECK(snapshot.ok());
        const auto before = read_bytes(snapshot.value.archive);
        auto name = snapshot.value.archive.filename().wstring();
        if (upper_case) std::transform(name.begin(), name.end(), name.begin(), ::towupper);
        const auto destination = snapshot.value.archive.parent_path() / name;
        CHECK(!store->save_export(snapshot.value, destination).ok());
        CHECK(read_bytes(snapshot.value.archive) == before);
        CHECK(store->preview_export(snapshot.value).ok());
    }
    return 0;
}

static int test_common_export_metadata(const fs::path &repo_root) {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    std::map<std::string, ParsedZipEntry> entries;
    CHECK(parse_zip(read_bytes(snapshot.value.archive), &entries));
    auto expected_manifest = read_bytes(repo_root / "shared/testdata/doctor-v1/export-manifest.json");
    auto expected_mark = read_bytes(repo_root / "shared/testdata/doctor-v1/export-mark.json");
    CHECK(!expected_manifest.empty() && !expected_mark.empty());
    expected_manifest.pop_back();
    expected_mark.pop_back();
    CHECK(entries.at("doctor/manifest-v1.json").data == expected_manifest);
    CHECK(entries.at("doctor/mark-v1.json").data == expected_mark);
    CHECK(entries.at("doctor/events-v1.jsonl").data.empty());
    return 0;
}

static int test_common_archive_rejects_extra_payload(const fs::path &repo_root) {
    for (const auto &name : {"export-contract.zip", "export-unknown-field.zip", "export-bad-crc.zip", "export-deflated.zip"}) {
        TempDirectory temporary;
        StoreContext context(temporary.path());
        auto store = context.open();
        CHECK(store->enable().ok());
        const auto mark = store->mark_issue(context.clock.wall_utc_ms());
        CHECK(mark.ok());
        const auto snapshot = store->prepare_export(mark.value);
        CHECK(snapshot.ok());
        const auto bytes = read_bytes(repo_root / "shared/testdata/doctor-v1" / name);
        CHECK(!bytes.empty());
        CHECK(write_bytes(snapshot.value.archive, bytes));
        if (std::string(name) == "export-contract.zip") {
            CHECK(store->preview_export(snapshot.value).ok());
        } else {
            CHECK(!store->preview_export(snapshot.value).ok());
            const auto destination = context.root / "rejected.zip";
            CHECK(!store->save_export(snapshot.value, destination).ok());
            CHECK(!fs::exists(destination));
        }
    }
    return 0;
}

static int test_retention_preserves_build_metadata_and_gap_on_reopen() {
    TempDirectory temporary;
    auto limits = DoctorStoreLimits::defaults();
    limits.max_age_ms = 10'000;
    StoreContext context(temporary.path(), limits);
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock, kStartWallMs - 5'000)).ok());
    CHECK(store->append(event_record(*store, 2, context.clock)).ok());
    store.reset();
    const DoctorBuildMetadata newer{"0.1.1", std::string(40, 'a')};
    context.clock.advance(6'000);
    store = DoctorStore::open(context.root, newer, context.clock, limits, &context.faults);
    CHECK(store->append(event_record(*store, 3, context.clock)).ok());
    const auto gap = store->status().sequence_gap;
    CHECK(gap >= 1);
    store.reset();
    store = DoctorStore::open(context.root, newer, context.clock, limits, &context.faults);
    if (store->status().sequence_gap != gap)
        std::cerr << "retention gap before reopen=" << gap << " after=" << store->status().sequence_gap << '\n';
    CHECK(store->status().sequence_gap == gap);
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    std::map<std::string, ParsedZipEntry> entries;
    CHECK(parse_zip(read_bytes(snapshot.value.archive), &entries));
    const auto decoded = DoctorEventCodec::decode_jsonl(entries.at("doctor/events-v1.jsonl").data);
    CHECK(decoded.failure == DoctorEventFailure::None && decoded.events.size() == 2);
    CHECK(decoded.events[0].event.sequence == 2);
    CHECK(decoded.events[0].build.version == kBuild.version);
    CHECK(decoded.events[0].build.source_commit == kBuild.source_commit);
    CHECK(decoded.events[1].event.sequence == 3);
    CHECK(decoded.events[1].build.version == newer.version);
    CHECK(decoded.events[1].build.source_commit == newer.source_commit);
    return 0;
}

static int test_atomic_export_preserves_old_destination_when_replace_fails() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    const auto mark = store->mark_issue(context.clock.wall_utc_ms());
    CHECK(mark.ok());
    const auto snapshot = store->prepare_export(mark.value);
    CHECK(snapshot.ok());
    const auto bytes = read_bytes(snapshot.value.archive);
    const auto destination = context.root / "download.zip";
    const auto unrelated_tmp = context.root / "download.zip.tmp";
    CHECK(write_bytes(destination, "previous user file"));
    CHECK(write_bytes(unrelated_tmp, "unrelated temporary user file"));
    const auto locked = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    const auto failed = store->save_export(snapshot.value, destination);
    CloseHandle(locked);
    CHECK(!failed.ok());
    CHECK(read_bytes(destination) == "previous user file");
    CHECK(read_bytes(unrelated_tmp) == "unrelated temporary user file");
    CHECK(store->preview_export(snapshot.value).ok());
    CHECK(store->save_export(snapshot.value, destination).ok());
    CHECK(read_bytes(destination) == bytes);
    CHECK(read_bytes(unrelated_tmp) == "unrelated temporary user file");
    return 0;
}

static int test_uncommitted_clear_intent_preserves_consent() {
    for (const bool enabled : {true, false}) {
        TempDirectory temporary;
        StoreContext context(temporary.path());
        auto store = context.open();
        CHECK(store->enable().ok());
        CHECK(store->append(event_record(*store, 1, context.clock)).ok());
        if (!enabled) CHECK(store->disable().ok());
        const auto mark = store->mark_issue(context.clock.wall_utc_ms());
        CHECK(mark.ok());
        const auto snapshot = store->prepare_export(mark.value);
        CHECK(snapshot.ok());
        const auto epoch = store->status().epoch;
        context.faults.fail_next(DoctorIoOperation::PersistControl, DoctorStoreFailure::DiskFull,
            static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
        CHECK(store->clear().failure == DoctorStoreFailure::DiskFull);
        CHECK(store->status().enabled == enabled);
        CHECK(!store->status().collecting);
        CHECK(store->status().failure == DoctorStoreFailure::EpochCommitFailed);
        CHECK(!store->status().has_retained_range);
        CHECK(store->read_records().failure == DoctorStoreFailure::EpochCommitFailed);
        CHECK(store->preview_export(snapshot.value).failure == DoctorStoreFailure::EpochCommitFailed);
        CHECK(store->append(event_record(*store, 2, context.clock)).failure ==
            (enabled ? DoctorStoreFailure::EpochCommitFailed : DoctorStoreFailure::Disabled));
        CHECK(store->clear().ok());
        CHECK(store->status().enabled == enabled);
        CHECK(store->status().collecting == enabled);
        CHECK(store->status().epoch == epoch + 1);
        CHECK(store->read_records().ok());
        CHECK(store->read_records().value.empty());
    }
    return 0;
}

static int test_clear_preparation_failure_blocks_old_data_until_retry() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    CHECK(store->enable().ok());
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    const auto epoch = store->status().epoch;
    const auto staging = context.root / L"temp";
    const auto saved_staging = context.root / L"temp-test-backup";
    CHECK(fs::weakly_canonical(staging).parent_path() ==
        fs::weakly_canonical(context.root));
    fs::rename(staging, saved_staging);
    CHECK(write_bytes(staging, "test-owned obstruction"));

    CHECK(!store->clear().ok());
    CHECK(store->status().enabled);
    CHECK(!store->status().collecting);
    CHECK(store->status().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(!store->status().has_retained_range);
    CHECK(store->read_records().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->append(event_record(*store, 2, context.clock)).failure ==
        DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->enable().failure == DoctorStoreFailure::EpochCommitFailed);

    CHECK(fs::remove(staging));
    fs::rename(saved_staging, staging);
    CHECK(store->clear().ok());
    CHECK(store->status().enabled);
    CHECK(store->status().collecting);
    CHECK(store->status().epoch == epoch + 1);
    CHECK(store->read_records().ok());
    CHECK(store->read_records().value.empty());
    return 0;
}

static int test_clear_epoch_exhaustion_blocks_old_data() {
    TempDirectory temporary;
    StoreContext context(temporary.path());
    auto store = context.open();
    store.reset();
    doctor_store_detail::ControlState control;
    control.enabled = true;
    control.epoch = std::numeric_limits<std::int64_t>::max();
    CHECK(write_bytes(context.root / L"doctor-control-v1.bin",
        doctor_store_detail::encode_control(control)));
    store = context.open();
    CHECK(store->append(event_record(*store, 1, context.clock)).ok());
    CHECK(store->clear().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(!store->status().collecting);
    CHECK(!store->status().has_retained_range);
    CHECK(store->read_records().failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->append(event_record(*store, 2, context.clock)).failure ==
        DoctorStoreFailure::EpochCommitFailed);
    CHECK(store->enable().failure == DoctorStoreFailure::EpochCommitFailed);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: doctor_store_tests <repo-root>\n";
        return 2;
    }
    const auto repo_root = fs::absolute(argv[1]).lexically_normal();
    if (argc == 2) {
        if (const auto result = test_clear_preparation_failure_blocks_old_data_until_retry()) return result;
        if (const auto result = test_clear_epoch_exhaustion_blocks_old_data()) return result;
        if (const auto result = test_uncommitted_clear_intent_preserves_consent()) return result;
        if (const auto result = test_cached_retained_range_follows_expiry_and_clear()) return result;
    }
    if (argc == 3) {
        const std::string selected = argv[2];
        if (selected == "clear-staging-symlink")
            return test_clear_removes_owned_record_staging_before_success(true);
        if (selected == "numeric-order") return test_numeric_segment_order_after_tenth_rotation();
        if (selected == "pending-clear") return test_pending_clear_blocks_operations_and_retries_same_epoch();
        if (selected == "failed-epoch") return test_failed_epoch_cannot_be_cleared_by_enable_or_disable();
        if (selected == "save-alias") return test_save_rejects_snapshot_file_and_case_alias();
        if (selected == "zip-metadata") return test_common_export_metadata(repo_root);
        if (selected == "atomic-save") return test_atomic_export_preserves_old_destination_when_replace_fails();
        if (selected == "zip-allowlist") return test_common_archive_rejects_extra_payload(repo_root);
        if (selected == "history-build") return test_retention_preserves_build_metadata_and_gap_on_reopen();
        return 2;
    }
    if (const auto result = test_numeric_segment_order_after_tenth_rotation()) return result;
    if (const auto result = test_clear_double_write_failure_blocks_history_after_reopen()) return result;
    if (const auto result = test_pending_clear_blocks_operations_and_retries_same_epoch()) return result;
    if (const auto result = test_failed_epoch_cannot_be_cleared_by_enable_or_disable()) return result;
    if (const auto result = test_save_rejects_snapshot_file_and_case_alias()) return result;
    if (const auto result = test_atomic_export_preserves_old_destination_when_replace_fails()) return result;
    if (const auto result = test_common_export_metadata(repo_root)) return result;
    if (const auto result = test_common_archive_rejects_extra_payload(repo_root)) return result;
    if (const auto result = test_retention_preserves_build_metadata_and_gap_on_reopen()) return result;
    if (const auto result = test_defaults_consent_and_lifecycle()) return result;
    if (const auto result = test_fixed_failures_and_real_io()) return result;
    if (const auto result = test_rotation_limits_and_record_bound()) return result;
    if (const auto result = test_rotation_fault_is_at_real_boundary()) return result;
    if (const auto result = test_age_cleanup_at_read_and_startup()) return result;
    if (const auto result = test_clock_jump_preserves_sequence_order()) return result;
    if (const auto result = test_torn_and_malformed_records_are_visible(repo_root)) return result;
    if (const auto result = test_privacy_gate_and_current_epoch()) return result;
    if (const auto result = test_restricted_health_io_gap_recovers_without_rewriting_sequence()) return result;
    if (const auto result = test_clear_commits_epoch_before_delete_and_retries()) return result;
    if (const auto result = test_clear_removes_owned_record_staging_before_success()) return result;
    if (const auto result = test_epoch_commit_failure_fails_closed_and_persists()) return result;
    if (const auto result = test_allowlist_and_reparse_never_read_arbitrary_files()) return result;
    if (const auto result = test_export_snapshot_window_bytes_and_allowlist()) return result;
    if (const auto result = test_export_preview_reports_unavailable_selection_edges()) return result;
    if (const auto result = test_one_snapshot_cancel_expiry_and_unique_paths()) return result;
    if (const auto result = test_save_failure_and_disabled_same_epoch_snapshot()) return result;
    if (const auto result = test_export_create_fault_cleans_partial_temp()) return result;
    if (const auto result = test_default_capacity_and_small_limits()) return result;
    if (const auto result = test_export_cleanup_removes_expired_raw_before_selection()) return result;
    std::cout << "Doctor persistence and export tests passed\n";
    return 0;
}
