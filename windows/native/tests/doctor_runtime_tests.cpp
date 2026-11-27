#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>

#include "../doctor_runtime.hpp"
#include "../doctor_store.hpp"
#include "../tsf/doctor_transport.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#define CHECK(value) \
    do { \
        if (!(value)) { \
            std::cerr << #value << " line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

using namespace boshiamy::doctor;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kStartWallMs = 1'764'057'600'000;
constexpr std::int64_t kStartMonotonicNs = 5'000'000'000;

const DoctorBuildMetadata kBuild{
    "0.1.0-runtime-test",
    "e045f978eb52001abc8b632482a6f4536cbdf2ab",
};

class TempDirectory {
public:
    TempDirectory()
        : base_(fs::weakly_canonical(fs::temp_directory_path())), path_(base_ /
            (L"liu-kai-doctor-runtime-" +
             std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) +
             L"-" + std::to_wstring(GetTickCount64()) + L"-" +
             std::to_wstring(counter()))) {
        if (!fs::create_directory(path_)) throw std::runtime_error("Test directory already exists");
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        const auto resolved = fs::weakly_canonical(path_, ignored);
        if (ignored || resolved.parent_path() != base_ ||
            resolved.filename().wstring().rfind(L"liu-kai-doctor-runtime-", 0) != 0) return;
        fs::remove_all(path_, ignored);
    }

    const fs::path &path() const noexcept { return path_; }

private:
    static volatile LONG64 next() {
        static volatile LONG64 value = 0;
        return InterlockedIncrement64(&value);
    }

    static std::uint64_t counter() {
        return static_cast<std::uint64_t>(next());
    }

    fs::path base_;
    fs::path path_;
};

class ManualDoctorClock final : public DoctorClock {
public:
    explicit ManualDoctorClock(
        std::int64_t wall_ms = kStartWallMs,
        std::int64_t monotonic_ns = kStartMonotonicNs)
        : wall_ms_(wall_ms), monotonic_ns_(monotonic_ns) {}

    std::int64_t wall_utc_ms() const override { return wall_ms_; }
    std::int64_t monotonic_ns() const override { return monotonic_ns_; }

    void advance(std::int64_t milliseconds) {
        wall_ms_ += milliseconds;
        monotonic_ns_ += milliseconds * 1'000'000;
    }

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
        const auto fault = found->second.front();
        found->second.pop_front();
        return fault;
    }

private:
    std::map<DoctorIoOperation, std::deque<DoctorIoFault>> pending_;
};

class BlockingExportFaults final : public DoctorIoFaults {
public:
    std::optional<DoctorIoFault> before(
        DoctorIoOperation operation) override {
        if (operation == DoctorIoOperation::CreateExport &&
            armed_.exchange(false)) {
            entered_.store(true);
            while (!release_.load()) Sleep(1);
        }
        return std::nullopt;
    }

    void wait_until_entered() const {
        for (int attempt = 0; attempt != 10'000 && !entered_.load(); ++attempt)
            Sleep(1);
    }

    bool entered() const { return entered_.load(); }
    void release() { release_.store(true); }

private:
    std::atomic<bool> armed_{true};
    std::atomic<bool> entered_{false};
    std::atomic<bool> release_{false};
};

static std::string read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

static bool write_bytes(const fs::path &path, const std::string &bytes) {
    std::error_code ignored;
    fs::create_directories(path.parent_path(), ignored);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

using FileSnapshot = std::map<std::string, std::string>;

static FileSnapshot snapshot_files(const fs::path &root) {
    FileSnapshot result;
    std::error_code error;
    if (!fs::exists(root, error)) return result;
    fs::recursive_directory_iterator current(root, error);
    const fs::recursive_directory_iterator end;
    while (!error && current != end) {
        if (current->is_regular_file(error) && !error) {
            result[current->path().lexically_relative(root).string()] =
                read_bytes(current->path());
        }
        current.increment(error);
    }
    return result;
}

static DoctorEventRecord ordinary_store_event(
    const DoctorStore &store,
    const ManualDoctorClock &clock,
    std::int64_t operation_id,
    std::int64_t sequence) {
    DoctorEventRecord event{};
    event.producer_id = 41;
    event.session_id = 7;
    event.epoch = store.status().epoch;
    event.operation_id = operation_id;
    event.sequence = sequence;
    event.monotonic_ns = clock.monotonic_ns();
    event.wall_utc_ms = clock.wall_utc_ms();
    event.event_code = DoctorEventCode::KeyDown;
    event.callback_source = DoctorCallbackSource::KeyAction;
    event.scope = DoctorInputScope::Ordinary;
    event.phase = DoctorEventPhase::Composition;
    event.result_code = DoctorResultCode::Ok;
    return event;
}

static int seed_pending_clear(
    const fs::path &root,
    ManualDoctorClock &clock) {
    ScriptedDoctorIoFaults faults;
    auto store = DoctorStore::open(root, kBuild, clock,
        DoctorStoreLimits::defaults(), &faults);
    CHECK(store);
    CHECK(store->enable().ok());
    CHECK(store->append(
        ordinary_store_event(*store, clock, 1, 1)).ok());
    faults.fail_next(
        DoctorIoOperation::DeleteOldRecord,
        DoctorStoreFailure::PermissionDenied,
        static_cast<std::int32_t>(
            HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));
    CHECK(store->clear().failure == DoctorStoreFailure::ClearIncomplete);
    CHECK(store->status().pending_clear);
    return 0;
}

static EventRecord queue_event(
    const ManualDoctorClock &clock,
    Event event,
    ScopeClass scope,
    std::uint64_t operation_id,
    DoctorCallbackSource source) {
    EventRecord record{};
    record.operation_id = operation_id;
    record.monotonic_ns =
        static_cast<std::uint64_t>(clock.monotonic_ns());
    record.event_code = static_cast<std::uint32_t>(event);
    record.callback_source = static_cast<std::uint32_t>(source);
    record.scope = static_cast<std::uint32_t>(scope);
    record.result_code = static_cast<std::int32_t>(S_OK);
    record.duration_us = 25;
    return record;
}

static bool decode_store_files(
    const fs::path &root,
    std::vector<DoctorEventEnvelope> *events) {
    events->clear();
    std::vector<fs::path> files;
    std::error_code error;
    const auto records = root / "records";
    if (!fs::exists(records, error) || !fs::is_directory(records, error))
        return !error;

    fs::directory_iterator current(records, error);
    const fs::directory_iterator end;
    while (!error && current != end) {
        const auto name = current->path().filename().string();
        if (current->is_regular_file(error) && !error &&
            name.rfind("doctor-record-", 0) == 0 &&
            name.size() >= 6 &&
            name.substr(name.size() - 6) == ".jsonl") {
            files.push_back(current->path());
        }
        current.increment(error);
    }
    if (error) return false;
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
        const auto decoded = DoctorEventCodec::decode_jsonl(read_bytes(file));
        if (decoded.failure != DoctorEventFailure::None) return false;
        events->insert(
            events->end(), decoded.events.begin(), decoded.events.end());
    }
    return true;
}

static std::string quote(const std::string &value) {
    return "\"" + value + "\"";
}

static bool current_executable(std::string *path) {
    char buffer[MAX_PATH * 4]{};
    const DWORD length =
        GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer)) return false;
    path->assign(buffer, length);
    return true;
}

static bool start_helper(
    const std::string &arguments,
    PROCESS_INFORMATION *child) {
    std::string executable;
    if (!current_executable(&executable)) return false;
    const std::string command = quote(executable) + " " + arguments;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    return CreateProcessA(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startup,
        child) != FALSE;
}

static bool wait_for_file(const fs::path &path, HANDLE process) {
    for (int attempt = 0; attempt != 1'000; ++attempt) {
        std::error_code ignored;
        if (fs::exists(path, ignored)) return true;
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return false;
        Sleep(10);
    }
    return false;
}

static void close_child(PROCESS_INFORMATION *child) {
    if (child->hThread) CloseHandle(child->hThread);
    if (child->hProcess) CloseHandle(child->hProcess);
    child->hThread = nullptr;
    child->hProcess = nullptr;
}

static int child_ownership_conflict(int argc, char **argv) {
    if (argc != 3) return 2;
    const fs::path root = argv[2];
    ManualDoctorClock clock;
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(!opened.ok());
    CHECK(!opened.holder);
    CHECK(opened.failure == DoctorStoreFailure::PermissionDenied);
    CHECK(opened.platform_result_code ==
        static_cast<std::int32_t>(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)));
    CHECK(!opened.recovered_abandoned);
    return 0;
}

static int child_ownership_abandon(int argc, char **argv) {
    if (argc != 4) return 2;
    const fs::path root = argv[2];
    const fs::path ready = argv[3];
    ManualDoctorClock clock;
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(opened.ok());
    CHECK(opened.holder);
    CHECK(write_bytes(ready, "acquired\n"));
    Sleep(INFINITE);
    return 3;
}

static int child_lifetime_producer(int argc, char **argv) {
    if (argc != 3) return 2;
    ProcessProducer producer;
    CHECK(producer.ensure());
    CHECK(write_bytes(argv[2], std::to_string(producer.process_creation())));
    Sleep(INFINITE);
    return 3;
}

class ProducerProcess {
public:
    explicit ProducerProcess(const fs::path &ready) {
        if (!start_helper("--lifetime-producer " + quote(ready.string()), &process_)) return;
        if (!wait_for_file(ready, process_.hProcess)) return;
        creation_ = std::strtoull(read_bytes(ready).c_str(), nullptr, 10);
    }
    ~ProducerProcess() { stop(); close_child(&process_); }
    ProducerProcess(const ProducerProcess &) = delete;
    ProducerProcess &operator=(const ProducerProcess &) = delete;
    DWORD id() const { return process_.dwProcessId; }
    std::uint64_t creation() const { return creation_; }
    bool valid() const { return process_.hProcess && creation_ != 0; }
    bool stop() {
        if (!process_.hProcess) return false;
        if (WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0) return true;
        return TerminateProcess(process_.hProcess, 0) &&
            WaitForSingleObject(process_.hProcess, 5'000) == WAIT_OBJECT_0;
    }
private:
    PROCESS_INFORMATION process_{};
    std::uint64_t creation_ = 0;
};

static bool reserve_unpublished_slot(const ProducerProcess &producer) {
    detail::TargetMappingIdentity identity;
    if (FAILED(detail::process_target_mapping(producer.id(), producer.creation(), true, &identity)))
        return false;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, identity.name.c_str());
    if (!mapping) return false;
    using Queue = ProcessCollectorView::Queue;
    auto *storage = static_cast<Queue::Storage *>(MapViewOfFile(mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, Queue::storage_bytes()));
    if (!storage) { CloseHandle(mapping); return false; }
    // Model a producer terminated after reserving its write index but before
    // publishing the slot sequence. No input payload is fabricated.
    InterlockedIncrement64(&storage->control.write_index);
    const bool pending = Queue(storage).snapshot().queued == 1;
    UnmapViewOfFile(storage);
    CloseHandle(mapping);
    return pending;
}

static int test_health_has_fixed_cadence_without_any_input_source() {
    TempDirectory temporary;
    ManualDoctorClock clock;
    const auto root = temporary.path() / "store";
    auto opened = DoctorRuntimeHolder::open(root, kBuild, clock);
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.heartbeat().ok());
    CHECK(runtime.enable().ok());
    clock.advance(9'999);
    CHECK(runtime.heartbeat().ok());
    std::vector<DoctorEventEnvelope> events;
    CHECK(decode_store_files(root, &events) && events.empty());
    clock.advance(1);
    CHECK(runtime.heartbeat().ok());
    for (int index = 0; index != 100; ++index) CHECK(runtime.heartbeat().ok());
    CHECK(decode_store_files(root, &events) && events.size() == 1);
    clock.advance(10'000);
    CHECK(runtime.heartbeat().ok());
    CHECK(runtime.disable().ok());
    clock.advance(10'000);
    CHECK(runtime.heartbeat().ok());
    events.clear();
    CHECK(decode_store_files(root, &events) && events.size() == 2);
    for (const auto &envelope : events) {
        const auto &event = envelope.event;
        CHECK(event.event_code == DoctorEventCode::Health);
        CHECK(event.scope == DoctorInputScope::Unknown && event.operation_id == 0);
        CHECK(event.callback_source == DoctorCallbackSource::None);
        CHECK(event.phase == DoctorEventPhase::Health && event.duration_us == 0);
        CHECK(!event.gesture_token && !event.pointer_count && !event.pressed);
        CHECK(!event.drop_count && !event.gap_from_sequence);
    }
    CHECK(events[1].event.sequence == events[0].event.sequence + 1);
    return 0;
}

static int test_attachment_capacity_reclaims_exited_producers() {
    TempDirectory temporary;
    ManualDoctorClock clock;
    auto opened = DoctorRuntimeHolder::open(temporary.path() / "store", kBuild,
        clock, DoctorStoreLimits::defaults(), nullptr, 2);
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.enable().ok());
    ProducerProcess first(temporary.path() / "ready-first");
    ProducerProcess second(temporary.path() / "ready-second");
    ProducerProcess third(temporary.path() / "ready-third");
    CHECK(first.valid() && second.valid() && third.valid());
    CHECK(runtime.attach(first.id(), first.creation()).ok());
    CHECK(runtime.attach(second.id(), second.creation()).ok());
    CHECK(runtime.attach(first.id(), first.creation()).ok());
    CHECK(runtime.attachment_count() == 2);
    const auto full = runtime.attach(third.id(), third.creation());
    CHECK(!full.ok());
    CHECK(full.platform_result_code == static_cast<std::int32_t>(
        HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES)));
    CHECK(runtime.status().collecting && runtime.status().producer_limit_reached);
    CHECK(reserve_unpublished_slot(first));
    CHECK(first.stop());
    CHECK(runtime.drain().ok());
    CHECK(runtime.attachment_count() == 1);
    CHECK(!runtime.status().producer_limit_reached);
    CHECK(runtime.attach(third.id(), third.creation()).ok());
    CHECK(third.stop());
    CHECK(runtime.drain().ok());
    for (int index = 0; index != 8; ++index) {
        ProducerProcess transient(temporary.path() / ("ready-churn-" + std::to_string(index)));
        CHECK(transient.valid());
        CHECK(runtime.attach(transient.id(), transient.creation()).ok());
        CHECK(runtime.attachment_count() == 2);
        CHECK(transient.stop());
        CHECK(runtime.drain().ok());
        CHECK(runtime.attachment_count() == 1);
    }
    CHECK(runtime.disable().ok());
    return 0;
}

static int test_writer_ownership_is_global_and_precedes_store_open() {
    TempDirectory temporary;
    const std::wstring user_sid = L"S-1-5-21-runtime-owner";
    const auto first_name = DoctorRuntimeOwnership::mutex_name(user_sid);
    const auto second_name =
        DoctorRuntimeOwnership::mutex_name(user_sid + L"-other");
    CHECK(first_name.rfind(L"Global\\", 0) == 0);
    CHECK(first_name.find(user_sid) != std::wstring::npos);
    CHECK(first_name != second_name);

    const fs::path conflict_root = temporary.path() / "conflict-store";
    ManualDoctorClock seed_clock;
    CHECK(seed_pending_clear(conflict_root, seed_clock) == 0);
    const auto before = snapshot_files(conflict_root);
    CHECK(!before.empty());

    const fs::path owner_root = temporary.path() / "owner-store";
    ManualDoctorClock owner_clock;
    auto owner = DoctorRuntimeHolder::open(
        owner_root, kBuild, owner_clock, DoctorStoreLimits::defaults());
    CHECK(owner.ok());

    PROCESS_INFORMATION child{};
    CHECK(start_helper(
        "--ownership-conflict " + quote(conflict_root.string()), &child));
    const bool exited =
        WaitForSingleObject(child.hProcess, 10'000) == WAIT_OBJECT_0;
    if (!exited) {
        TerminateProcess(child.hProcess, 0xfeedbeefUL);
        WaitForSingleObject(child.hProcess, 10'000);
        close_child(&child);
        CHECK(exited);
    }
    DWORD exit_code = 0xffffffffUL;
    CHECK(GetExitCodeProcess(child.hProcess, &exit_code));
    close_child(&child);
    CHECK(exit_code == 0);
    CHECK(snapshot_files(conflict_root) == before);
    return 0;
}

static int test_abandoned_same_user_mutex_is_recovered() {
    TempDirectory temporary;
    const auto root = temporary.path() / "abandoned-store";
    const auto ready = temporary.path() / "helper-ready.txt";
    PROCESS_INFORMATION child{};
    CHECK(start_helper(
        "--ownership-abandon " + quote(root.string()) + " " +
            quote(ready.string()),
        &child));
    const bool ready_file_created = wait_for_file(ready, child.hProcess);
    if (!ready_file_created) {
        TerminateProcess(child.hProcess, 0xfeedbeefUL);
        WaitForSingleObject(child.hProcess, 10'000);
        close_child(&child);
        CHECK(ready_file_created);
    }
    // Keep the kernel object alive after its owner's last process handle closes.
    std::wstring own_sid;
    CHECK(detail::own_user_sid(&own_sid));
    HANDLE retained = OpenMutexW(READ_CONTROL | SYNCHRONIZE, FALSE,
        DoctorRuntimeOwnership::mutex_name(own_sid).c_str());
    CHECK(retained != nullptr);
    CHECK(TerminateProcess(child.hProcess, 0xfeedbeefUL));
    CHECK(WaitForSingleObject(child.hProcess, 10'000) == WAIT_OBJECT_0);
    close_child(&child);

    ManualDoctorClock clock;
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(opened.ok());
    CHECK(opened.holder);
    CHECK(opened.recovered_abandoned);
    CHECK(!opened.holder->status().enabled);
    CloseHandle(retained);
    return 0;
}

static int test_low_label_writer_object_is_rejected_before_store_open() {
    TempDirectory temporary;
    std::wstring own_sid;
    CHECK(detail::own_user_sid(&own_sid));
    ManualDoctorClock clock;
    auto legitimate = DoctorRuntimeHolder::open(temporary.path() / "original",
        kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(legitimate.ok());
    HANDLE spoofed = OpenMutexW(READ_CONTROL | WRITE_OWNER | SYNCHRONIZE, FALSE,
        DoctorRuntimeOwnership::mutex_name(own_sid).c_str());
    CHECK(spoofed != nullptr);
    legitimate.holder.reset();
    PSECURITY_DESCRIPTOR security = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)",
        SDDL_REVISION_1, &security, nullptr));
    PACL labels = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    CHECK(GetSecurityDescriptorSacl(security, &present, &labels, &defaulted) && present);
    CHECK(SetSecurityInfo(spoofed, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, labels) == ERROR_SUCCESS);
    LocalFree(security);
    // Only the mandatory label changed; the real owner's protected DACL is intact.
    PSECURITY_DESCRIPTOR readback = nullptr;
    CHECK(GetSecurityInfo(spoofed, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, &labels, &readback) == ERROR_SUCCESS);
    CHECK(labels && labels->AceCount == 1);
    void* raw_ace = nullptr;
    CHECK(GetAce(labels, 0, &raw_ace));
    const auto* ace = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(raw_ace);
    CHECK(ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE);
    CHECK(*GetSidSubAuthority(const_cast<DWORD*>(&ace->SidStart), 0) == SECURITY_MANDATORY_LOW_RID);
    LocalFree(readback);
    const auto root = temporary.path() / "untouched-store";
    auto opened = DoctorRuntimeHolder::open(root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(!opened.ok());
    CHECK(!fs::exists(root));
    CloseHandle(spoofed);
    return 0;
}

static int test_off_commands_accept_numbers_without_enabling() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(!runtime.status().enabled);
    CHECK(!runtime.status().collecting);

    constexpr std::int64_t selected = 1'764'057'599'000;
    constexpr std::int64_t before = 12'345;
    constexpr std::int64_t after = 6'789;
    const auto snapshot = runtime.prepare(selected, before, after);
    CHECK(snapshot.ok());
    const auto archive = read_bytes(snapshot.value.archive);
    CHECK(archive.find(
        "\"selectedIssueTimeMs\":" + std::to_string(selected)) !=
        std::string::npos);
    CHECK(archive.find("\"beforeMs\":" + std::to_string(before)) !=
        std::string::npos);
    CHECK(archive.find("\"afterMs\":" + std::to_string(after)) !=
        std::string::npos);
    const auto preview = runtime.preview(snapshot.value);
    CHECK(preview.ok());
    CHECK(preview.value.epoch == snapshot.value.epoch);
    CHECK(!runtime.status().enabled);

    const auto destination = temporary.path() / "off-save.zip";
    CHECK(runtime.save(snapshot.value, destination).ok());
    CHECK(read_bytes(destination) == archive);
    CHECK(!fs::exists(snapshot.value.archive));
    CHECK(!runtime.status().enabled);

    const auto cancelled = runtime.prepare(
        clock.wall_utc_ms(), 1'000, 1'000);
    CHECK(cancelled.ok());
    CHECK(runtime.cancel(cancelled.value).ok());
    CHECK(!fs::exists(cancelled.value.archive));
    CHECK(!runtime.status().enabled);

    CHECK(runtime.prepare(-1, 0, 0).failure ==
        DoctorStoreFailure::InvalidSchema);
    const auto old_epoch = runtime.status().epoch;
    CHECK(runtime.clear().ok());
    CHECK(runtime.status().epoch == old_epoch + 1);
    CHECK(!runtime.status().enabled);
    CHECK(!runtime.status().collecting);
    return 0;
}

static int test_queue_drain_uses_canonical_codec_and_suppresses_private_input() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    ProcessProducer producer;
    CHECK(producer.ensure());
    const auto raw_producer_id = producer.producer_id();

    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.attach(
        GetCurrentProcessId(), producer.process_creation()).ok());
    CHECK(runtime.enable().ok());
    const auto ordinary_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(ordinary_operation != 0);
    CHECK(producer.begin_per_key_operation(ScopeClass::Sensitive) == 0);
    CHECK(producer.begin_per_key_operation(ScopeClass::Unknown) == 0);

    CHECK(producer.try_write(queue_event(
        clock, Event::Focus, ScopeClass::Unknown, 901,
        DoctorCallbackSource::Dispatch)) == WriteResult::Written);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        ordinary_operation, DoctorCallbackSource::KeyAction)) ==
        WriteResult::Written);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Sensitive, 902,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Unknown, 903,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.drain().ok());
    CHECK(runtime.status().collecting);
    opened.holder.reset();

    auto persisted = DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(persisted);
    CHECK(persisted->status().enabled);
    persisted.reset();

    std::vector<DoctorEventEnvelope> events;
    CHECK(decode_store_files(root, &events));
    CHECK(events.size() == 2);
    for (const auto &envelope : events) {
        CHECK(envelope.build.version == kBuild.version);
        CHECK(envelope.build.source_commit == kBuild.source_commit);
        const auto &event = envelope.event;
        CHECK(event.producer_id > 0);
        CHECK(static_cast<std::uint64_t>(event.producer_id) <=
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()));
        CHECK(event.session_id > 0);
    }

    const auto masked_producer =
        static_cast<std::int64_t>(
            raw_producer_id &
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()));
    CHECK(masked_producer > 0);
    CHECK(events[0].event.producer_id == masked_producer);
    CHECK(events[1].event.producer_id == masked_producer);
    DWORD session_id = 0;
    CHECK(ProcessIdToSessionId(GetCurrentProcessId(), &session_id));
    CHECK(events[0].event.session_id ==
        static_cast<std::int64_t>(session_id));
    CHECK(events[0].event.event_code == DoctorEventCode::Focus);
    CHECK(events[0].event.operation_id == 0);
    CHECK(events[0].event.callback_source == DoctorCallbackSource::None);
    CHECK(events[0].event.phase == DoctorEventPhase::Lifecycle);
    CHECK(events[1].event.event_code == DoctorEventCode::KeyDown);
    CHECK(events[1].event.operation_id ==
        static_cast<std::int64_t>(ordinary_operation));
    CHECK(events[1].event.callback_source ==
        DoctorCallbackSource::KeyAction);
    CHECK(events[1].event.scope == DoctorInputScope::Ordinary);
    return 0;
}

static int test_disable_discards_pending_queue_and_preserves_history() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    ProcessProducer producer;
    CHECK(producer.ensure());
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.attach(
        GetCurrentProcessId(), producer.process_creation()).ok());
    CHECK(runtime.enable().ok());

    const auto first_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary, first_operation,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.drain().ok());

    const auto discarded_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary, discarded_operation,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.disable().ok());
    CHECK(!runtime.status().enabled);
    CHECK(!runtime.status().collecting);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        producer.begin_per_key_operation(ScopeClass::Ordinary),
        DoctorCallbackSource::KeyAction)) == WriteResult::Disabled);

    CHECK(runtime.enable().ok());
    CHECK(runtime.drain().ok());
    const auto resumed_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary, resumed_operation,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.drain().ok());
    opened.holder.reset();

    std::vector<DoctorEventEnvelope> events;
    CHECK(decode_store_files(root, &events));
    CHECK(events.size() == 2);
    CHECK(events[0].event.operation_id ==
        static_cast<std::int64_t>(first_operation));
    CHECK(events[1].event.operation_id ==
        static_cast<std::int64_t>(resumed_operation));
    CHECK(std::none_of(events.begin(), events.end(), [&](const auto &envelope) {
        return envelope.event.operation_id ==
            static_cast<std::int64_t>(discarded_operation);
    }));
    return 0;
}

static int test_export_snapshot_is_immutable_and_clear_invalidates_old_handle() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    ProcessProducer producer;
    CHECK(producer.ensure());
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.attach(
        GetCurrentProcessId(), producer.process_creation()).ok());
    CHECK(runtime.enable().ok());

    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        producer.begin_per_key_operation(ScopeClass::Ordinary),
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.drain().ok());
    const auto immutable = runtime.prepare(
        clock.wall_utc_ms(), 1'000, 1'000);
    CHECK(immutable.ok());
    const auto immutable_bytes = read_bytes(immutable.value.archive);
    CHECK(runtime.preview(immutable.value).ok());

    clock.advance(10);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        producer.begin_per_key_operation(ScopeClass::Ordinary),
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.drain().ok());
    CHECK(read_bytes(immutable.value.archive) == immutable_bytes);
    CHECK(runtime.preview(immutable.value).value.record_count == 1);
    CHECK(read_bytes(immutable.value.archive) == immutable_bytes);
    const auto saved = temporary.path() / "selected-output.zip";
    CHECK(runtime.save(immutable.value, saved).ok());
    CHECK(read_bytes(saved) == immutable_bytes);

    const auto stale = runtime.prepare(
        clock.wall_utc_ms(), 10'000, 10'000);
    CHECK(stale.ok());
    const auto stale_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary, stale_operation,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    const auto old_epoch = runtime.status().epoch;
    CHECK(runtime.clear().ok());
    CHECK(runtime.status().enabled);
    CHECK(runtime.status().collecting);
    CHECK(runtime.status().epoch == old_epoch + 1);
    CHECK(!fs::exists(stale.value.archive));

    const auto current_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary, current_operation,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);
    CHECK(runtime.drain().ok());

    const auto guarded_destination = temporary.path() / "must-stay.zip";
    CHECK(write_bytes(guarded_destination, "destination sentinel"));
    CHECK(runtime.preview(stale.value).failure ==
        DoctorStoreFailure::EpochMismatch);
    CHECK(runtime.save(stale.value, guarded_destination).failure ==
        DoctorStoreFailure::EpochMismatch);
    CHECK(read_bytes(guarded_destination) == "destination sentinel");

    opened.holder.reset();
    std::vector<DoctorEventEnvelope> events;
    CHECK(decode_store_files(root, &events));
    CHECK(events.size() == 1);
    CHECK(events[0].event.epoch == old_epoch + 1);
    CHECK(events[0].event.operation_id ==
        static_cast<std::int64_t>(current_operation));
    CHECK(events[0].event.operation_id !=
        static_cast<std::int64_t>(stale_operation));
    return 0;
}

static int test_clear_failure_stays_fail_closed_and_discards_queue() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    ScriptedDoctorIoFaults faults;
    ProcessProducer producer;
    CHECK(producer.ensure());
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults(), &faults);
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.attach(
        GetCurrentProcessId(), producer.process_creation()).ok());
    CHECK(runtime.enable().ok());
    const auto discarded_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary, discarded_operation,
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);

    faults.fail_next(
        DoctorIoOperation::CommitEpoch,
        DoctorStoreFailure::IoError,
        static_cast<std::int32_t>(E_FAIL));
    CHECK(runtime.clear().failure == DoctorStoreFailure::EpochCommitFailed);
    auto failed = runtime.status();
    CHECK(!failed.enabled);
    CHECK(!failed.collecting);
    CHECK(!failed.pending_clear);
    CHECK(failed.failure == DoctorStoreFailure::EpochCommitFailed);
    CHECK(failed.platform_result_code ==
        static_cast<std::int32_t>(E_FAIL));
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        producer.begin_per_key_operation(ScopeClass::Ordinary),
        DoctorCallbackSource::KeyAction)) == WriteResult::Disabled);
    CHECK(runtime.drain().ok());

    CHECK(runtime.clear().ok());
    CHECK(!runtime.status().enabled);
    CHECK(!runtime.status().collecting);
    opened.holder.reset();

    auto reopened = DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(reopened);
    CHECK(!reopened->status().enabled);
    CHECK(reopened->read_records().ok());
    CHECK(reopened->read_records().value.empty());
    return 0;
}

static int test_interrupted_writer_failure_is_visible_and_fail_closed() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    ScriptedDoctorIoFaults faults;
    ProcessProducer producer;
    CHECK(producer.ensure());
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults(), &faults);
    CHECK(opened.ok());
    auto &runtime = *opened.holder;
    CHECK(runtime.attach(
        GetCurrentProcessId(), producer.process_creation()).ok());
    CHECK(runtime.enable().ok());
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        producer.begin_per_key_operation(ScopeClass::Ordinary),
        DoctorCallbackSource::KeyAction)) == WriteResult::Written);

    faults.fail_next(
        DoctorIoOperation::AppendRecord,
        DoctorStoreFailure::DiskFull,
        static_cast<std::int32_t>(
            HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
    const auto drained = runtime.drain();
    CHECK(drained.failure == DoctorStoreFailure::DiskFull);
    const auto status = runtime.status();
    CHECK(status.enabled);
    CHECK(!status.collecting);
    CHECK(status.failure == DoctorStoreFailure::DiskFull);
    CHECK(status.platform_result_code ==
        static_cast<std::int32_t>(
            HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
    CHECK(producer.try_write(queue_event(
        clock, Event::KeyDown, ScopeClass::Ordinary,
        producer.begin_per_key_operation(ScopeClass::Ordinary),
        DoctorCallbackSource::KeyAction)) == WriteResult::Disabled);
    return 0;
}

static int test_all_handler_commands_are_serialized_by_the_holder() {
    TempDirectory temporary;
    const auto root = temporary.path() / "store";
    ManualDoctorClock clock;
    BlockingExportFaults faults;
    auto opened = DoctorRuntimeHolder::open(
        root, kBuild, clock, DoctorStoreLimits::defaults(), &faults);
    CHECK(opened.ok());

    std::atomic<bool> prepare_finished{false};
    std::atomic<int> prepare_result{1};
    std::thread prepare_thread([&] {
        const auto result = opened.holder->prepare(
            clock.wall_utc_ms(), 1'000, 1'000);
        prepare_result.store(result.ok() ? 0 : 1);
        prepare_finished.store(true);
    });
    faults.wait_until_entered();
    const bool prepare_entered = faults.entered();
    if (!prepare_entered) {
        faults.release();
        prepare_thread.join();
        CHECK(prepare_entered);
    }
    const bool prepare_still_blocked = !prepare_finished.load();
    if (!prepare_still_blocked) {
        faults.release();
        prepare_thread.join();
        CHECK(prepare_still_blocked);
    }

    std::atomic<bool> clear_started{false};
    std::atomic<bool> clear_finished{false};
    std::atomic<int> clear_result{1};
    std::thread clear_thread([&] {
        clear_started.store(true);
        const auto result = opened.holder->clear();
        clear_result.store(result.ok() ? 0 : 1);
        clear_finished.store(true);
    });
    while (!clear_started.load()) Sleep(1);
    for (int attempt = 0;
            attempt != 100 && !clear_finished.load();
            ++attempt)
        Sleep(10);
    const bool clear_waited = !clear_finished.load();

    faults.release();
    prepare_thread.join();
    clear_thread.join();
    CHECK(clear_waited);
    CHECK(prepare_finished.load());
    CHECK(clear_finished.load());
    CHECK(prepare_result.load() == 0);
    CHECK(clear_result.load() == 0);
    return 0;
}

struct TestCase {
    const char *name;
    int (*run)();
};

constexpr TestCase kTests[] = {
    {"fixed-health-without-input", test_health_has_fixed_cadence_without_any_input_source},
    {"bounded-producer-lifetime", test_attachment_capacity_reclaims_exited_producers},
    {
        "writer-ownership-global-conflict",
        test_writer_ownership_is_global_and_precedes_store_open,
    },
    {
        "writer-ownership-abandoned",
        test_abandoned_same_user_mutex_is_recovered,
    },
    {
        "off-commands",
        test_off_commands_accept_numbers_without_enabling,
    },
    {
        "writer-ownership-label",
        test_low_label_writer_object_is_rejected_before_store_open,
    },
    {
        "queue-canonical-drain",
        test_queue_drain_uses_canonical_codec_and_suppresses_private_input,
    },
    {
        "disable-discards-queue",
        test_disable_discards_pending_queue_and_preserves_history,
    },
    {
        "export-and-clear-epoch",
        test_export_snapshot_is_immutable_and_clear_invalidates_old_handle,
    },
    {
        "clear-failure",
        test_clear_failure_stays_fail_closed_and_discards_queue,
    },
    {
        "interrupted-status",
        test_interrupted_writer_failure_is_visible_and_fail_closed,
    },
    {
        "handler-serialization",
        test_all_handler_commands_are_serialized_by_the_holder,
    },
};

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--lifetime-producer")
        return child_lifetime_producer(argc, argv);
    if (argc > 1 &&
        std::string(argv[1]) == "--ownership-conflict") {
        return child_ownership_conflict(argc, argv);
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--ownership-abandon") {
        return child_ownership_abandon(argc, argv);
    }
    if (argc > 2) return 2;

    const std::string selected = argc == 2 ? argv[1] : std::string();
    bool found = selected.empty();
    for (const auto &test : kTests) {
        if (!selected.empty() && selected != test.name) continue;
        found = true;
        const int result = test.run();
        if (result != 0) {
            std::cerr << "FAILED: " << test.name << '\n';
            return result;
        }
        std::cout << "PASS: " << test.name << '\n';
    }
    if (!found) {
        std::cerr << "Unknown runtime test case: " << selected << '\n';
        return 2;
    }
    return 0;
}
