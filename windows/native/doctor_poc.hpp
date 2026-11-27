#ifndef BOSHIAMY_WINDOWS_DOCTOR_POC_HPP
#define BOSHIAMY_WINDOWS_DOCTOR_POC_HPP

#include "tsf/doctor_transport.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace boshiamy::doctor::poc {

inline constexpr std::uint64_t kMaxEventFileBytes = 4 * 1024 * 1024;
inline constexpr std::uint64_t kStatusIntervalMs = 250;
inline constexpr std::uint64_t kAttachRefreshIntervalMs = 1000;

inline bool write_all(HANDLE file, const std::string &bytes) noexcept {
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(
            bytes.size() - offset > 0x7fffffff ?
                0x7fffffff : bytes.size() - offset);
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk)
            return false;
        offset += written;
    }
    return true;
}

inline bool atomic_write(const std::filesystem::path &path,
    const std::string &bytes) noexcept {
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    try {
        temporary = path;
        temporary += L".tmp.";
        temporary += std::to_wstring(GetCurrentProcessId());
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    } catch (...) {
        return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(file, bytes) && FlushFileBuffers(file);
    if (!CloseHandle(file)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

inline bool read_file(const std::filesystem::path &path,
    std::string *bytes) {
    if (!bytes) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes->assign(std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    return !input.bad();
}

class CollectorOwnership {
public:
    CollectorOwnership() = default;
    CollectorOwnership(const CollectorOwnership &) = delete;
    CollectorOwnership &operator=(const CollectorOwnership &) = delete;
    ~CollectorOwnership() {
        if (owned_ && mutex_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }

    bool try_acquire() {
        auto security = UserSecurity::create_mutex();
        if (FAILED(security.result)) {
            result_ = security.result;
            return false;
        }
        std::wstring user_sid;
        if (!detail::own_user_sid(&user_sid)) {
            result_ = E_ACCESSDENIED;
            return false;
        }
        const auto name = detail::mutex_name(user_sid);
        if (name.empty()) {
            result_ = E_OUTOFMEMORY;
            return false;
        }
        mutex_ = CreateMutexW(&security.attributes, FALSE,
            name.c_str());
        if (!mutex_) {
            result_ = detail::hresult_from_last_error();
            return false;
        }
        const DWORD wait = WaitForSingleObject(mutex_, 0);
        if (wait == WAIT_OBJECT_0) {
            owned_ = true;
            result_ = S_OK;
            return true;
        }
        if (wait == WAIT_ABANDONED) {
            owned_ = true;
            abandoned_ = true;
            result_ = S_OK;
            return true;
        }
        CloseHandle(mutex_);
        mutex_ = nullptr;
        result_ = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        return false;
    }

    bool recovered_abandoned() const noexcept { return abandoned_; }
    HRESULT result() const noexcept { return result_; }

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
    bool abandoned_ = false;
    HRESULT result_ = E_UNEXPECTED;
};

struct CollectorCounters {
    std::uint64_t events_read = 0;
    std::uint64_t collector_dropped = 0;
    std::uint64_t event_file_bytes = 0;
    std::uint32_t mapping_access_denied = 0;
    std::int32_t event_file_hr = static_cast<std::int32_t>(S_OK);
};

struct Attachment {
    DWORD process_id = 0;
    std::uint64_t process_creation = 0;
    std::unique_ptr<ProcessCollectorView> view;
};

inline std::string event_json(const EventRecord &record,
    std::uint64_t epoch) {
    char line[768]{};
    const int length = std::snprintf(line, sizeof(line),
        "{\"schemaVersion\":%u,\"epoch\":%llu,\"producerId\":%llu,"
        "\"operationId\":%llu,\"sequence\":%llu,\"monotonicNs\":%llu,"
        "\"eventCode\":%u,\"callbackSource\":%u,\"scope\":%u,"
        "\"resultCode\":%d,\"durationUs\":%u,\"flags\":%u,\"reserved\":%u}\n",
        record.schema_version, static_cast<unsigned long long>(epoch),
        static_cast<unsigned long long>(record.producer_id),
        static_cast<unsigned long long>(record.operation_id),
        static_cast<unsigned long long>(record.sequence),
        static_cast<unsigned long long>(record.monotonic_ns),
        record.event_code, record.callback_source, record.scope,
        record.result_code, record.duration_us, record.flags, record.reserved);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(line)
        ? std::string(line, static_cast<std::size_t>(length)) : std::string();
}

inline std::string status_json(
    std::uint64_t epoch,
    const CollectorCounters &counters,
    const std::vector<Attachment> &attachments,
    bool owner_recovered,
    bool alive) {
    std::uint64_t total_queue_dropped = 0;
    for (const auto &attachment : attachments) {
        if (!attachment.view) continue;
        const auto snapshot = attachment.view->snapshot();
        if (snapshot.valid) total_queue_dropped += snapshot.dropped;
    }
    std::ostringstream output;
    output << "{\"schemaVersion\":1,\"phase\":\"p0\",\"epoch\":"
        << epoch << ",\"collectorPid\":" << GetCurrentProcessId()
        << ",\"collectorAlive\":" << (alive ? 1 : 0)
        << ",\"ownerRecovered\":" << (owner_recovered ? 1 : 0)
        << ",\"eventsRead\":" << counters.events_read
        << ",\"collectorDropped\":" << counters.collector_dropped
        << ",\"queueDropped\":" << total_queue_dropped
        << ",\"eventFileBytes\":" << counters.event_file_bytes
        << ",\"eventFileHr\":" << counters.event_file_hr
        << ",\"mappingAccessDenied\":" << counters.mapping_access_denied
        << ",\"producers\":[";
    bool first = true;
    for (const auto &attachment : attachments) {
        if (!attachment.view) continue;
        const auto snapshot = attachment.view->snapshot();
        if (!snapshot.valid) continue;
        if (!first) output << ',';
        first = false;
        output << "{\"pid\":" << snapshot.process_id
            << ",\"processCreation\":" << snapshot.process_creation
            << ",\"producerId\":" << snapshot.producer_id
            << ",\"hostKind\":" << snapshot.host_kind
            << ",\"integrityLevelRid\":" << snapshot.integrity_rid
            << ",\"isAppContainer\":" << snapshot.app_container
            << ",\"tokenQueryHr\":" << snapshot.token_result_hr
            << ",\"scopeQueryHr\":" << snapshot.scope_query_hr
            << ",\"scopeAppPropertyHr\":" << snapshot.scope_app_property_hr
            << ",\"scopeRequestOuterHr\":" << snapshot.scope_request_outer_hr
            << ",\"scopeRequestInnerHr\":" << snapshot.scope_request_inner_hr
            << ",\"scopeRangeHr\":" << snapshot.scope_range_hr
            << ",\"scopePropertyValueHr\":" << snapshot.scope_property_value_hr
            << ",\"scopeUnpackHr\":" << snapshot.scope_unpack_hr
            << ",\"scopeVariantType\":" << snapshot.scope_variant_type
            << ",\"scopeClass\":" << snapshot.scope_class
            << ",\"scopeTokenCount\":" << snapshot.scope_token_count
            << ",\"scopeTruncated\":" << snapshot.scope_truncated
            << ",\"scopeTokens\":[";
        for (std::size_t i = 0; i < snapshot.scope_token_count; ++i) {
            if (i) output << ',';
            output << snapshot.scope_raw_tokens[i];
        }
        output << "],\"targetValidationHr\":"
            << static_cast<std::int32_t>(attachment.view->target_hr())
            << ",\"queueOpenHr\":"
            << static_cast<std::int32_t>(attachment.view->mapping_open_hr())
            << ",\"ownerValidationHr\":"
            << static_cast<std::int32_t>(attachment.view->owner_hr())
            << ",\"eventsWritten\":"
            << snapshot.events_written
            << ",\"queueDropped\":" << snapshot.dropped
            << ",\"queued\":" << snapshot.queued
            << ",\"heartbeatAgeMs\":";
        const auto now = GetTickCount64();
        output << (now >= snapshot.heartbeat_ms ? now - snapshot.heartbeat_ms : 0)
            << '}';
    }
    output << "]}";
    return output.str();
}

class Collector {
public:
    explicit Collector(std::filesystem::path output)
        : output_(std::move(output)) {}

    int run() noexcept {
        try {
            if (!output_.is_absolute()) return 2;
            if (!CreateDirectoryW(output_.c_str(), nullptr)) {
                if (GetLastError() != ERROR_ALREADY_EXISTS) return 2;
                return 3;
            }
            if (!ownership_.try_acquire()) {
                if (ownership_.result() == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) return 4;
                return 5;
            }

            std::uint64_t creation = 0;
            if (!detail::process_creation_stamp(&creation))
                creation = static_cast<std::uint64_t>(GetCurrentProcessId()) << 32;
            epoch_ = detail::random_producer_id(creation, GetCurrentProcessId()) ^
                GetTickCount64();
            if (epoch_ == 0) epoch_ = 1;

            events_ = CreateFileW((output_ / L"events.jsonl").c_str(),
                GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (events_ == INVALID_HANDLE_VALUE) return 3;

            std::uint64_t last_status = 0;
            std::uint64_t last_refresh = 0;
            while (GetFileAttributesW((output_ / L"stop.request").c_str()) ==
                INVALID_FILE_ATTRIBUTES) {
                const auto now = GetTickCount64();
                if (now - last_refresh >= kAttachRefreshIntervalMs) {
                    refresh_attachments();
                    last_refresh = now;
                }
                drain();
                if (now - last_status >= kStatusIntervalMs) {
                    write_status(true);
                    last_status = now;
                }
                Sleep(25);
            }
            for (const auto &attachment : attachments_)
                if (attachment.view) attachment.view->disable();
            drain();
            if (events_ != INVALID_HANDLE_VALUE) {
                if (!FlushFileBuffers(events_))
                    counters_.event_file_hr =
                        static_cast<std::int32_t>(detail::hresult_from_last_error());
                CloseHandle(events_);
                events_ = INVALID_HANDLE_VALUE;
            }
            return write_status(false) ? 0 : 6;
        } catch (...) {
            close();
            return 7;
        }
    }

private:
    void refresh_attachments() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        std::set<std::pair<DWORD, std::uint64_t>> current;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                std::uint64_t creation = 0;
                if (!detail::process_creation_stamp(entry.th32ProcessID, &creation))
                    continue;
                current.emplace(entry.th32ProcessID, creation);
                const bool known = std::any_of(attachments_.begin(), attachments_.end(),
                    [&](const Attachment &attachment) {
                        return attachment.process_id == entry.th32ProcessID &&
                            attachment.process_creation == creation;
                    });
                if (known) continue;

                auto view = std::make_unique<ProcessCollectorView>(
                    entry.th32ProcessID, creation);
                if (!view->open()) {
                    if (view->last_hr() == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
                        ++counters_.mapping_access_denied;
                    continue;
                }
                if (!view->initialize(epoch_, true)) continue;
                attachments_.push_back({entry.th32ProcessID, creation,
                    std::move(view)});
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);

        attachments_.erase(
            std::remove_if(attachments_.begin(), attachments_.end(),
                [&](const Attachment &attachment) {
                    const bool current_attachment = current.count(
                        {attachment.process_id, attachment.process_creation}) != 0;
                    if (!current_attachment && attachment.view)
                        attachment.view->disable();
                    return !current_attachment;
                }),
            attachments_.end());
    }

    void drain() {
        for (const auto &attachment : attachments_) {
            if (!attachment.view) continue;
            attachment.view->refresh_heartbeat();
            for (std::size_t limit = 0; limit < kQueueCapacity * 2; ++limit) {
                EventRecord record{};
                std::uint64_t event_epoch = 0;
                const auto result = attachment.view->read(&record, &event_epoch);
                if (result != ReadResult::Read) break;
                ++counters_.events_read;
                const auto encoded = event_json(record, event_epoch);
                if (encoded.empty()) {
                    ++counters_.collector_dropped;
                    continue;
                }
                if (counters_.event_file_bytes + encoded.size() > kMaxEventFileBytes) {
                    ++counters_.collector_dropped;
                    continue;
                }
                if (!write_all(events_, encoded)) {
                    counters_.event_file_hr =
                        static_cast<std::int32_t>(detail::hresult_from_last_error());
                    ++counters_.collector_dropped;
                    continue;
                }
                counters_.event_file_bytes += encoded.size();
            }
        }
    }

    bool write_status(bool alive) {
        const auto json = status_json(epoch_, counters_, attachments_,
            ownership_.recovered_abandoned(), alive);
        return atomic_write(output_ / L"status.json", json);
    }

    void close() noexcept {
        for (const auto &attachment : attachments_)
            if (attachment.view) attachment.view->disable();
        if (events_ != INVALID_HANDLE_VALUE) {
            CloseHandle(events_);
            events_ = INVALID_HANDLE_VALUE;
        }
    }

    std::filesystem::path output_;
    CollectorOwnership ownership_;
    std::uint64_t epoch_ = 0;
    HANDLE events_ = INVALID_HANDLE_VALUE;
    std::vector<Attachment> attachments_;
    CollectorCounters counters_;
};

inline int snapshot(const std::filesystem::path &directory) {
    if (!directory.is_absolute()) return 2;
    std::string bytes;
    if (!read_file(directory / L"status.json", &bytes)) return 3;
    return atomic_write(directory / L"snapshot.json", bytes) ? 0 : 4;
}

inline int stop(const std::filesystem::path &directory) noexcept {
    if (!directory.is_absolute()) return 2;
    HANDLE file = CreateFileW((directory / L"stop.request").c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    CloseHandle(file);
    return 0;
}

inline int access_check(DWORD process_id, std::uint64_t process_creation,
    const std::filesystem::path &output) noexcept {
    try {
        if (!output.is_absolute()) return 2;
        ProcessCollectorView view(process_id, process_creation, false);
        const bool opened = view.open();
        const auto open_hr = view.last_hr();
        const auto state = view.snapshot();
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"phase\":\"acl-probe\",\"pid\":"
            << process_id << ",\"processCreation\":" << process_creation
            << ",\"openHr\":" << static_cast<std::int32_t>(open_hr)
            << ",\"targetValidationHr\":"
            << static_cast<std::int32_t>(view.target_hr())
            << ",\"mappingOpenHr\":"
            << static_cast<std::int32_t>(view.mapping_open_hr())
            << ",\"ownerValidationHr\":"
            << static_cast<std::int32_t>(view.owner_hr())
            << ",\"opened\":" << (opened ? 1 : 0)
            << ",\"producerId\":"
            << (state.valid ? state.producer_id : 0)
            << '}';
        return atomic_write(output, json.str()) ? 0 : 3;
    } catch (...) {
        return 4;
    }
}

enum class CommandKind { None, Collect, Snapshot, Stop, AccessCheck };

struct Command {
    CommandKind kind = CommandKind::None;
    std::filesystem::path output;
    DWORD process_id = 0;
    std::uint64_t process_creation = 0;
};

inline bool is_doctor_argument(const std::wstring &argument) {
    return argument == L"--doctor-collector" ||
        argument == L"--doctor-once" || argument == L"--doctor-probe";
}

inline Command parse(int argc, wchar_t **argv) {
    if (argc < 2 || !is_doctor_argument(argv[1])) return {};
    std::wstring mode = argv[1];
    int operation_index = 2;
    Command command;
    if (mode == L"--doctor-collector") {
        command.kind = CommandKind::Collect;
    } else {
        if (argc < 3) return {};
        const std::wstring operation = argv[operation_index++];
        if (operation == L"collect")
            command.kind = CommandKind::Collect;
        else if (operation == L"snapshot" || operation == L"status")
            command.kind = CommandKind::Snapshot;
        else if (operation == L"stop")
            command.kind = CommandKind::Stop;
        else if (operation == L"access-check")
            command.kind = CommandKind::AccessCheck;
        else
            return {};
    }
    for (int i = operation_index; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if ((argument == L"--output" || argument == L"--output-dir") &&
            i + 1 < argc) {
            command.output = argv[++i];
        } else if (argument == L"--pid" && i + 1 < argc) {
            command.process_id = static_cast<DWORD>(std::stoul(argv[++i], nullptr, 0));
        } else if (argument == L"--process-creation" && i + 1 < argc) {
            command.process_creation =
                std::stoull(argv[++i], nullptr, 0);
        } else {
            return {};
        }
    }
    if (command.output.empty() ||
        (command.kind == CommandKind::AccessCheck &&
            (!command.process_id || !command.process_creation)))
        command.kind = CommandKind::None;
    return command;
}

inline int run(int argc, wchar_t **argv) noexcept {
    try {
        const auto command = parse(argc, argv);
        switch (command.kind) {
        case CommandKind::Collect:
            return Collector(command.output).run();
        case CommandKind::Snapshot:
            return snapshot(command.output);
        case CommandKind::Stop:
            return stop(command.output);
        case CommandKind::AccessCheck:
            return access_check(command.process_id,
                command.process_creation, command.output);
        default:
            return 2;
        }
    } catch (...) {
        return 7;
    }
}

} // namespace boshiamy::doctor::poc

#endif
