#ifndef BOSHIAMY_WINDOWS_DOCTOR_HOST_HPP
#define BOSHIAMY_WINDOWS_DOCTOR_HOST_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <tlhelp32.h>

#include "build_info.hpp"
#include "doctor_paths.hpp"
#include "doctor_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace boshiamy::doctor {

inline constexpr std::uint32_t kDoctorHostProtocolMagic = 0x4c4b4448U;
inline constexpr std::uint16_t kDoctorHostProtocolVersion = 1;
inline constexpr std::size_t kDoctorHostMaximumPathCodeUnits = 1024;
inline constexpr const wchar_t kDoctorHostRunValueName[] =
    L"LiuKaiDoctorCollector";
inline constexpr const wchar_t kDoctorHostProductRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t kDoctorMaintenanceStopUpgradeArgument[] =
    L"--doctor-maintenance-stop-upgrade";
inline constexpr const wchar_t kDoctorMaintenanceStopRemovalArgument[] =
    L"--doctor-maintenance-stop-removal";
inline constexpr const wchar_t kDoctorMaintenanceResumeArgument[] =
    L"--doctor-maintenance-resume";

enum class DoctorHostMaintenanceAction : std::uint32_t {
    StopForUpgrade = 1,
    StopForRemoval = 2,
    ResumeAfterUpgrade = 3,
};

enum class DoctorHostCommand : std::uint32_t {
    Status = 1,
    Enable = 2,
    Disable = 3,
    Mark = 4,
    Prepare = 5,
    Preview = 6,
    Save = 7,
    Cancel = 8,
    Clear = 9,
    StopForUpgrade = 10,
    StopForRemoval = 11,
    ResumeForUpgrade = 12,
};

enum class DoctorHostClientFailure : std::uint32_t {
    None = 0,
    EndpointUnavailable = 1,
    AccessDenied = 2,
    EndpointUntrusted = 3,
    OwnerConflict = 4,
    Timeout = 5,
    Disconnect = 6,
    ProtocolRejected = 7,
    InvalidRequest = 8,
    ProcessStartFailed = 9,
};

enum class DoctorHostProtocolFailure : std::uint32_t {
    None = 0,
    Magic = 1,
    Version = 2,
    Size = 3,
    Reserved = 4,
    Command = 5,
    Snapshot = 6,
    Destination = 7,
    Reply = 8,
};

enum class DoctorHostExitCode : int {
    Success = 0,
    Error = 1,
    OwnerConflict = 2,
};

struct DoctorHostMark {
    std::int64_t epoch = 0;
    std::int64_t action_time_ms = 0;
    std::int64_t selected_issue_time_ms = 0;
    std::int64_t before_ms = 0;
    std::int64_t after_ms = 0;
};

struct DoctorHostSnapshot {
    std::int64_t epoch = 0;
    std::uint64_t handle = 0;
};

struct DoctorHostRequest {
    DoctorHostCommand command = DoctorHostCommand::Status;
    DoctorHostMark mark{};
    DoctorHostSnapshot snapshot{};
    std::wstring destination;
};

struct DoctorHostReply {
    DoctorHostProtocolFailure protocol_failure =
        DoctorHostProtocolFailure::None;
    DoctorStoreFailure failure = DoctorStoreFailure::None;
    std::int32_t platform_result_code = 0;
    DoctorStoreState status{};
    DoctorHostMark mark{};
    DoctorHostSnapshot snapshot{};
    DoctorExportPreview preview{};
};

struct DoctorHostClientResult {
    DoctorHostClientFailure failure = DoctorHostClientFailure::None;
    std::int32_t platform_result_code = 0;
    DoctorHostReply reply{};
    DWORD server_pid = 0;
};

class DoctorHostEndpoint {
public:
    static std::wstring pipe_name(const std::wstring &user_sid) {
        if (user_sid.empty()) return {};
        return L"\\\\.\\pipe\\LiuKaiDoctorHost-" + user_sid;
    }
};

#pragma pack(push, 1)
struct DoctorHostWireHeader {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t total_bytes = 0;
};

struct DoctorHostWireMark {
    std::int64_t epoch = 0;
    std::int64_t action_time_ms = 0;
    std::int64_t selected_issue_time_ms = 0;
    std::int64_t before_ms = 0;
    std::int64_t after_ms = 0;
};

struct DoctorHostWireSnapshot {
    std::int64_t epoch = 0;
    std::uint64_t handle = 0;
};

struct DoctorHostWireState {
    std::uint8_t enabled = 0;
    std::uint8_t collecting = 0;
    std::uint8_t pending_clear = 0;
    std::uint8_t reserved = 0;
    std::int64_t epoch = 0;
    std::uint32_t failure = 0;
    std::int32_t platform_result_code = 0;
    std::uint64_t raw_bytes = 0;
    std::uint64_t temp_bytes = 0;
    std::uint64_t sequence_gap = 0;
    std::uint64_t ignored_lines = 0;
    std::uint8_t producer_limit_reached = 0;
    std::uint8_t has_retained_range = 0;
    std::int64_t oldest_retained_ms = 0;
    std::int64_t newest_retained_ms = 0;
};

struct DoctorHostWirePreview {
    std::int64_t epoch = 0;
    std::uint64_t record_count = 0;
    std::uint64_t sequence_gap = 0;
    std::int64_t missing_before_ms = 0;
    std::int64_t missing_after_ms = 0;
};

struct DoctorHostWireRequest {
    DoctorHostWireHeader header{};
    std::uint32_t command = 0;
    std::uint32_t reserved = 0;
    DoctorHostWireMark mark{};
    DoctorHostWireSnapshot snapshot{};
    std::uint32_t destination_code_units = 0;
    std::uint32_t destination_reserved = 0;
    wchar_t destination[kDoctorHostMaximumPathCodeUnits]{};
};

struct DoctorHostWireReply {
    DoctorHostWireHeader header{};
    DoctorHostProtocolFailure protocol_failure =
        DoctorHostProtocolFailure::None;
    std::uint32_t store_failure = 0;
    std::int32_t platform_result_code = 0;
    DoctorHostWireState status{};
    DoctorHostWireMark mark{};
    DoctorHostWireSnapshot snapshot{};
    DoctorHostWirePreview preview{};
};
#pragma pack(pop)

static_assert(sizeof(wchar_t) == 2, "Doctor host protocol requires UTF-16");
static_assert(
    sizeof(DoctorHostWireReply) < 1024,
    "Doctor host reply must stay bounded");

#ifdef BOSHIAMY_DOCTOR_HOST_TESTING
struct DoctorHostTestProducer {
    DWORD process_id = 0;
    std::uint64_t process_creation = 0;
};
#endif

namespace doctor_host_detail {

inline constexpr DWORD kPipeIoTimeoutMs = 4'000;
inline constexpr DWORD kIoPollMs = 25;
inline constexpr DWORD kAttachRefreshMs = 1'000;
inline constexpr DWORD kMaintenanceTimeoutMs = 8'000;

inline constexpr const wchar_t *kTestRootVariable =
    L"LIUKAI_DOCTOR_HOST_TEST_ROOT";
inline constexpr const wchar_t *kTestVersionVariable =
    L"LIUKAI_DOCTOR_HOST_TEST_VERSION";
inline constexpr const wchar_t *kTestCommitVariable =
    L"LIUKAI_DOCTOR_HOST_TEST_COMMIT";
inline constexpr const wchar_t *kTestExecutableVariable =
    L"LIUKAI_DOCTOR_HOST_TEST_EXE";
inline constexpr const wchar_t *kTestRunKeyVariable =
    L"LIUKAI_DOCTOR_HOST_TEST_RUN_KEY";
inline constexpr const wchar_t *kTestIdleVariable =
    L"LIUKAI_DOCTOR_HOST_TEST_IDLE_MS";
inline constexpr const wchar_t *kTestEnvironmentPrefix =
    L"LIUKAI_DOCTOR_HOST_TEST_";

struct ProducerIdentity {
    DWORD process_id = 0;
    std::uint64_t process_creation = 0;
};

struct HostOptions {
    std::filesystem::path root;
    DoctorBuildMetadata build;
    std::filesystem::path settings_exe_path;
    std::wstring run_key_path;
    std::uint32_t idle_timeout_ms = 60'000;
    std::vector<ProducerIdentity> producers;
    bool test_environment = false;
};

class SystemDoctorClock final : public DoctorClock {
public:
    std::int64_t wall_utc_ms() const override {
        FILETIME file_time{};
        GetSystemTimeAsFileTime(&file_time);
        ULARGE_INTEGER value{};
        value.LowPart = file_time.dwLowDateTime;
        value.HighPart = file_time.dwHighDateTime;
        constexpr std::uint64_t kUnixEpochTicks =
            116444736000000000ULL;
        return value.QuadPart > kUnixEpochTicks
            ? static_cast<std::int64_t>(
                (value.QuadPart - kUnixEpochTicks) / 10000ULL)
            : 0;
    }

    std::int64_t monotonic_ns() const override {
        LARGE_INTEGER frequency{};
        LARGE_INTEGER counter{};
        if (!QueryPerformanceFrequency(&frequency) ||
            !QueryPerformanceCounter(&counter) ||
            frequency.QuadPart <= 0) {
            return static_cast<std::int64_t>(GetTickCount64()) *
                1'000'000;
        }
        const auto whole =
            static_cast<std::uint64_t>(counter.QuadPart) /
            static_cast<std::uint64_t>(frequency.QuadPart);
        const auto remainder =
            static_cast<std::uint64_t>(counter.QuadPart) %
            static_cast<std::uint64_t>(frequency.QuadPart);
        return static_cast<std::int64_t>(
            whole * 1'000'000'000ULL +
            (remainder * 1'000'000'000ULL) /
                static_cast<std::uint64_t>(frequency.QuadPart));
    }
};

inline HRESULT hresult_from_last_error() noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS
        ? S_OK
        : HRESULT_FROM_WIN32(error);
}

inline std::int32_t platform_hresult(HRESULT value) noexcept {
    return static_cast<std::int32_t>(value);
}

inline DoctorStoreActionResult action_result(
    DoctorStoreFailure failure,
    std::int32_t platform_result_code = 0) {
    return DoctorStoreActionResult{failure, platform_result_code};
}

inline DoctorStoreActionResult registry_result(DWORD error) {
    const HRESULT value = HRESULT_FROM_WIN32(error);
    return action_result(
        error == ERROR_ACCESS_DENIED ||
                error == ERROR_PRIVILEGE_NOT_HELD
            ? DoctorStoreFailure::PermissionDenied
            : DoctorStoreFailure::IoError,
        static_cast<std::int32_t>(value));
}

inline bool command_valid(DoctorHostCommand command) {
    return static_cast<std::uint32_t>(command) >=
            static_cast<std::uint32_t>(DoctorHostCommand::Status) &&
        static_cast<std::uint32_t>(command) <=
            static_cast<std::uint32_t>(
                DoctorHostCommand::ResumeForUpgrade);
}

inline bool store_failure_valid(std::uint32_t value) {
    return value <= static_cast<std::uint32_t>(
        DoctorStoreFailure::RecordTooLarge);
}

inline std::filesystem::path canonical_path(
    const std::filesystem::path &path) {
    if (path.empty()) return {};
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error || canonical.empty() || !canonical.is_absolute()) return {};
    return canonical;
}

inline bool same_path(
    const std::filesystem::path &left,
    const std::filesystem::path &right) {
    const auto left_name = canonical_path(left).wstring();
    const auto right_name = canonical_path(right).wstring();
    return !left_name.empty() &&
        left_name.size() == right_name.size() &&
        CompareStringOrdinal(
            left_name.c_str(),
            static_cast<int>(left_name.size()),
            right_name.c_str(),
            static_cast<int>(right_name.size()),
            TRUE) == CSTR_EQUAL;
}

inline std::filesystem::path current_executable_path() {
    std::vector<wchar_t> buffer(4096);
    for (;;) {
        const DWORD read = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (!read) return {};
        if (read < buffer.size() - 1) {
            return canonical_path(std::filesystem::path(
                std::wstring(buffer.data(), read)));
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

inline std::filesystem::path process_executable_path(HANDLE process) {
    if (!process) return {};
    std::vector<wchar_t> buffer(4096);
    for (;;) {
        DWORD length = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(
                process, 0, buffer.data(), &length)) {
            return canonical_path(std::filesystem::path(
                std::wstring(buffer.data(), length)));
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

inline bool own_sid_and_session(
    std::vector<unsigned char> *sid, DWORD *session_id) {
    if (!sid || !session_id) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const HRESULT sid_result = detail::token_sid_copy(token, sid);
    const HRESULT session_result = SUCCEEDED(sid_result)
        ? detail::token_session_id(token, session_id)
        : sid_result;
    CloseHandle(token);
    return SUCCEEDED(session_result);
}

class ProcessIdentity {
public:
    ProcessIdentity() = default;
    ProcessIdentity(const ProcessIdentity &) = delete;
    ProcessIdentity &operator=(const ProcessIdentity &) = delete;

    ProcessIdentity(ProcessIdentity &&other) noexcept {
        move_from(std::move(other));
    }

    ProcessIdentity &operator=(ProcessIdentity &&other) noexcept {
        if (this != &other) {
            close();
            move_from(std::move(other));
        }
        return *this;
    }

    ~ProcessIdentity() { close(); }

    bool open(
        DWORD process_id,
        const std::filesystem::path &expected_executable,
        bool require_normal_medium = false) {
        close();
        pid_ = process_id;
        process_ = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE,
            process_id);
        if (!process_) return false;

        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(
                process_, &created, &exited, &kernel, &user)) {
            close();
            return false;
        }
        ULARGE_INTEGER creation{};
        creation.LowPart = created.dwLowDateTime;
        creation.HighPart = created.dwHighDateTime;
        creation_ = creation.QuadPart;
        if (!creation_ || !same_path(
                process_executable_path(process_),
                expected_executable)) {
            close();
            return false;
        }

        HANDLE token = nullptr;
        if (!OpenProcessToken(process_, TOKEN_QUERY, &token)) {
            close();
            return false;
        }
        std::vector<unsigned char> target_sid;
        const HRESULT target_sid_result =
            detail::token_sid_copy(token, &target_sid);
        DWORD target_session = 0;
        const HRESULT session_result = SUCCEEDED(target_sid_result)
            ? detail::token_session_id(token, &target_session)
            : target_sid_result;
        const auto identity = detail::token_identity(token);
        TOKEN_ELEVATION elevation{};
        DWORD copied = 0;
        const BOOL elevation_read = GetTokenInformation(
            token,
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &copied);
        CloseHandle(token);

        std::vector<unsigned char> own_sid;
        DWORD own_session = 0;
        if (FAILED(session_result) ||
            identity.result_hr != static_cast<std::int32_t>(S_OK) ||
            identity.app_container != 0 ||
            identity.integrity_rid <
                static_cast<std::uint32_t>(
                    SECURITY_MANDATORY_MEDIUM_RID) ||
            !elevation_read ||
            (require_normal_medium &&
                (identity.integrity_rid !=
                    static_cast<std::uint32_t>(
                        SECURITY_MANDATORY_MEDIUM_RID) ||
                elevation.TokenIsElevated != 0)) ||
            !own_sid_and_session(&own_sid, &own_session) ||
            own_sid.size() != target_sid.size() ||
            !EqualSid(own_sid.data(), target_sid.data()) ||
            own_session != target_session) {
            close();
            return false;
        }
        integrity_rid_ = identity.integrity_rid;
        elevated_ = elevation.TokenIsElevated != 0;
        return true;
    }

    HANDLE process() const noexcept { return process_; }
    DWORD process_id() const noexcept { return pid_; }
    std::uint64_t creation() const noexcept { return creation_; }
    bool normal_medium() const noexcept {
        return integrity_rid_ ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID) &&
            !elevated_;
    }

    bool take_process(HANDLE *output) {
        if (!output || !process_) return false;
        *output = process_;
        process_ = nullptr;
        pid_ = 0;
        creation_ = 0;
        integrity_rid_ = 0;
        elevated_ = true;
        return true;
    }

private:
    void close() {
        if (process_) CloseHandle(process_);
        process_ = nullptr;
        pid_ = 0;
        creation_ = 0;
        integrity_rid_ = 0;
        elevated_ = true;
    }

    void move_from(ProcessIdentity &&other) {
        process_ = other.process_;
        pid_ = other.pid_;
        creation_ = other.creation_;
        integrity_rid_ = other.integrity_rid_;
        elevated_ = other.elevated_;
        other.process_ = nullptr;
        other.pid_ = 0;
        other.creation_ = 0;
        other.integrity_rid_ = 0;
        other.elevated_ = true;
    }

    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    std::uint64_t creation_ = 0;
    std::uint32_t integrity_rid_ = 0;
    bool elevated_ = true;
};

struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    PipeSecurity() = default;
    PipeSecurity(const PipeSecurity &) = delete;
    PipeSecurity &operator=(const PipeSecurity &) = delete;

    ~PipeSecurity() {
        if (descriptor) LocalFree(descriptor);
    }

    bool create(const std::wstring &user_sid) {
        if (descriptor || user_sid.empty()) return false;
        const std::wstring sddl =
            L"D:P(A;;0x12019f;;;" + user_sid +
            L")(A;;0x12019f;;;SY)S:(ML;;NW;;;ME)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(),
                SDDL_REVISION_1,
                &descriptor,
                nullptr)) {
            return false;
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};

inline bool explicit_medium_label(HANDLE object) {
    if (!object) return false;
    DWORD needed = 0;
    if (!GetKernelObjectSecurity(
            object,
            LABEL_SECURITY_INFORMATION,
            nullptr,
            0,
            &needed)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0)
            return false;
    }
    std::vector<unsigned char> storage(needed);
    if (!GetKernelObjectSecurity(
            object,
            LABEL_SECURITY_INFORMATION,
            storage.data(),
            static_cast<DWORD>(storage.size()),
            &needed)) {
        return false;
    }
    PACL labels = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    if (!GetSecurityDescriptorSacl(
            reinterpret_cast<PSECURITY_DESCRIPTOR>(storage.data()),
            &present,
            &labels,
            &defaulted) ||
        !present || !labels || labels->AceCount != 1) {
        return false;
    }
    void *raw = nullptr;
    if (!GetAce(labels, 0, &raw) || !raw) return false;
    const auto *header = static_cast<ACE_HEADER *>(raw);
    if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE ||
        header->AceSize <
            sizeof(SYSTEM_MANDATORY_LABEL_ACE)) {
        return false;
    }
    const auto *label =
        static_cast<SYSTEM_MANDATORY_LABEL_ACE *>(raw);
    PSID sid = const_cast<DWORD *>(&label->SidStart);
    const SID_IDENTIFIER_AUTHORITY authority =
        SECURITY_MANDATORY_LABEL_AUTHORITY;
    return IsValidSid(sid) &&
        *GetSidSubAuthorityCount(sid) == 1 &&
        std::memcmp(
            GetSidIdentifierAuthority(sid),
            &authority,
            sizeof(authority)) == 0 &&
        *GetSidSubAuthority(sid, 0) ==
            SECURITY_MANDATORY_MEDIUM_RID &&
        (label->Mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) != 0;
}

inline bool pipe_security_valid(HANDLE pipe) {
    std::wstring user_sid;
    PipeSecurity expected;
    return detail::own_user_sid(&user_sid) &&
        expected.create(user_sid) &&
        doctor_runtime_detail::last_dacl_matches(
            pipe, expected.descriptor) &&
        explicit_medium_label(pipe);
}

inline DoctorHostMark host_mark(const DoctorIssueMark &mark) {
    return DoctorHostMark{
        mark.epoch,
        mark.action_time_ms,
        mark.selected_issue_time_ms,
        mark.window.before_ms,
        mark.window.after_ms,
    };
}

inline DoctorIssueMark store_mark(const DoctorHostMark &mark) {
    return DoctorIssueMark{
        mark.epoch,
        mark.action_time_ms,
        mark.selected_issue_time_ms,
        DoctorExportWindow{mark.before_ms, mark.after_ms},
    };
}

inline DoctorHostWireMark wire_mark(const DoctorHostMark &mark) {
    return DoctorHostWireMark{
        mark.epoch,
        mark.action_time_ms,
        mark.selected_issue_time_ms,
        mark.before_ms,
        mark.after_ms,
    };
}

inline DoctorHostMark host_mark(const DoctorHostWireMark &mark) {
    return DoctorHostMark{
        mark.epoch,
        mark.action_time_ms,
        mark.selected_issue_time_ms,
        mark.before_ms,
        mark.after_ms,
    };
}

inline bool mark_wire_empty(const DoctorHostWireMark &mark) {
    return mark.epoch == 0 &&
        mark.action_time_ms == 0 &&
        mark.selected_issue_time_ms == 0 &&
        mark.before_ms == 0 &&
        mark.after_ms == 0;
}

inline bool snapshot_wire_empty(
    const DoctorHostWireSnapshot &snapshot) {
    return snapshot.epoch == 0 && snapshot.handle == 0;
}

inline bool command_uses_mark(DoctorHostCommand command) {
    return command == DoctorHostCommand::Mark ||
        command == DoctorHostCommand::Prepare;
}

inline bool command_uses_snapshot(DoctorHostCommand command) {
    return command == DoctorHostCommand::Preview ||
        command == DoctorHostCommand::Save ||
        command == DoctorHostCommand::Cancel;
}

inline bool bytes_zero(const void *value, std::size_t bytes) {
    if (!value || bytes == 0) return true;
    const auto *cursor =
        static_cast<const unsigned char *>(value);
    for (std::size_t index = 0; index < bytes; ++index)
        if (cursor[index] != 0) return false;
    return true;
}

inline bool encode_request_wire(
    const DoctorHostRequest &request,
    DoctorHostWireRequest *wire) {
    if (!wire || !command_valid(request.command) ||
        request.destination.size() >
            kDoctorHostMaximumPathCodeUnits ||
        (request.command == DoctorHostCommand::Save &&
            request.destination.empty()) ||
        (request.command != DoctorHostCommand::Save &&
            !request.destination.empty()) ||
        request.snapshot.epoch < 0 ||
        request.destination.find(L'\0') != std::wstring::npos ||
        (!command_uses_mark(request.command) &&
            !mark_wire_empty(wire_mark(request.mark))) ||
        (!command_uses_snapshot(request.command) &&
            !snapshot_wire_empty(DoctorHostWireSnapshot{
                request.snapshot.epoch,
                request.snapshot.handle})) ||
        (command_uses_snapshot(request.command) &&
            (request.snapshot.epoch < 0 ||
                request.snapshot.handle == 0))) {
        return false;
    }
    std::memset(wire, 0, sizeof(*wire));
    wire->header.magic = kDoctorHostProtocolMagic;
    wire->header.version = kDoctorHostProtocolVersion;
    wire->header.total_bytes =
        static_cast<std::uint32_t>(sizeof(*wire));
    wire->command = static_cast<std::uint32_t>(request.command);
    wire->mark = wire_mark(request.mark);
    wire->snapshot = DoctorHostWireSnapshot{
        request.snapshot.epoch, request.snapshot.handle};
    wire->destination_code_units =
        static_cast<std::uint32_t>(request.destination.size());
    if (!request.destination.empty()) {
        std::memcpy(
            wire->destination,
            request.destination.data(),
            request.destination.size() * sizeof(wchar_t));
    }
    return true;
}

inline DoctorHostProtocolFailure decode_request_wire(
    const unsigned char *bytes,
    std::size_t size,
    DoctorHostRequest *request) {
    if (!request || !bytes) return DoctorHostProtocolFailure::Size;
    if (size != sizeof(DoctorHostWireRequest))
        return DoctorHostProtocolFailure::Size;
    DoctorHostWireRequest wire{};
    std::memcpy(&wire, bytes, sizeof(wire));
    if (wire.header.magic != kDoctorHostProtocolMagic)
        return DoctorHostProtocolFailure::Magic;
    if (wire.header.version != kDoctorHostProtocolVersion)
        return DoctorHostProtocolFailure::Version;
    if (wire.header.total_bytes != sizeof(wire))
        return DoctorHostProtocolFailure::Size;
    if (wire.header.reserved != 0 ||
        wire.reserved != 0 ||
        wire.destination_reserved != 0) {
        return DoctorHostProtocolFailure::Reserved;
    }
    const auto command_value = wire.command;
    if (command_value <
            static_cast<std::uint32_t>(DoctorHostCommand::Status) ||
        command_value >
            static_cast<std::uint32_t>(
                DoctorHostCommand::ResumeForUpgrade)) {
        return DoctorHostProtocolFailure::Command;
    }
    const auto command =
        static_cast<DoctorHostCommand>(command_value);
    if (wire.destination_code_units >
            kDoctorHostMaximumPathCodeUnits ||
        (wire.destination_code_units == 0 &&
            command == DoctorHostCommand::Save) ||
        (wire.destination_code_units != 0 &&
            command != DoctorHostCommand::Save) ||
        !bytes_zero(
            wire.destination + wire.destination_code_units,
            (kDoctorHostMaximumPathCodeUnits -
                wire.destination_code_units) *
                sizeof(wchar_t))) {
        return DoctorHostProtocolFailure::Destination;
    }
    for (std::uint32_t index = 0;
            index < wire.destination_code_units;
            ++index) {
        if (wire.destination[index] == L'\0')
            return DoctorHostProtocolFailure::Destination;
    }

    const bool uses_mark = command_uses_mark(command);
    const bool uses_snapshot = command_uses_snapshot(command);
    if ((!uses_mark && !mark_wire_empty(wire.mark)) ||
        (!uses_snapshot && !snapshot_wire_empty(wire.snapshot)) ||
        (uses_snapshot &&
            (wire.snapshot.epoch < 0 ||
                wire.snapshot.handle == 0))) {
        return DoctorHostProtocolFailure::Snapshot;
    }

    request->command = command;
    request->mark = host_mark(wire.mark);
    request->snapshot = DoctorHostSnapshot{
        wire.snapshot.epoch, wire.snapshot.handle};
    if (wire.destination_code_units) {
        request->destination.assign(
            wire.destination, wire.destination_code_units);
    } else {
        request->destination.clear();
    }
    return DoctorHostProtocolFailure::None;
}

inline DoctorHostWireState wire_state(
    const DoctorStoreState &state) {
    return DoctorHostWireState{
        static_cast<std::uint8_t>(state.enabled),
        static_cast<std::uint8_t>(state.collecting),
        static_cast<std::uint8_t>(state.pending_clear),
        0,
        state.epoch,
        static_cast<std::uint32_t>(state.failure),
        state.platform_result_code,
        state.raw_bytes,
        state.temp_bytes,
        state.sequence_gap,
        state.ignored_lines,
        static_cast<std::uint8_t>(state.producer_limit_reached),
        static_cast<std::uint8_t>(state.has_retained_range),
        state.oldest_retained_ms,
        state.newest_retained_ms,
    };
}

inline bool host_state(
    const DoctorHostWireState &wire,
    DoctorStoreState *state) {
    if (!state ||
        wire.enabled > 1 ||
        wire.collecting > 1 ||
        wire.pending_clear > 1 ||
        wire.reserved != 0 ||
        wire.producer_limit_reached > 1 ||
        wire.has_retained_range > 1 || wire.oldest_retained_ms < 0 ||
        wire.newest_retained_ms < wire.oldest_retained_ms ||
        (!wire.has_retained_range && (wire.oldest_retained_ms || wire.newest_retained_ms)) ||
        !store_failure_valid(wire.failure)) {
        return false;
    }
    *state = DoctorStoreState{
        wire.enabled != 0,
        wire.collecting != 0,
        wire.pending_clear != 0,
        wire.epoch,
        static_cast<DoctorStoreFailure>(wire.failure),
        wire.platform_result_code,
        wire.raw_bytes,
        wire.temp_bytes,
        wire.sequence_gap,
        wire.ignored_lines,
        wire.producer_limit_reached != 0,
        wire.has_retained_range != 0,
        wire.oldest_retained_ms,
        wire.newest_retained_ms,
    };
    return true;
}

inline DoctorHostWirePreview wire_preview(
    const DoctorExportPreview &preview) {
    return DoctorHostWirePreview{
        preview.epoch,
        static_cast<std::uint64_t>(preview.record_count),
        preview.sequence_gap,
        preview.missing_before_ms,
        preview.missing_after_ms,
    };
}

inline bool host_preview(
    const DoctorHostWirePreview &wire,
    DoctorExportPreview *preview) {
    if (!preview ||
        wire.record_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    *preview = DoctorExportPreview{
        wire.epoch,
        static_cast<std::size_t>(wire.record_count),
        wire.sequence_gap,
        wire.missing_before_ms,
        wire.missing_after_ms,
    };
    return true;
}

inline void encode_reply_wire(
    const DoctorHostReply &reply,
    DoctorHostWireReply *wire) {
    if (!wire) return;
    std::memset(wire, 0, sizeof(*wire));
    wire->header.magic = kDoctorHostProtocolMagic;
    wire->header.version = kDoctorHostProtocolVersion;
    wire->header.total_bytes =
        static_cast<std::uint32_t>(sizeof(*wire));
    wire->protocol_failure = reply.protocol_failure;
    wire->store_failure =
        static_cast<std::uint32_t>(reply.failure);
    wire->platform_result_code = reply.platform_result_code;
    wire->status = wire_state(reply.status);
    wire->mark = wire_mark(reply.mark);
    wire->snapshot = DoctorHostWireSnapshot{
        reply.snapshot.epoch, reply.snapshot.handle};
    wire->preview = wire_preview(reply.preview);
}

inline bool decode_reply_wire(
    const unsigned char *bytes,
    std::size_t size,
    DoctorHostReply *reply) {
    if (!reply || !bytes ||
        size != sizeof(DoctorHostWireReply)) {
        return false;
    }
    DoctorHostWireReply wire{};
    std::memcpy(&wire, bytes, sizeof(wire));
    if (wire.header.magic != kDoctorHostProtocolMagic ||
        wire.header.version != kDoctorHostProtocolVersion ||
        wire.header.total_bytes != sizeof(wire) ||
        wire.header.reserved != 0 ||
        !store_failure_valid(wire.store_failure) ||
        static_cast<std::uint32_t>(wire.protocol_failure) >
            static_cast<std::uint32_t>(DoctorHostProtocolFailure::Reply)) {
        return false;
    }
    DoctorStoreState state{};
    DoctorExportPreview preview{};
    if (!host_state(wire.status, &state) ||
        !host_preview(wire.preview, &preview)) {
        return false;
    }
    reply->protocol_failure = wire.protocol_failure;
    reply->failure =
        static_cast<DoctorStoreFailure>(wire.store_failure);
    reply->platform_result_code = wire.platform_result_code;
    reply->status = state;
    reply->mark = host_mark(wire.mark);
    reply->snapshot = DoctorHostSnapshot{
        wire.snapshot.epoch, wire.snapshot.handle};
    reply->preview = preview;
    return true;
}

inline std::uint64_t random_snapshot_handle() {
    for (int attempt = 0; attempt != 4; ++attempt) {
        std::uint64_t value = 0;
        if (BCryptGenRandom(
                nullptr,
                reinterpret_cast<PUCHAR>(&value),
                static_cast<ULONG>(sizeof(value)),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 &&
            value != 0) {
            return value;
        }
    }
    static volatile LONG64 fallback = 0;
    auto value = static_cast<std::uint64_t>(
        InterlockedIncrement64(&fallback)) ^
        static_cast<std::uint64_t>(GetTickCount64()) ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
    return value == 0 ? 1 : value;
}

inline bool options_valid(const HostOptions &options) {
    return options.root.is_absolute() &&
        !options.settings_exe_path.empty() &&
        options.settings_exe_path.is_absolute() &&
        !options.run_key_path.empty() &&
        doctor_event_detail::valid_build(options.build);
}

inline bool maintenance_action_command(
    DoctorHostMaintenanceAction action,
    DoctorHostCommand *command) {
    if (!command) return false;
    switch (action) {
    case DoctorHostMaintenanceAction::StopForUpgrade:
        *command = DoctorHostCommand::StopForUpgrade;
        return true;
    case DoctorHostMaintenanceAction::StopForRemoval:
        *command = DoctorHostCommand::StopForRemoval;
        return true;
    case DoctorHostMaintenanceAction::ResumeAfterUpgrade:
        *command = DoctorHostCommand::ResumeForUpgrade;
        return true;
    }
    return false;
}

inline bool maintenance_host_command(DoctorHostCommand command) {
    return command == DoctorHostCommand::StopForUpgrade ||
        command == DoctorHostCommand::StopForRemoval ||
        command == DoctorHostCommand::ResumeForUpgrade;
}

inline bool maintenance_process_token_valid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const auto identity = detail::token_identity(token);
    TOKEN_ELEVATION elevation{};
    DWORD copied = 0;
    DWORD session = 0;
    std::vector<unsigned char> owner;
    const bool valid =
        identity.result_hr == static_cast<std::int32_t>(S_OK) &&
        identity.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID) &&
        identity.app_container == 0 &&
        GetTokenInformation(
            token,
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &copied) != FALSE &&
        elevation.TokenIsElevated == 0 &&
        SUCCEEDED(detail::token_session_id(token, &session)) &&
        session != 0 &&
        SUCCEEDED(detail::token_sid_copy(token, &owner)) &&
        IsValidSid(owner.data()) &&
        !IsWellKnownSid(owner.data(), WinLocalSystemSid) &&
        !IsWellKnownSid(owner.data(), WinLocalServiceSid) &&
        !IsWellKnownSid(owner.data(), WinNetworkServiceSid);
    CloseHandle(token);
    return valid;
}

inline std::wstring quote(const std::wstring &value) {
    return value.find(L'"') == std::wstring::npos
        ? L"\"" + value + L"\""
        : std::wstring();
}

inline DoctorStoreActionResult set_run_value(
    const HostOptions &options) {
    const auto executable = quote(options.settings_exe_path.wstring());
    if (executable.empty())
        return action_result(
            DoctorStoreFailure::InvalidSchema,
            static_cast<std::int32_t>(E_INVALIDARG));
    const std::wstring value =
        executable + L" --doctor-collector";
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        options.run_key_path.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (opened != ERROR_SUCCESS) return registry_result(opened);
    const LSTATUS written = RegSetValueExW(
        key,
        kDoctorHostRunValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE *>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return written == ERROR_SUCCESS ? action_result(
        DoctorStoreFailure::None) : registry_result(written);
}

inline DoctorStoreActionResult remove_run_value(
    const HostOptions &options) {
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        options.run_key_path.c_str(),
        0,
        KEY_SET_VALUE,
        &key);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND)
        return {};
    if (opened != ERROR_SUCCESS) return registry_result(opened);
    const LSTATUS removed =
        RegDeleteValueW(key, kDoctorHostRunValueName);
    RegCloseKey(key);
    return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND
        ? action_result(DoctorStoreFailure::None)
        : registry_result(removed);
}

struct BoundedIo {
    bool complete = false;
    bool timed_out = false;
    bool more_data = false;
    DWORD transferred = 0;
    DWORD error = ERROR_SUCCESS;
};

inline DWORD remaining_ms(std::uint64_t deadline) {
    const auto now = GetTickCount64();
    if (now >= deadline) return 0;
    const auto remaining = deadline - now;
    return remaining > std::numeric_limits<DWORD>::max()
        ? std::numeric_limits<DWORD>::max()
        : static_cast<DWORD>(remaining);
}

inline void cancel_pending_io(HANDLE handle, OVERLAPPED *operation) noexcept {
    CancelIoEx(handle, operation);
    // OVERLAPPED and its buffers remain owned until Windows acknowledges
    // cancellation; a fixed sleep does not establish that lifetime boundary.
    DWORD transferred = 0;
    GetOverlappedResult(handle, operation, &transferred, TRUE);
}

inline BoundedIo client_io(
    HANDLE handle,
    bool write,
    const void *input,
    DWORD input_bytes,
    void *output,
    DWORD output_bytes,
    DWORD timeout_ms) {
    BoundedIo result;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        result.error = GetLastError();
        return result;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    DWORD transferred = 0;
    const BOOL immediate = write
        ? WriteFile(
            handle, input, input_bytes, &transferred, &overlapped)
        : ReadFile(
            handle, output, output_bytes, &transferred, &overlapped);
    if (immediate) {
        result.complete = true;
        result.transferred = transferred;
        CloseHandle(event);
        return result;
    }
    const DWORD start_error = GetLastError();
    if (start_error == ERROR_MORE_DATA) {
        result.complete = true;
        result.more_data = true;
        result.transferred = transferred;
        result.error = start_error;
        CloseHandle(event);
        return result;
    }
    if (start_error != ERROR_IO_PENDING) {
        result.error = start_error;
        CloseHandle(event);
        return result;
    }
    const DWORD wait = WaitForSingleObject(event, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
        result.error = ERROR_TIMEOUT;
        cancel_pending_io(handle, &overlapped);
        CloseHandle(event);
        return result;
    }
    if (wait != WAIT_OBJECT_0) {
        result.error = GetLastError();
        cancel_pending_io(handle, &overlapped);
        CloseHandle(event);
        return result;
    }
    const BOOL complete =
        GetOverlappedResult(handle, &overlapped, &transferred, FALSE);
    result.complete = complete != FALSE;
    result.transferred = transferred;
    result.error = complete ? ERROR_SUCCESS : GetLastError();
    result.more_data = result.error == ERROR_MORE_DATA;
    CloseHandle(event);
    return result;
}

class ActiveSnapshot {
public:
    ActiveSnapshot() = default;
    ActiveSnapshot(const ActiveSnapshot &) = delete;
    ActiveSnapshot &operator=(const ActiveSnapshot &) = delete;

    ~ActiveSnapshot() { reset(); }

    bool valid() const noexcept { return valid_; }
    const DoctorExportSnapshot &store_snapshot() const noexcept {
        return store_snapshot_;
    }
    const DoctorHostSnapshot &host_snapshot() const noexcept {
        return host_snapshot_;
    }
    HANDLE client_process() const noexcept { return client_process_; }

    bool replace(
        const DoctorExportSnapshot &store_snapshot,
        std::uint64_t handle,
        HANDLE client_process) {
        if (!client_process) return false;
        reset();
        store_snapshot_ = store_snapshot;
        host_snapshot_ = DoctorHostSnapshot{
            store_snapshot.epoch, handle};
        client_process_ = client_process;
        valid_ = true;
        return true;
    }

    void reset() {
        if (client_process_) CloseHandle(client_process_);
        client_process_ = nullptr;
        store_snapshot_ = DoctorExportSnapshot{};
        host_snapshot_ = DoctorHostSnapshot{};
        valid_ = false;
    }

private:
    bool valid_ = false;
    DoctorExportSnapshot store_snapshot_{};
    DoctorHostSnapshot host_snapshot_{};
    HANDLE client_process_ = nullptr;
};

class DoctorHostOwner {
public:
    enum class Mode : std::uint32_t {
        Once = 1,
        Collector = 2,
    };

    DoctorHostOwner(
        HostOptions options,
        Mode mode)
        : options_(std::move(options)),
          mode_(mode),
          last_activity_ms_(GetTickCount64()) {}

    DoctorHostOwner(const DoctorHostOwner &) = delete;
    DoctorHostOwner &operator=(const DoctorHostOwner &) = delete;

    ~DoctorHostOwner() {
        if (holder_ && active_snapshot_.valid()) {
            holder_->cancel(active_snapshot_.store_snapshot());
        }
        if (pipe_ && pipe_ != INVALID_HANDLE_VALUE)
            CloseHandle(pipe_);
    }

    int run() {
        if (!options_valid(options_)) return static_cast<int>(
            DoctorHostExitCode::Error);
        options_.settings_exe_path =
            canonical_path(options_.settings_exe_path);
        if (options_.settings_exe_path.empty()) return static_cast<int>(
            DoctorHostExitCode::Error);

        if (!detail::own_user_sid(&user_sid_)) return static_cast<int>(
            DoctorHostExitCode::Error);
        auto opened = DoctorRuntimeHolder::open(
            options_.root,
            options_.build,
            clock_,
            DoctorStoreLimits::defaults());
        if (!opened.ok()) {
            if (opened.platform_result_code ==
                static_cast<std::int32_t>(
                    HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))) {
                return static_cast<int>(
                    DoctorHostExitCode::OwnerConflict);
            }
            return static_cast<int>(DoctorHostExitCode::Error);
        }
        holder_ = std::move(opened.holder);
        if (!holder_) return static_cast<int>(
            DoctorHostExitCode::Error);
        if (!holder_->recover_pending_export().ok())
            return static_cast<int>(DoctorHostExitCode::Error);

        for (const auto &producer : options_.producers) {
            if (producer.process_id == 0 ||
                producer.process_creation == 0) {
                return static_cast<int>(DoctorHostExitCode::Error);
            }
            if (!holder_->attach(
                    producer.process_id,
                    producer.process_creation).ok()) {
                return static_cast<int>(DoctorHostExitCode::Error);
            }
        }

        const auto status = holder_->status();
        if (mode_ == Mode::Collector && !status.enabled)
            return static_cast<int>(DoctorHostExitCode::Success);
        if (!pipe_security_.create(user_sid_))
            return static_cast<int>(DoctorHostExitCode::Error);
        if (!create_pipe())
            return static_cast<int>(DoctorHostExitCode::Error);

        maintain();
        while (!stop_) {
            if (!serve_one_connection())
                continue;
            maintain();
        }
        return static_cast<int>(DoctorHostExitCode::Success);
    }

private:
    bool create_pipe() {
        const auto name = DoctorHostEndpoint::pipe_name(user_sid_);
        if (name.empty()) return false;
        pipe_ = CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX |
                FILE_FLAG_FIRST_PIPE_INSTANCE |
                FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE |
                PIPE_READMODE_MESSAGE |
                PIPE_REJECT_REMOTE_CLIENTS,
            1,
            static_cast<DWORD>(sizeof(DoctorHostWireReply) + 1),
            static_cast<DWORD>(sizeof(DoctorHostWireRequest) + 1),
            100,
            &pipe_security_.attributes);
        return pipe_ && pipe_ != INVALID_HANDLE_VALUE;
    }

    bool maintain() {
        if (!holder_) return false;
        const auto now = GetTickCount64();
        if (now - last_discovery_ms_ >= kAttachRefreshMs) {
            discover_producers();
            last_discovery_ms_ = GetTickCount64();
        }
        holder_->drain();
        holder_->heartbeat();
        if (active_snapshot_.valid() &&
            active_snapshot_.client_process() &&
            WaitForSingleObject(
                active_snapshot_.client_process(), 0) ==
                WAIT_OBJECT_0) {
            const auto cancelled =
                holder_->cancel(
                    active_snapshot_.store_snapshot());
            if (cancelled.ok()) active_snapshot_.reset();
        }
        if (mode_ == Mode::Once &&
            !active_snapshot_.valid() &&
            !holder_->status().enabled &&
            now - last_activity_ms_ >=
                options_.idle_timeout_ms) {
            stop_ = true;
        }
        if (stop_after_pending_export_ && !active_snapshot_.valid() &&
            !holder_->status().enabled) stop_ = true;
        return true;
    }

    void discover_producers() {
        if (!holder_) return;
        const auto state = holder_->status();
        if (!state.enabled || !state.collecting) return;
        HANDLE snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                std::uint64_t creation = 0;
                if (!detail::process_creation_stamp(
                        entry.th32ProcessID, &creation) ||
                    !creation) {
                    continue;
                }
                holder_->attach(entry.th32ProcessID, creation);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    enum class ServerIoState {
        Complete,
        Timeout,
        Disconnected,
        MoreData,
    };

    ServerIoState wait_server_io(
        HANDLE pipe,
        bool write,
        const void *input,
        DWORD input_bytes,
        void *output,
        DWORD output_bytes,
        DWORD *transferred) {
        if (transferred) *transferred = 0;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return ServerIoState::Disconnected;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        DWORD completed = 0;
        const BOOL immediate = write
            ? WriteFile(
                pipe, input, input_bytes, &completed, &overlapped)
            : ReadFile(
                pipe, output, output_bytes, &completed, &overlapped);
        if (immediate) {
            if (transferred) *transferred = completed;
            CloseHandle(event);
            return ServerIoState::Complete;
        }
        const DWORD initial_error = GetLastError();
        if (initial_error == ERROR_MORE_DATA) {
            if (transferred) *transferred = completed;
            CloseHandle(event);
            return ServerIoState::MoreData;
        }
        if (initial_error != ERROR_IO_PENDING) {
            CloseHandle(event);
            return ServerIoState::Disconnected;
        }

        const auto deadline =
            GetTickCount64() + kPipeIoTimeoutMs;
        for (;;) {
            maintain();
            const DWORD wait = WaitForSingleObject(event, kIoPollMs);
            if (wait == WAIT_OBJECT_0) {
                const BOOL result = GetOverlappedResult(
                    pipe, &overlapped, &completed, FALSE);
                CloseHandle(event);
                if (!result) {
                    return GetLastError() == ERROR_MORE_DATA
                        ? ServerIoState::MoreData
                        : ServerIoState::Disconnected;
                }
                if (transferred) *transferred = completed;
                return ServerIoState::Complete;
            }
            if (wait != WAIT_TIMEOUT ||
                GetTickCount64() >= deadline) {
                cancel_pending_io(pipe, &overlapped);
                CloseHandle(event);
                return ServerIoState::Timeout;
            }
        }
    }

    bool connect_pipe() {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        if (ConnectNamedPipe(pipe_, &overlapped)) {
            CloseHandle(event);
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            CloseHandle(event);
            return true;
        }
        if (error != ERROR_IO_PENDING) {
            CloseHandle(event);
            return false;
        }
        for (;;) {
            maintain();
            if (stop_) {
                cancel_pending_io(pipe_, &overlapped);
                CloseHandle(event);
                return false;
            }
            const DWORD wait =
                WaitForSingleObject(event, kIoPollMs);
            if (wait == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                const BOOL result = GetOverlappedResult(
                    pipe_, &overlapped, &transferred, FALSE);
                CloseHandle(event);
                return result != FALSE ||
                    GetLastError() == ERROR_PIPE_CONNECTED;
            }
            if (wait != WAIT_TIMEOUT) {
                cancel_pending_io(pipe_, &overlapped);
                CloseHandle(event);
                return false;
            }
        }
    }

    bool authenticate_client(ProcessIdentity *identity) {
        if (!identity) return false;
        DWORD process_id = 0;
        return GetNamedPipeClientProcessId(
                pipe_, &process_id) != FALSE &&
            identity->open(
                process_id, options_.settings_exe_path);
    }

    bool serve_one_connection() {
        if (!connect_pipe()) {
            if (!stop_) {
                DisconnectNamedPipe(pipe_);
                Sleep(kIoPollMs);
            }
            return false;
        }

        ProcessIdentity identity;
        if (!authenticate_client(&identity)) {
            DisconnectNamedPipe(pipe_);
            return false;
        }

        std::vector<unsigned char> bytes(
            sizeof(DoctorHostWireRequest) + 1);
        DWORD transferred = 0;
        const auto read_result = wait_server_io(
            pipe_,
            false,
            nullptr,
            0,
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &transferred);
        if (read_result == ServerIoState::Timeout ||
            read_result == ServerIoState::Disconnected) {
            DisconnectNamedPipe(pipe_);
            return false;
        }
        if (read_result == ServerIoState::MoreData) {
            transferred =
                static_cast<DWORD>(bytes.size());
        } else if (transferred > bytes.size()) {
            transferred =
                static_cast<DWORD>(bytes.size());
        }
        bytes.resize(transferred);

        DoctorHostWireReply wire_reply{};
        DoctorHostReply reply{};
        DoctorHostRequest request{};
        reply.protocol_failure = decode_request_wire(
            bytes.data(), bytes.size(), &request);
        if (reply.protocol_failure ==
            DoctorHostProtocolFailure::None) {
            if (maintenance_host_command(request.command) &&
                !identity.normal_medium()) {
                reply.failure =
                    DoctorStoreFailure::PermissionDenied;
                reply.platform_result_code =
                    static_cast<std::int32_t>(E_ACCESSDENIED);
            } else {
                process_request(request, &identity, &reply);
            }
            last_activity_ms_ = GetTickCount64();
        }
        reply.status = holder_->status();
        encode_reply_wire(reply, &wire_reply);
        DWORD written = 0;
        const auto write_result = wait_server_io(
            pipe_,
            true,
            &wire_reply,
            sizeof(wire_reply),
            nullptr,
            0,
            &written);
        if (request.command ==
                DoctorHostCommand::ResumeForUpgrade &&
            write_result != ServerIoState::Complete) {
            remove_run_value(options_);
            stop_after_pending_export_ = true;
            stop_ = true;
        }
        if (write_result == ServerIoState::Complete &&
            written ==
                static_cast<DWORD>(sizeof(wire_reply))) {
            unsigned char extra{};
            DWORD ignored = 0;
            // Write completion only queues the reply; wait boundedly for
            // client close so DisconnectNamedPipe cannot discard it.
            wait_server_io(
                pipe_,
                false,
                nullptr,
                0,
                &extra,
                static_cast<DWORD>(sizeof(extra)),
                &ignored);
        }
        DisconnectNamedPipe(pipe_);
        return true;
    }

    void set_action_reply(
        const DoctorStoreActionResult &result,
        DoctorHostReply *reply) {
        if (!reply) return;
        reply->failure = result.failure;
        reply->platform_result_code = result.platform_result_code;
    }

    void stop_for_maintenance(DoctorHostReply *reply) {
        if (!reply) return;
        if (active_snapshot_.valid()) {
            const auto cancelled =
                holder_->cancel(active_snapshot_.store_snapshot());
            set_action_reply(cancelled, reply);
            if (!cancelled.ok()) return;
            active_snapshot_.reset();
        }
        const auto removed = remove_run_value(options_);
        set_action_reply(removed, reply);
        if (!removed.ok()) return;
        stop_after_pending_export_ = true;
        stop_ = true;
    }

    void resume_after_upgrade(DoctorHostReply *reply) {
        if (!reply) return;
        const auto status = holder_->status();
        if (status.failure != DoctorStoreFailure::None || status.pending_clear) {
            // An unreadable store is not an explicit disabled consent state.
            // Keep the original failure visible and do not leave a new owner
            // or a stale autostart value after an unsuccessful resume.
            remove_run_value(options_);
            reply->failure = status.failure != DoctorStoreFailure::None
                ? status.failure : DoctorStoreFailure::ClearIncomplete;
            reply->platform_result_code = status.platform_result_code;
            stop_after_pending_export_ = true;
            stop_ = true;
            return;
        }
        const auto updated = status.enabled
            ? set_run_value(options_)
            : remove_run_value(options_);
        set_action_reply(updated, reply);
        if (status.enabled && updated.ok()) {
            stop_after_pending_export_ = false;
            stop_ = false;
            return;
        }
        stop_after_pending_export_ = true;
        stop_ = true;
    }

    bool resolve_snapshot(
        const DoctorHostSnapshot &snapshot,
        DoctorExportSnapshot *store_snapshot,
        DoctorHostReply *reply) {
        if (!store_snapshot || !reply) return false;
        const auto status = holder_->status();
        if (snapshot.epoch != status.epoch) {
            reply->failure = DoctorStoreFailure::EpochMismatch;
            return false;
        }
        if (!active_snapshot_.valid() ||
            active_snapshot_.host_snapshot().epoch != snapshot.epoch ||
            active_snapshot_.host_snapshot().handle != snapshot.handle) {
            reply->failure = DoctorStoreFailure::ExportExpired;
            return false;
        }
        *store_snapshot =
            active_snapshot_.store_snapshot();
        return true;
    }

    void prepare_reply(
        const DoctorStoreResult<DoctorExportSnapshot> &prepared,
        ProcessIdentity *identity,
        DoctorHostReply *reply) {
        if (!reply || !identity) return;
        if (!prepared.ok()) {
            reply->failure = prepared.failure;
            reply->platform_result_code =
                prepared.platform_result_code;
            return;
        }
        HANDLE process = nullptr;
        const auto handle = random_snapshot_handle();
        if (!identity->take_process(&process) ||
            !active_snapshot_.replace(
                prepared.value, handle, process)) {
            if (process) CloseHandle(process);
            set_action_reply(
                holder_->cancel(prepared.value), reply);
            reply->failure = DoctorStoreFailure::IoError;
            reply->platform_result_code =
                static_cast<std::int32_t>(E_UNEXPECTED);
            return;
        }
        reply->snapshot = active_snapshot_.host_snapshot();
    }

    void process_request(
        const DoctorHostRequest &request,
        ProcessIdentity *identity,
        DoctorHostReply *reply) {
        if (!holder_ || !reply || !identity) return;
        switch (request.command) {
        case DoctorHostCommand::Status:
            break;
        case DoctorHostCommand::Enable: {
            auto result = holder_->enable();
            if (result.ok()) {
                const auto registered = set_run_value(options_);
                if (!registered.ok()) {
                    holder_->disable();
                    result = registered;
                }
                if (result.ok()) stop_after_pending_export_ = false;
            }
            set_action_reply(result, reply);
            break;
        }
        case DoctorHostCommand::Disable: {
            auto result = holder_->disable();
            if (result.ok()) {
                const auto removed = remove_run_value(options_);
                if (!removed.ok()) result = removed;
                stop_after_pending_export_ = true;
                stop_ = !active_snapshot_.valid();
            }
            set_action_reply(result, reply);
            break;
        }
        case DoctorHostCommand::StopForUpgrade:
        case DoctorHostCommand::StopForRemoval:
            stop_for_maintenance(reply);
            break;
        case DoctorHostCommand::ResumeForUpgrade:
            resume_after_upgrade(reply);
            break;
        case DoctorHostCommand::Mark: {
            const auto marked = holder_->mark(
                request.mark.selected_issue_time_ms,
                request.mark.before_ms,
                request.mark.after_ms);
            if (marked.ok()) {
                reply->mark = host_mark(marked.value);
            } else {
                reply->failure = marked.failure;
                reply->platform_result_code =
                    marked.platform_result_code;
            }
            break;
        }
        case DoctorHostCommand::Prepare: {
            DoctorStoreResult<DoctorExportSnapshot> prepared;
            if (request.mark.action_time_ms != 0 ||
                request.mark.epoch != 0) {
                prepared =
                    holder_->prepare(store_mark(request.mark));
            } else {
                prepared = holder_->prepare(
                    request.mark.selected_issue_time_ms,
                    request.mark.before_ms,
                    request.mark.after_ms);
            }
            prepare_reply(prepared, identity, reply);
            break;
        }
        case DoctorHostCommand::Preview: {
            DoctorExportSnapshot snapshot{};
            if (!resolve_snapshot(
                    request.snapshot, &snapshot, reply)) {
                break;
            }
            const auto previewed = holder_->preview(snapshot);
            if (previewed.ok()) {
                reply->preview = previewed.value;
            } else {
                reply->failure = previewed.failure;
                reply->platform_result_code =
                    previewed.platform_result_code;
            }
            break;
        }
        case DoctorHostCommand::Save: {
            DoctorExportSnapshot snapshot{};
            if (!resolve_snapshot(
                    request.snapshot, &snapshot, reply)) {
                break;
            }
            const auto saved = holder_->save(
                snapshot,
                std::filesystem::path(request.destination));
            set_action_reply(saved, reply);
            if (saved.ok()) active_snapshot_.reset();
            break;
        }
        case DoctorHostCommand::Cancel: {
            DoctorExportSnapshot snapshot{};
            if (!resolve_snapshot(
                    request.snapshot, &snapshot, reply)) {
                break;
            }
            const auto cancelled = holder_->cancel(snapshot);
            set_action_reply(cancelled, reply);
            if (cancelled.ok()) active_snapshot_.reset();
            break;
        }
        case DoctorHostCommand::Clear: {
            const auto cleared = holder_->clear();
            set_action_reply(cleared, reply);
            if (cleared.ok()) active_snapshot_.reset();
            break;
        }
        }
    }

    HostOptions options_;
    Mode mode_;
    SystemDoctorClock clock_;
    std::unique_ptr<DoctorRuntimeHolder> holder_;
    std::wstring user_sid_;
    PipeSecurity pipe_security_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    ActiveSnapshot active_snapshot_;
    bool stop_after_pending_export_ = false;
    std::uint64_t last_discovery_ms_ = 0;
    std::uint64_t last_activity_ms_ = 0;
    bool stop_ = false;
};

inline DoctorHostClientFailure io_failure(
    const BoundedIo &io) {
    if (io.timed_out) return DoctorHostClientFailure::Timeout;
    if (io.error == ERROR_ACCESS_DENIED)
        return DoctorHostClientFailure::AccessDenied;
    return DoctorHostClientFailure::Disconnect;
}

inline std::wstring environment_wide(const std::string &value) {
    return std::wstring(value.begin(), value.end());
}

inline bool read_environment(
    const wchar_t *name, std::wstring *value) {
    if (!name || !value) return false;
    DWORD length =
        GetEnvironmentVariableW(name, nullptr, 0);
    if (!length || length > 32768) {
        value->clear();
        return false;
    }
    std::vector<wchar_t> storage(length);
    length = GetEnvironmentVariableW(
        name, storage.data(), length);
    if (!length) {
        value->clear();
        return false;
    }
    value->assign(storage.data(), length);
    return true;
}

inline bool append_environment(
    std::vector<wchar_t> *block,
    const wchar_t *name,
    const std::wstring &value) {
    if (!block || !name) return false;
    const std::wstring entry =
        std::wstring(name) + L"=" + value;
    block->insert(block->end(), entry.begin(), entry.end());
    block->push_back(L'\0');
    return true;
}

inline std::vector<wchar_t> test_environment_block(
    const HostOptions &options) {
    std::vector<wchar_t> result;
    LPWCH inherited = GetEnvironmentStringsW();
    if (inherited) {
        for (const wchar_t *entry = inherited;
                *entry != L'\0';
                entry += std::wcslen(entry) + 1) {
            if (std::wcsncmp(
                    entry,
                    kTestEnvironmentPrefix,
                    std::wcslen(kTestEnvironmentPrefix)) == 0) {
                continue;
            }
            const auto length = std::wcslen(entry);
            result.insert(
                result.end(), entry, entry + length + 1);
        }
        FreeEnvironmentStringsW(inherited);
    }
    append_environment(
        &result, kTestRootVariable, options.root.wstring());
    append_environment(
        &result,
        kTestVersionVariable,
        environment_wide(options.build.version));
    append_environment(
        &result,
        kTestCommitVariable,
        environment_wide(options.build.source_commit));
    append_environment(
        &result,
        kTestExecutableVariable,
        options.settings_exe_path.wstring());
    append_environment(
        &result, kTestRunKeyVariable, options.run_key_path);
    append_environment(
        &result,
        kTestIdleVariable,
        std::to_wstring(options.idle_timeout_ms));
    result.push_back(L'\0');
    return result;
}

inline bool start_once_process(const HostOptions &options) {
    const auto application =
        options.settings_exe_path.wstring();
    std::wstring command =
        L"\"" + application + L"\" --doctor-once";
    if (application.find(L'"') != std::wstring::npos) return false;
    std::vector<wchar_t> mutable_command(
        command.begin(), command.end());
    mutable_command.push_back(L'\0');
    std::vector<wchar_t> environment;
    if (options.test_environment)
        environment = test_environment_block(options);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        application.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW |
            (options.test_environment
                ? CREATE_UNICODE_ENVIRONMENT
                : 0),
        options.test_environment
            ? environment.data()
            : nullptr,
        nullptr,
        &startup,
        &process);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

inline DoctorHostClientResult request_internal(
    const HostOptions &options,
    const DoctorHostRequest &request,
    DWORD timeout_ms,
    bool auto_start_if_absent) {
    DoctorHostClientResult result;
    result.failure = DoctorHostClientFailure::InvalidRequest;
    DoctorHostWireRequest wire_request{};
    if (!encode_request_wire(request, &wire_request)) return result;

    std::wstring user_sid;
    if (!detail::own_user_sid(&user_sid)) return result;
    const auto pipe_name = DoctorHostEndpoint::pipe_name(user_sid);
    if (pipe_name.empty()) return result;
    const auto expected_executable =
        canonical_path(options.settings_exe_path);
    if (expected_executable.empty()) return result;

    const auto deadline =
        GetTickCount64() + static_cast<std::uint64_t>(timeout_ms);
    bool start_attempted = false;
    bool endpoint_missing = true;
    for (;;) {
        HANDLE pipe = CreateFileW(
            pipe_name.c_str(),
            GENERIC_READ |
                GENERIC_WRITE |
                READ_CONTROL,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED) {
                result.failure =
                    DoctorHostClientFailure::AccessDenied;
                result.platform_result_code =
                    static_cast<std::int32_t>(
                        HRESULT_FROM_WIN32(error));
                return result;
            }
            if ((error == ERROR_FILE_NOT_FOUND ||
                    error == ERROR_PATH_NOT_FOUND) &&
                auto_start_if_absent && !start_attempted) {
                start_attempted = true;
                if (!start_once_process(options)) {
                    result.failure =
                        DoctorHostClientFailure::ProcessStartFailed;
                    result.platform_result_code =
                        static_cast<std::int32_t>(
                            detail::hresult_from_last_error());
                    return result;
                }
            }
            if (GetTickCount64() >= deadline) {
                result.failure = endpoint_missing
                    ? DoctorHostClientFailure::EndpointUnavailable
                    : DoctorHostClientFailure::Timeout;
                return result;
            }
            const DWORD wait =
                std::min<DWORD>(
                    remaining_ms(deadline), 50);
            WaitNamedPipeW(pipe_name.c_str(), wait);
            continue;
        }
        endpoint_missing = false;

        DWORD server_pid = 0;
        ProcessIdentity server;
        if (!pipe_security_valid(pipe) ||
            !GetNamedPipeServerProcessId(pipe, &server_pid) ||
            !server.open(
                server_pid,
                expected_executable,
                maintenance_host_command(request.command))) {
            result.failure =
                DoctorHostClientFailure::EndpointUntrusted;
            result.platform_result_code =
                static_cast<std::int32_t>(E_ACCESSDENIED);
            CloseHandle(pipe);
            return result;
        }
        result.server_pid = server_pid;

        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(
                pipe, &mode, nullptr, nullptr)) {
            result.failure =
                DoctorHostClientFailure::Disconnect;
            result.platform_result_code =
                static_cast<std::int32_t>(
                    hresult_from_last_error());
            CloseHandle(pipe);
            return result;
        }

        const auto write = client_io(
            pipe,
            true,
            &wire_request,
            sizeof(wire_request),
            nullptr,
            0,
            remaining_ms(deadline));
        if (!write.complete || write.transferred !=
                sizeof(wire_request)) {
            result.failure = io_failure(write);
            CloseHandle(pipe);
            return result;
        }

        std::array<unsigned char, sizeof(DoctorHostWireReply)>
            reply_bytes{};
        const auto read = client_io(
            pipe,
            false,
            nullptr,
            0,
            reply_bytes.data(),
            static_cast<DWORD>(reply_bytes.size()),
            remaining_ms(deadline));
        CloseHandle(pipe);
        if (!read.complete ||
            read.transferred != reply_bytes.size() ||
            !decode_reply_wire(
                reply_bytes.data(),
                reply_bytes.size(),
                &result.reply)) {
            result.failure = io_failure(read);
            return result;
        }
        if (result.reply.protocol_failure !=
            DoctorHostProtocolFailure::None) {
            result.failure =
                DoctorHostClientFailure::ProtocolRejected;
            return result;
        }
        result.failure = DoctorHostClientFailure::None;
        result.platform_result_code =
            result.reply.platform_result_code;
        const bool wait_for_exit =
            ((request.command ==
                    DoctorHostCommand::StopForUpgrade ||
                request.command ==
                    DoctorHostCommand::StopForRemoval) &&
                result.reply.failure ==
                    DoctorStoreFailure::None) ||
            (request.command ==
                    DoctorHostCommand::ResumeForUpgrade &&
                (!result.reply.status.enabled ||
                    result.reply.failure !=
                        DoctorStoreFailure::None));
        if (wait_for_exit) {
            const DWORD wait = WaitForSingleObject(
                server.process(), remaining_ms(deadline));
            if (wait != WAIT_OBJECT_0) {
                result.failure =
                    wait == WAIT_FAILED
                        ? DoctorHostClientFailure::Disconnect
                        : DoctorHostClientFailure::Timeout;
                result.platform_result_code =
                    wait == WAIT_FAILED
                        ? static_cast<std::int32_t>(
                            hresult_from_last_error())
                        : static_cast<std::int32_t>(
                            HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            }
        }
        return result;
    }
}

inline HostOptions product_host_options() {
    return HostOptions{
        doctor_directory(),
        DoctorBuildMetadata{
            boshiamy::build_info::product_version,
            boshiamy::build_info::source_commit,
        },
        current_executable_path(),
        kDoctorHostProductRunKey,
        60'000,
        {},
        false,
    };
}

inline int run_owner(
    const HostOptions &options,
    DoctorHostOwner::Mode mode) {
    try {
        return DoctorHostOwner(options, mode).run();
    } catch (...) {
        return static_cast<int>(DoctorHostExitCode::Error);
    }
}

inline int run_maintenance(
    const HostOptions &options,
    DoctorHostMaintenanceAction action,
    DWORD timeout_ms) {
    if (!maintenance_process_token_valid()) {
        return static_cast<int>(DoctorHostExitCode::Error);
    }
    DoctorHostCommand command_value =
        DoctorHostCommand::Status;
    if (!maintenance_action_command(
            action, &command_value)) {
        return static_cast<int>(DoctorHostExitCode::Error);
    }
    DoctorHostRequest request;
    request.command = command_value;
    try {
        const auto result = request_internal(
            options, request, timeout_ms, true);
        return result.failure == DoctorHostClientFailure::None &&
                result.reply.failure == DoctorStoreFailure::None
            ? static_cast<int>(DoctorHostExitCode::Success)
            : static_cast<int>(DoctorHostExitCode::Error);
    } catch (...) {
        return static_cast<int>(DoctorHostExitCode::Error);
    }
}

inline bool parse_test_idle(
    const std::wstring &value, std::uint32_t *idle) {
    if (!idle || value.empty()) return false;
    std::uint64_t parsed = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        parsed = parsed * 10 + static_cast<std::uint32_t>(ch - L'0');
        if (parsed > std::numeric_limits<std::uint32_t>::max())
            return false;
    }
    *idle = static_cast<std::uint32_t>(parsed);
    return true;
}

inline bool environment_host_options(HostOptions *options) {
    if (!options) return false;
    std::wstring root;
    std::wstring version;
    std::wstring commit;
    std::wstring executable;
    std::wstring run_key;
    std::wstring idle_text;
    if (!read_environment(kTestRootVariable, &root) ||
        !read_environment(kTestVersionVariable, &version) ||
        !read_environment(kTestCommitVariable, &commit) ||
        !read_environment(kTestExecutableVariable, &executable) ||
        !read_environment(kTestRunKeyVariable, &run_key) ||
        !read_environment(kTestIdleVariable, &idle_text)) {
        return false;
    }
    std::uint32_t idle = 0;
    if (!parse_test_idle(idle_text, &idle)) return false;
    *options = HostOptions{
        std::filesystem::path(root),
        DoctorBuildMetadata{
            std::string(version.begin(), version.end()),
            std::string(commit.begin(), commit.end()),
        },
        std::filesystem::path(executable),
        run_key,
        idle,
        {},
        true,
    };
    return options_valid(*options);
}

} // namespace doctor_host_detail

namespace doctor_host {

inline int run_once() {
    return doctor_host_detail::run_owner(
        doctor_host_detail::product_host_options(),
        doctor_host_detail::DoctorHostOwner::Mode::Once);
}

inline int run_collector() {
    return doctor_host_detail::run_owner(
        doctor_host_detail::product_host_options(),
        doctor_host_detail::DoctorHostOwner::Mode::Collector);
}

inline int run_maintenance(
    DoctorHostMaintenanceAction action) {
    return doctor_host_detail::run_maintenance(
        doctor_host_detail::product_host_options(),
        action,
        doctor_host_detail::kMaintenanceTimeoutMs);
}

inline DoctorHostClientResult request(
    const DoctorHostRequest &request,
    DWORD timeout_ms,
    bool auto_start_if_absent) {
    return doctor_host_detail::request_internal(
        doctor_host_detail::product_host_options(),
        request,
        timeout_ms,
        auto_start_if_absent);
}

} // namespace doctor_host

#ifdef BOSHIAMY_DOCTOR_HOST_TESTING
namespace doctor_host_test {

struct Options {
    std::filesystem::path root;
    DoctorBuildMetadata build;
    std::filesystem::path settings_exe_path;
    std::wstring run_key_path;
    std::uint32_t idle_timeout_ms = 30'000;
    std::vector<DoctorHostTestProducer> producer_attachments;
};

inline doctor_host_detail::HostOptions host_options(
    const Options &options) {
    doctor_host_detail::HostOptions result{
        options.root,
        options.build,
        options.settings_exe_path,
        options.run_key_path,
        options.idle_timeout_ms,
        {},
        true,
    };
    for (const auto &producer : options.producer_attachments) {
        result.producers.push_back(
            doctor_host_detail::ProducerIdentity{
                producer.process_id, producer.process_creation});
    }
    return result;
}

inline int run_once(const Options &options) {
    return doctor_host_detail::run_owner(
        host_options(options),
        doctor_host_detail::DoctorHostOwner::Mode::Once);
}

inline int run_collector(const Options &options) {
    return doctor_host_detail::run_owner(
        host_options(options),
        doctor_host_detail::DoctorHostOwner::Mode::Collector);
}

inline int run_once_from_environment() {
    doctor_host_detail::HostOptions options;
    return doctor_host_detail::environment_host_options(&options)
        ? doctor_host_detail::run_owner(
            options,
            doctor_host_detail::DoctorHostOwner::Mode::Once)
        : static_cast<int>(DoctorHostExitCode::Error);
}

inline int run_collector_from_environment() {
    doctor_host_detail::HostOptions options;
    return doctor_host_detail::environment_host_options(&options)
        ? doctor_host_detail::run_owner(
            options,
            doctor_host_detail::DoctorHostOwner::Mode::Collector)
        : static_cast<int>(DoctorHostExitCode::Error);
}

inline int run_maintenance_from_environment(
    DoctorHostMaintenanceAction action) {
    doctor_host_detail::HostOptions options;
    return doctor_host_detail::environment_host_options(&options)
        ? doctor_host_detail::run_maintenance(
            options,
            action,
            doctor_host_detail::kMaintenanceTimeoutMs)
        : static_cast<int>(DoctorHostExitCode::Error);
}

inline DoctorHostClientResult maintenance_request(
    const Options &options,
    DoctorHostMaintenanceAction action,
    DWORD timeout_ms) {
    DoctorHostCommand command_value =
        DoctorHostCommand::Status;
    if (!doctor_host_detail::maintenance_action_command(
            action, &command_value)) {
        DoctorHostClientResult result;
        result.failure = DoctorHostClientFailure::InvalidRequest;
        result.platform_result_code =
            static_cast<std::int32_t>(E_INVALIDARG);
        return result;
    }
    DoctorHostRequest request;
    request.command = command_value;
    return doctor_host_detail::request_internal(
        host_options(options), request, timeout_ms, true);
}

inline DoctorHostClientResult request(
    const Options &options,
    const DoctorHostRequest &request,
    DWORD timeout_ms,
    bool auto_start_if_absent) {
    return doctor_host_detail::request_internal(
        host_options(options),
        request,
        timeout_ms,
        auto_start_if_absent);
}

inline bool encode_wire_request(
    const DoctorHostRequest &request,
    DoctorHostWireRequest *wire) {
    return doctor_host_detail::encode_request_wire(
        request, wire);
}

} // namespace doctor_host_test
#endif

} // namespace boshiamy::doctor

#endif
