#define WIN32_LEAN_AND_MEAN
#define BOSHIAMY_DOCTOR_HOST_TESTING 1
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace review7_toolhelp {

inline constexpr const wchar_t kPrefixVariable[] =
    L"LIUKAI_DOCTOR_HOST_TEST_ENUMERATION_PREFIX";
inline constexpr const wchar_t kTargetVariable[] =
    L"LIUKAI_DOCTOR_HOST_TEST_ENUMERATION_PID";
inline constexpr const wchar_t kMarkerVariable[] =
    L"LIUKAI_DOCTOR_HOST_TEST_ENUMERATION_MARKER";

using SnapshotFunction =
    HANDLE (WINAPI *)(DWORD, DWORD);
using ProcessFirstFunction =
    BOOL (WINAPI *)(HANDLE, LPPROCESSENTRY32W);
using ProcessNextFunction =
    BOOL (WINAPI *)(HANDLE, LPPROCESSENTRY32W);

const SnapshotFunction real_snapshot =
    &::CreateToolhelp32Snapshot;
const ProcessFirstFunction real_first =
    &::Process32FirstW;
const ProcessNextFunction real_next =
    &::Process32NextW;

static HANDLE synthetic_snapshot = nullptr;
static std::size_t synthetic_next = 0;
static std::size_t synthetic_prefix = 0;
static DWORD synthetic_target = 0;

static bool read_decimal_environment(
    const wchar_t *name, std::uint64_t *value) {
    if (!value) return false;
    wchar_t storage[64]{};
    const DWORD length =
        GetEnvironmentVariableW(
            name, storage, static_cast<DWORD>(sizeof(storage) / sizeof(storage[0])));
    if (!length ||
        length >= sizeof(storage) / sizeof(storage[0])) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (DWORD index = 0; index != length; ++index) {
        const wchar_t ch = storage[index];
        if (ch < L'0' || ch > L'9') return false;
        parsed = parsed * 10 + static_cast<unsigned>(ch - L'0');
    }
    *value = parsed;
    return true;
}

static bool target_marker(PROCESSENTRY32W *entry) {
    if (!entry) return false;
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        kMarkerVariable,
        path,
        static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (!length ||
        length >= sizeof(path) / sizeof(path[0])) {
        return false;
    }
    const HANDLE file = CreateFileW(
        path, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

static void fill_invalid(PROCESSENTRY32W *entry) {
    if (!entry) return;
    std::memset(entry, 0, sizeof(*entry));
    entry->dwSize = sizeof(*entry);
    entry->th32ProcessID = 0xffffffffUL;
}

static void fill_target(PROCESSENTRY32W *entry) {
    if (!entry) return;
    std::memset(entry, 0, sizeof(*entry));
    entry->dwSize = sizeof(*entry);
    entry->th32ProcessID = synthetic_target;
}

static HANDLE WINAPI create_snapshot(
    DWORD flags, DWORD process_id) {
    std::uint64_t prefix = 0;
    std::uint64_t target = 0;
    if (process_id == 0 &&
        (flags & TH32CS_SNAPPROCESS) != 0 &&
        read_decimal_environment(kPrefixVariable, &prefix) &&
        read_decimal_environment(kTargetVariable, &target) &&
        prefix <= 4096 &&
        target != 0 &&
        target <= 0xffffffffULL) {
        synthetic_snapshot =
            CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!synthetic_snapshot) return INVALID_HANDLE_VALUE;
        synthetic_next = 0;
        synthetic_prefix =
            static_cast<std::size_t>(prefix);
        synthetic_target = static_cast<DWORD>(target);
        return synthetic_snapshot;
    }
    synthetic_snapshot = nullptr;
    return real_snapshot(flags, process_id);
}

static BOOL WINAPI process_first(
    HANDLE snapshot, LPPROCESSENTRY32W entry) {
    if (!entry || !synthetic_snapshot ||
        snapshot != synthetic_snapshot) {
        return real_first(snapshot, entry);
    }
    synthetic_next = 0;
    if (synthetic_prefix == 0) {
        fill_target(entry);
        target_marker(entry);
        synthetic_next = 1;
    } else {
        fill_invalid(entry);
        synthetic_next = 1;
    }
    return TRUE;
}

static BOOL WINAPI process_next(
    HANDLE snapshot, LPPROCESSENTRY32W entry) {
    if (!entry || !synthetic_snapshot ||
        snapshot != synthetic_snapshot) {
        return real_next(snapshot, entry);
    }
    if (synthetic_next < synthetic_prefix) {
        fill_invalid(entry);
        ++synthetic_next;
        return TRUE;
    }
    if (synthetic_next == synthetic_prefix) {
        fill_target(entry);
        ++synthetic_next;
        target_marker(entry);
        return TRUE;
    }
    return FALSE;
}

} // namespace review7_toolhelp

#define CreateToolhelp32Snapshot review7_toolhelp::create_snapshot
#define Process32FirstW review7_toolhelp::process_first
#define Process32NextW review7_toolhelp::process_next
#include "../doctor_host.hpp"
#undef Process32NextW
#undef Process32FirstW
#undef CreateToolhelp32Snapshot
#include "../doctor_store.hpp"
#include "../tsf/doctor_transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
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

const DoctorBuildMetadata kBuild{
    "0.1.0-host-test",
    "506dcded002b2b3fa9edbe6b387d3e6797aa2383",
};

constexpr DWORD kHostTimeoutMs = 5'000;
constexpr DWORD kCleanupTimeoutMs = 3'000;
constexpr DWORD kDelayedClientReadMs = 200;
constexpr DWORD kPipeRecoveryMarginMs = 2'000;

class SyntheticProcessEnumeration {
public:
    SyntheticProcessEnumeration(
        DWORD target_pid,
        std::size_t prefix,
        const fs::path &marker) {
        const bool configured =
            SetEnvironmentVariableW(
                review7_toolhelp::kPrefixVariable,
                std::to_wstring(prefix).c_str()) &&
            SetEnvironmentVariableW(
                review7_toolhelp::kTargetVariable,
                std::to_wstring(target_pid).c_str()) &&
            SetEnvironmentVariableW(
                review7_toolhelp::kMarkerVariable,
                marker.c_str());
        if (!configured) {
            SetEnvironmentVariableW(
                review7_toolhelp::kPrefixVariable, nullptr);
            SetEnvironmentVariableW(
                review7_toolhelp::kTargetVariable, nullptr);
            SetEnvironmentVariableW(
                review7_toolhelp::kMarkerVariable, nullptr);
            throw std::runtime_error(
                "Cannot configure synthetic Toolhelp enumeration");
        }
    }

    SyntheticProcessEnumeration(
        const SyntheticProcessEnumeration &) = delete;
    SyntheticProcessEnumeration &operator=(
        const SyntheticProcessEnumeration &) = delete;

    ~SyntheticProcessEnumeration() {
        SetEnvironmentVariableW(
            review7_toolhelp::kPrefixVariable, nullptr);
        SetEnvironmentVariableW(
            review7_toolhelp::kTargetVariable, nullptr);
        SetEnvironmentVariableW(
            review7_toolhelp::kMarkerVariable, nullptr);
    }
};

class OwnedTempDirectory {
public:
    OwnedTempDirectory() {
        std::error_code error;
        base_ = fs::weakly_canonical(fs::temp_directory_path(), error);
        if (error || !base_.is_absolute()) {
            throw std::runtime_error("Cannot resolve the system temp directory");
        }
        const auto prefix =
            L"liu-kai-doctor-host-" +
            std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-";
        for (std::uint32_t attempt = 0; attempt != 32; ++attempt) {
            const auto candidate =
                base_ / (prefix + std::to_wstring(counter() + attempt));
            if (fs::create_directory(candidate, error)) {
                path_ = candidate;
                prefix_ = L"liu-kai-doctor-host-";
                return;
            }
            error.clear();
        }
        throw std::runtime_error("Cannot create an owned doctor host test directory");
    }

    OwnedTempDirectory(const OwnedTempDirectory &) = delete;
    OwnedTempDirectory &operator=(const OwnedTempDirectory &) = delete;

    ~OwnedTempDirectory() {
        std::error_code error;
        const auto canonical_base = fs::weakly_canonical(base_, error);
        if (error || !canonical_base.is_absolute()) return;
        error.clear();
        const auto canonical_path = fs::weakly_canonical(path_, error);
        if (error || !canonical_path.is_absolute() ||
            canonical_path.parent_path() != canonical_base ||
            canonical_path.filename().wstring().rfind(prefix_, 0) != 0) {
            return;
        }
        error.clear();
        fs::remove_all(canonical_path, error);
    }

    const fs::path &path() const noexcept { return path_; }

private:
    static std::uint64_t counter() {
        static volatile LONG64 value = 0;
        return static_cast<std::uint64_t>(InterlockedIncrement64(&value));
    }

    fs::path base_;
    fs::path path_;
    std::wstring prefix_;
};

class TemporaryRegistryKey {
public:
    TemporaryRegistryKey() {
        path_ = L"Software\\LiuKaiDoctorHostTests\\" +
            std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(counter());
        HKEY key = nullptr;
        const LSTATUS created = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            path_.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE,
            nullptr,
            &key,
            nullptr);
        if (key) RegCloseKey(key);
        if (created != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot create temporary HKCU test key");
        }
    }

    ~TemporaryRegistryKey() {
        if (path_.rfind(L"Software\\LiuKaiDoctorHostTests\\", 0) != 0) return;
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
    }

    const std::wstring &path() const noexcept { return path_; }

    bool set_value(const std::wstring &name, const std::wstring &value) const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER, path_.c_str(), 0, KEY_SET_VALUE, &key) !=
            ERROR_SUCCESS) {
            return false;
        }
        const LSTATUS result = RegSetValueExW(
            key,
            name.c_str(),
            0,
            REG_SZ,
            reinterpret_cast<const BYTE *>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    bool query_value(
        const std::wstring &name, std::wstring *value) const {
        if (!value) return false;
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER, path_.c_str(), 0, KEY_QUERY_VALUE, &key) !=
            ERROR_SUCCESS) {
            return false;
        }
        DWORD type = 0;
        DWORD bytes = 0;
        LSTATUS result = RegQueryValueExW(
            key, name.c_str(), nullptr, &type, nullptr, &bytes);
        if (result != ERROR_SUCCESS || type != REG_SZ || bytes == 0 ||
            bytes % sizeof(wchar_t) != 0) {
            RegCloseKey(key);
            return false;
        }
        std::vector<wchar_t> storage(bytes / sizeof(wchar_t));
        result = RegQueryValueExW(
            key,
            name.c_str(),
            nullptr,
            &type,
            reinterpret_cast<BYTE *>(storage.data()),
            &bytes);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS || storage.empty()) return false;
        if (storage.back() == L'\0') storage.pop_back();
        value->assign(storage.begin(), storage.end());
        return true;
    }

    bool has_value(const std::wstring &name) const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER, path_.c_str(), 0, KEY_QUERY_VALUE, &key) !=
            ERROR_SUCCESS) {
            return false;
        }
        DWORD type = 0;
        const LSTATUS result = RegQueryValueExW(
            key, name.c_str(), nullptr, &type, nullptr, nullptr);
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

private:
    static std::uint64_t counter() {
        static volatile LONG64 value = 0;
        return static_cast<std::uint64_t>(InterlockedIncrement64(&value));
    }

    std::wstring path_;
};

class SystemDoctorClock final : public DoctorClock {
public:
    std::int64_t wall_utc_ms() const override {
        FILETIME file_time{};
        GetSystemTimeAsFileTime(&file_time);
        ULARGE_INTEGER value{};
        value.LowPart = file_time.dwLowDateTime;
        value.HighPart = file_time.dwHighDateTime;
        constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
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
            return static_cast<std::int64_t>(GetTickCount64()) * 1'000'000;
        }
        return static_cast<std::int64_t>(
            counter.QuadPart / frequency.QuadPart) * 1'000'000'000 +
            static_cast<std::int64_t>(
                (counter.QuadPart % frequency.QuadPart) *
                1'000'000'000 / frequency.QuadPart);
    }
};

static bool current_executable(fs::path *path) {
    if (!path) return false;
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer)) return false;
    *path = fs::path(buffer);
    return true;
}

static std::wstring quote(const std::wstring &value) {
    return L"\"" + value + L"\"";
}

class ChildProcess {
public:
    ChildProcess(const fs::path &executable, const std::wstring &arguments) {
        const std::wstring command =
            quote(executable.wstring()) + L" " + arguments;
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        created_ = CreateProcessW(
            executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_PROCESS_GROUP,
            nullptr,
            nullptr,
            &startup,
            &process_) != FALSE;
    }

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    ~ChildProcess() {
        if (created_ && process_.hProcess) {
            DWORD exit = 0;
            if (!GetExitCodeProcess(process_.hProcess, &exit) ||
                exit == STILL_ACTIVE) {
                TerminateProcess(process_.hProcess, 0xfeedd00dUL);
                WaitForSingleObject(process_.hProcess, kCleanupTimeoutMs);
            }
        }
        close();
    }

    bool created() const noexcept { return created_; }
    DWORD pid() const noexcept { return created_ ? process_.dwProcessId : 0; }
    HANDLE handle() const noexcept { return process_.hProcess; }

    bool running() const {
        if (!created_) return false;
        DWORD exit = 0;
        return GetExitCodeProcess(process_.hProcess, &exit) &&
            exit == STILL_ACTIVE;
    }

    bool wait(DWORD milliseconds) const {
        return created_ &&
            WaitForSingleObject(process_.hProcess, milliseconds) ==
                WAIT_OBJECT_0;
    }

    DWORD exit_code() const {
        DWORD exit = 0xffffffffUL;
        if (created_) GetExitCodeProcess(process_.hProcess, &exit);
        return exit;
    }

    bool terminate() {
        if (!created_ || !process_.hProcess) return false;
        if (!TerminateProcess(process_.hProcess, 0xfeedfaceUL)) return false;
        return WaitForSingleObject(
            process_.hProcess, kCleanupTimeoutMs) == WAIT_OBJECT_0;
    }

private:
    void close() {
        if (process_.hThread) CloseHandle(process_.hThread);
        if (process_.hProcess) CloseHandle(process_.hProcess);
        process_ = {};
        created_ = false;
    }

    bool created_ = false;
    PROCESS_INFORMATION process_{};
};

class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    OwnedHandle(const OwnedHandle &) = delete;
    OwnedHandle &operator=(const OwnedHandle &) = delete;

    ~OwnedHandle() {
        if (valid()) CloseHandle(handle_);
    }

    HANDLE get() const noexcept { return handle_; }

    bool valid() const noexcept {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class KillOnCloseJob {
public:
    KillOnCloseJob() {
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
        information.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job_,
                JobObjectExtendedLimitInformation,
                &information,
                sizeof(information))) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }

    ~KillOnCloseJob() {
        if (job_) CloseHandle(job_);
    }

    bool assign(const ChildProcess &child) {
        return job_ && AssignProcessToJobObject(job_, child.handle()) != FALSE;
    }

private:
    HANDLE job_ = nullptr;
};

static fs::path self_executable() {
    fs::path executable;
    if (!current_executable(&executable)) {
        throw std::runtime_error("Cannot resolve the current test executable");
    }
    return executable;
}

static std::unique_ptr<ChildProcess> start_helper(
    const std::wstring &arguments) {
    auto child = std::make_unique<ChildProcess>(
        self_executable(), arguments);
    return child->created() ? std::move(child) : nullptr;
}

static std::unique_ptr<ChildProcess> start_helper(
    const fs::path &executable,
    const std::wstring &arguments) {
    auto child = std::make_unique<ChildProcess>(executable, arguments);
    return child->created() ? std::move(child) : nullptr;
}

static std::wstring ascii_wide(const std::string &value) {
    return std::wstring(value.begin(), value.end());
}

static std::wstring owner_arguments(
    const doctor_host_test::Options &options,
    const std::string &mode) {
    std::wstring result =
        L"--doctor-test-owner " + ascii_wide(mode) + L" " +
        quote(options.root.wstring()) + L" " +
        quote(options.run_key_path);
    for (const auto &producer : options.producer_attachments) {
        result += L" " + std::to_wstring(producer.process_id) + L" " +
            std::to_wstring(
                static_cast<unsigned long long>(producer.process_creation));
    }
    return result;
}

static doctor_host_test::Options host_options(
    const OwnedTempDirectory &temporary,
    const TemporaryRegistryKey &registry) {
    doctor_host_test::Options options;
    options.root = temporary.path() / "store";
    options.build = kBuild;
    options.settings_exe_path = self_executable();
    options.run_key_path = registry.path();
    options.idle_timeout_ms = 30'000;
    return options;
}

static DoctorHostRequest command(DoctorHostCommand value) {
    DoctorHostRequest request;
    request.command = value;
    return request;
}

static DoctorHostClientResult send(
    const doctor_host_test::Options &options,
    const DoctorHostRequest &request,
    bool auto_start = false,
    DWORD timeout_ms = kHostTimeoutMs) {
    return doctor_host_test::request(
        options, request, timeout_ms, auto_start);
}

static bool wait_for_owner(const doctor_host_test::Options &options) {
    const auto deadline = GetTickCount64() + kHostTimeoutMs;
    do {
        const auto result = send(
            options, command(DoctorHostCommand::Status), false, 500);
        if (result.failure == DoctorHostClientFailure::None &&
            result.reply.failure == DoctorStoreFailure::None) {
            return true;
        }
        if (result.failure == DoctorHostClientFailure::AccessDenied ||
            result.failure == DoctorHostClientFailure::EndpointUntrusted) {
            return false;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

static bool wait_for_endpoint_unavailable(
    const doctor_host_test::Options &options) {
    const auto deadline = GetTickCount64() + kCleanupTimeoutMs;
    do {
        const auto result = send(
            options, command(DoctorHostCommand::Status), false, 250);
        if (result.failure ==
            DoctorHostClientFailure::EndpointUnavailable) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

static std::string read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

static bool write_bytes(const fs::path &path, const std::string &value) {
    std::error_code ignored;
    fs::create_directories(path.parent_path(), ignored);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

using FileSnapshot = std::map<std::string, std::string>;

static FileSnapshot snapshot_files(const fs::path &root) {
    FileSnapshot result;
    std::error_code error;
    if (!fs::exists(root, error) || !fs::is_directory(root, error)) {
        return result;
    }
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

static std::int64_t wall_now_ms() {
    SystemDoctorClock clock;
    return clock.wall_utc_ms();
}

static bool wait_for_pipe(const std::wstring &name, DWORD timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    do {
        if (WaitNamedPipeW(name.c_str(), 25)) return true;
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_SEM_TIMEOUT &&
            error != ERROR_PIPE_BUSY) return false;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}

static DoctorHostRequest mark_request(
    std::int64_t selected_ms,
    std::int64_t before_ms,
    std::int64_t after_ms) {
    auto request = command(DoctorHostCommand::Mark);
    request.mark.selected_issue_time_ms = selected_ms;
    request.mark.before_ms = before_ms;
    request.mark.after_ms = after_ms;
    return request;
}

static DoctorHostRequest prepare_request(
    std::int64_t selected_ms,
    std::int64_t before_ms,
    std::int64_t after_ms) {
    auto request = mark_request(selected_ms, before_ms, after_ms);
    request.command = DoctorHostCommand::Prepare;
    return request;
}

static DoctorHostRequest snapshot_request(
    DoctorHostCommand value, const DoctorHostSnapshot &snapshot) {
    auto request = command(value);
    request.snapshot = snapshot;
    return request;
}

static bool enable_for_test(
    const doctor_host_test::Options &options) {
    const auto result = send(options, command(DoctorHostCommand::Enable));
    return result.failure == DoctorHostClientFailure::None &&
        result.reply.failure == DoctorStoreFailure::None;
}

static bool disable_and_wait(
    const doctor_host_test::Options &options,
    ChildProcess *child) {
    const auto result = send(options, command(DoctorHostCommand::Disable));
    if (result.failure != DoctorHostClientFailure::None ||
        result.reply.failure != DoctorStoreFailure::None) {
        return false;
    }
    return !child || child->wait(kHostTimeoutMs);
}

static bool overlapped_io(
    HANDLE handle,
    bool write,
    const void *input,
    DWORD bytes,
    void *output,
    DWORD *transferred,
    DWORD timeout_ms,
    DWORD *win32_error = nullptr) {
    if (win32_error) *win32_error = ERROR_SUCCESS;
    if (transferred) *transferred = 0;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        if (win32_error) *win32_error = GetLastError();
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    const BOOL immediate = write
        ? WriteFile(
            handle, input, bytes, transferred, &overlapped)
        : ReadFile(
            handle, output, bytes, transferred, &overlapped);
    if (immediate) {
        CloseHandle(event);
        return true;
    }
    const DWORD initial_error = GetLastError();
    if (initial_error != ERROR_IO_PENDING) {
        if (win32_error) *win32_error = initial_error;
        CloseHandle(event);
        return false;
    }
    const DWORD wait = WaitForSingleObject(event, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        if (win32_error) {
            *win32_error = wait == WAIT_TIMEOUT
                ? ERROR_TIMEOUT
                : GetLastError();
        }
        CancelIoEx(handle, &overlapped);
        WaitForSingleObject(event, 100);
        CloseHandle(event);
        return false;
    }
    const BOOL complete =
        GetOverlappedResult(handle, &overlapped, transferred, FALSE);
    if (!complete && win32_error) {
        *win32_error = GetLastError();
    }
    CloseHandle(event);
    return complete != FALSE;
}

static bool connect_overlapped_pipe(
    HANDLE pipe, DWORD timeout_ms) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (connected) {
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
    if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(event, 100);
        CloseHandle(event);
        return false;
    }
    DWORD ignored = 0;
    const BOOL complete =
        GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
    CloseHandle(event);
    return complete != FALSE;
}

struct RawExchange {
    bool connected = false;
    bool written = false;
    bool read = false;
    std::vector<unsigned char> reply;
};

static RawExchange raw_exchange(
    const std::wstring &pipe_name,
    const void *request_bytes,
    DWORD request_size,
    DWORD timeout_ms = kHostTimeoutMs) {
    RawExchange result;
    if (!WaitNamedPipeW(pipe_name.c_str(), timeout_ms)) return result;
    HANDLE pipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return result;
    result.connected = true;
    DWORD written = 0;
    result.written = overlapped_io(
        pipe, true, request_bytes, request_size,
        nullptr, &written, timeout_ms);
    result.reply.resize(sizeof(DoctorHostWireReply));
    DWORD read = 0;
    result.read = result.written &&
        overlapped_io(
            pipe,
            false,
            nullptr,
            static_cast<DWORD>(result.reply.size()),
            result.reply.data(),
            &read,
            timeout_ms);
    if (result.read) {
        result.reply.resize(read);
    } else {
        result.reply.clear();
    }
    CloseHandle(pipe);
    return result;
}

static bool report_raw_pipe_stage(
    const char *case_name,
    const char *stage,
    bool ok,
    DWORD actual_win32_error) {
    if (!ok) {
        std::cerr
            << case_name
            << " stage=" << stage
            << " actualWin32Error="
            << static_cast<unsigned long>(actual_win32_error)
            << '\n';
    }
    return ok;
}

static HANDLE open_raw_pipe_client(
    const std::wstring &pipe_name,
    DWORD timeout_ms,
    DWORD *server_pid,
    DWORD *win32_error) {
    if (!server_pid) {
        if (win32_error) *win32_error = ERROR_INVALID_PARAMETER;
        return INVALID_HANDLE_VALUE;
    }
    *server_pid = 0;
    if (win32_error) *win32_error = ERROR_SUCCESS;
    if (!WaitNamedPipeW(pipe_name.c_str(), timeout_ms)) {
        if (win32_error) *win32_error = GetLastError();
        return INVALID_HANDLE_VALUE;
    }
    HANDLE pipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (win32_error) *win32_error = GetLastError();
        return INVALID_HANDLE_VALUE;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(
            pipe, &mode, nullptr, nullptr) ||
        !GetNamedPipeServerProcessId(pipe, server_pid)) {
        if (win32_error) *win32_error = GetLastError();
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

static DoctorHostWireRequest wire_request(
    const DoctorHostRequest &request) {
    DoctorHostWireRequest wire{};
    if (!doctor_host_test::encode_wire_request(request, &wire)) {
        throw std::runtime_error("Cannot encode a doctor host test request");
    }
    return wire;
}

static bool decode_reply(
    const std::vector<unsigned char> &bytes,
    DoctorHostWireReply *reply) {
    if (!reply ||
        bytes.size() != sizeof(DoctorHostWireReply)) {
        return false;
    }
    std::memcpy(reply, bytes.data(), sizeof(*reply));
    return reply->header.magic == kDoctorHostProtocolMagic &&
        reply->header.version == kDoctorHostProtocolVersion &&
        reply->header.total_bytes == sizeof(DoctorHostWireReply);
}

static bool contains_bytes(
    const std::vector<unsigned char> &haystack,
    const void *needle,
    std::size_t needle_bytes) {
    const auto *begin = haystack.data();
    if (!begin || !needle || needle_bytes == 0 ||
        needle_bytes > haystack.size()) {
        return false;
    }
    for (std::size_t offset = 0;
            offset + needle_bytes <= haystack.size();
            ++offset) {
        if (std::memcmp(
                begin + offset, needle, needle_bytes) == 0) {
            return true;
        }
    }
    return false;
}

static bool same_sid(PSID left, PSID right) {
    return left && right && EqualSid(left, right) != FALSE;
}

struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    ~PipeSecurity() {
        if (descriptor) LocalFree(descriptor);
    }
};

static bool create_test_pipe_security(PipeSecurity *security) {
    if (!security) return false;
    std::wstring user_sid;
    if (!detail::own_user_sid(&user_sid)) return false;
    const std::wstring sddl =
        L"D:P(A;;0x12019f;;;" + user_sid +
        L")(A;;0x12019f;;;SY)S:(ML;;NW;;;ME)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &security->descriptor,
            nullptr)) {
        return false;
    }
    security->attributes.nLength = sizeof(security->attributes);
    security->attributes.lpSecurityDescriptor = security->descriptor;
    security->attributes.bInheritHandle = FALSE;
    return true;
}

static int disconnect_server_helper() {
    PipeSecurity security;
    if (!create_test_pipe_security(&security)) return 2;
    std::wstring user_sid;
    if (!detail::own_user_sid(&user_sid)) return 2;
    const auto pipe_name =
        DoctorHostEndpoint::pipe_name(user_sid);
    const HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE |
            FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,
        static_cast<DWORD>(sizeof(DoctorHostWireReply)),
        static_cast<DWORD>(sizeof(DoctorHostWireRequest)),
        0,
        &security.attributes);
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    if (!connect_overlapped_pipe(pipe, kHostTimeoutMs)) {
        CloseHandle(pipe);
        return 4;
    }
    std::vector<unsigned char> request(
        sizeof(DoctorHostWireRequest));
    DWORD read = 0;
    const bool received = overlapped_io(
        pipe,
        false,
        nullptr,
        static_cast<DWORD>(request.size()),
        request.data(),
        &read,
        kHostTimeoutMs);
    CloseHandle(pipe);
    return received ? 0 : 5;
}

static int low_integrity_client_helper() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_ADJUST_DEFAULT,
            &token)) {
        return 2;
    }
    PSID low_sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &low_sid) || !low_sid) {
        CloseHandle(token);
        return 3;
    }
    std::vector<unsigned char> label_storage(
        sizeof(TOKEN_MANDATORY_LABEL) + GetLengthSid(low_sid));
    auto *label =
        reinterpret_cast<TOKEN_MANDATORY_LABEL *>(label_storage.data());
    label->Label.Attributes = SE_GROUP_INTEGRITY;
    label->Label.Sid = reinterpret_cast<PSID>(
        label_storage.data() + sizeof(TOKEN_MANDATORY_LABEL));
    if (!CopySid(
            GetLengthSid(low_sid),
            label->Label.Sid,
            low_sid)) {
        LocalFree(low_sid);
        CloseHandle(token);
        return 4;
    }
    const BOOL lowered = SetTokenInformation(
        token,
        TokenIntegrityLevel,
        label,
        static_cast<DWORD>(label_storage.size()));
    LocalFree(low_sid);
    if (!lowered) {
        CloseHandle(token);
        return 5;
    }
    const auto identity = detail::token_identity(token);
    CloseHandle(token);
    if (identity.result_hr != S_OK ||
        identity.integrity_rid != SECURITY_MANDATORY_LOW_RID) {
        return 6;
    }

    std::wstring user_sid;
    if (!detail::own_user_sid(&user_sid)) return 7;
    const auto pipe_name = DoctorHostEndpoint::pipe_name(user_sid);
    const HANDLE pipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        return 8;
    }
    const DWORD error = GetLastError();
    return error == ERROR_ACCESS_DENIED ||
            error == HRESULT_CODE(E_ACCESSDENIED)
        ? 0
        : 9;
}

static doctor_host_test::Options client_options_from_arguments(
    char **argv) {
    doctor_host_test::Options result;
    result.root = fs::path(argv[2]);
    result.run_key_path = ascii_wide(argv[3]);
    result.settings_exe_path = self_executable();
    result.build = kBuild;
    result.idle_timeout_ms = 30'000;
    return result;
}

inline constexpr std::uint32_t kTestRpcResultMagic =
    0x4c4b5254UL;
inline constexpr std::uint32_t kTestRpcResultVersion = 1;
inline constexpr std::uint64_t kHeldPrepareTimeoutMs = 20'000;

struct TestRpcResult {
    std::uint32_t magic = kTestRpcResultMagic;
    std::uint32_t version = kTestRpcResultVersion;
    DoctorHostClientResult result{};
};

static_assert(
    std::is_trivially_copyable<DoctorHostClientResult>::value,
    "Doctor host test RPC result must be trivially copyable");
static_assert(
    std::is_trivially_copyable<TestRpcResult>::value,
    "Doctor host test RPC envelope must be trivially copyable");
static_assert(
    sizeof(TestRpcResult) < 1024,
    "Doctor host test RPC envelope must stay bounded");

static bool valid_test_rpc_path(
    const fs::path &path, const std::wstring &prefix) {
    if (path.wstring().size() >
        kDoctorHostMaximumPathCodeUnits) {
        return false;
    }
    const auto canonical = doctor_host_detail::canonical_path(path);
    const auto executable = self_executable();
    const auto executable_parent =
        doctor_host_detail::canonical_path(executable.parent_path());
    return !canonical.empty() &&
        canonical.is_absolute() &&
        doctor_host_detail::same_path(
            canonical.parent_path(), executable_parent) &&
        canonical.filename().wstring().rfind(prefix, 0) == 0;
}

static bool write_test_rpc_result(
    const fs::path &path,
    const DoctorHostClientResult &result) {
    if (!valid_test_rpc_path(
            path, L".liu-kai-doctor-rpc-")) {
        return false;
    }
    const TestRpcResult envelope{
        kTestRpcResultMagic,
        kTestRpcResultVersion,
        result,
    };
    const std::string bytes(
        reinterpret_cast<const char *>(&envelope),
        sizeof(envelope));
    auto stage = path;
    stage += L".tmp-" +
        std::to_wstring(static_cast<unsigned long>(
            GetCurrentProcessId()));
    if (!write_bytes(stage, bytes)) {
        DeleteFileW(stage.c_str());
        return false;
    }
    if (!MoveFileExW(
            stage.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(stage.c_str());
        return false;
    }
    return true;
}

static int owner_helper(int argc, char **argv) {
    if (argc < 5) return 2;
    doctor_host_test::Options options;
    options.root = fs::path(argv[3]);
    options.run_key_path = ascii_wide(argv[4]);
    options.settings_exe_path = self_executable();
    options.build = kBuild;
    options.idle_timeout_ms = 30'000;
    for (int index = 5; index + 1 < argc; index += 2) {
        DoctorHostTestProducer producer{};
        producer.process_id = static_cast<DWORD>(std::strtoul(
            argv[index], nullptr, 10));
        producer.process_creation = std::strtoull(
            argv[index + 1], nullptr, 10);
        options.producer_attachments.push_back(producer);
    }
    return std::string(argv[2]) == "collector"
        ? doctor_host_test::run_collector(options)
        : doctor_host_test::run_once(options);
}

static int prepare_client_helper(int argc, char **argv) {
    if (argc != 7) return 2;
    auto options = client_options_from_arguments(argv);
    const auto selected = std::stoll(argv[4]);
    const auto before = std::stoll(argv[5]);
    const auto after = std::stoll(argv[6]);
    const auto result = doctor_host_test::request(
        options,
        prepare_request(selected, before, after),
        kHostTimeoutMs,
        false);
    return result.failure == DoctorHostClientFailure::None &&
            result.reply.failure == DoctorStoreFailure::None
        ? 0
        : 3;
}

static int request_client_helper(int argc, char **argv) {
    if (argc != 7) return 2;
    auto options = client_options_from_arguments(argv);
    const auto response_path = fs::path(argv[4]);
    const std::string command = argv[5];
    const auto timeout_ms =
        static_cast<DWORD>(std::strtoul(argv[6], nullptr, 10));
    if (timeout_ms == 0 || timeout_ms > kHostTimeoutMs) return 3;
    DoctorHostRequest request;
    if (command == "status") {
        request.command = DoctorHostCommand::Status;
    } else if (command == "enable") {
        request.command = DoctorHostCommand::Enable;
    } else if (command == "disable") {
        request.command = DoctorHostCommand::Disable;
    } else {
        return 4;
    }
    const auto result = doctor_host_test::request(
        options, request, timeout_ms, false);
    return write_test_rpc_result(response_path, result) ? 0 : 5;
}

static int held_prepare_client_helper(int argc, char **argv) {
    if (argc != 9) return 2;
    auto options = client_options_from_arguments(argv);
    const auto response_path = fs::path(argv[4]);
    const auto release_path = fs::path(argv[5]);
    if (!valid_test_rpc_path(
            release_path, L".liu-kai-doctor-release-")) {
        return 3;
    }
    const auto selected = std::stoll(argv[6]);
    const auto before = std::stoll(argv[7]);
    const auto after = std::stoll(argv[8]);
    const auto result = doctor_host_test::request(
        options,
        prepare_request(selected, before, after),
        kHostTimeoutMs,
        false);
    if (!write_test_rpc_result(response_path, result)) return 4;
    if (result.failure != DoctorHostClientFailure::None ||
        result.reply.failure != DoctorStoreFailure::None) {
        return 5;
    }
    const auto deadline =
        GetTickCount64() + kHeldPrepareTimeoutMs;
    do {
        if (GetFileAttributesW(release_path.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            return 0;
        }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return 6;
}

static int auto_start_client_helper(int argc, char **argv) {
    if (argc != 4) return 2;
    auto options = client_options_from_arguments(argv);
    auto status = doctor_host_test::request(
        options,
        command(DoctorHostCommand::Status),
        kHostTimeoutMs,
        true);
    if (status.failure != DoctorHostClientFailure::None ||
        status.reply.failure != DoctorStoreFailure::None) {
        return 3;
    }
    const auto stopped = doctor_host_test::request(
        options,
        command(DoctorHostCommand::Disable),
        kHostTimeoutMs,
        false);
    if (stopped.failure != DoctorHostClientFailure::None ||
        stopped.reply.failure != DoctorStoreFailure::None ||
        !wait_for_endpoint_unavailable(options)) {
        return 4;
    }
    return 0;
}

static int test_normal_medium_non_elevated_token() {
    const auto identity = detail::own_token_identity();
    CHECK(identity.result_hr == S_OK);
    CHECK(identity.integrity_rid == SECURITY_MANDATORY_MEDIUM_RID);
    CHECK(identity.app_container == 0);
    HANDLE token = nullptr;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    TOKEN_ELEVATION elevation{};
    DWORD copied = 0;
    const BOOL elevation_read = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &copied);
    CloseHandle(token);
    CHECK(elevation_read);
    CHECK(elevation.TokenIsElevated == 0);
    return 0;
}

static int test_off_round_trip_never_enables() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    auto status = send(options, command(DoctorHostCommand::Status));
    CHECK(status.failure == DoctorHostClientFailure::None);
    CHECK(status.reply.failure == DoctorStoreFailure::None);
    CHECK(!status.reply.status.enabled);
    CHECK(!status.reply.status.collecting);

    const std::int64_t selected = wall_now_ms() - 60'000;
    auto marked = send(
        options, mark_request(selected, 17'000, 9'000));
    CHECK(marked.failure == DoctorHostClientFailure::None);
    CHECK(marked.reply.failure == DoctorStoreFailure::None);
    CHECK(marked.reply.mark.epoch == status.reply.status.epoch);
    CHECK(marked.reply.mark.selected_issue_time_ms == selected);
    CHECK(marked.reply.mark.before_ms == 17'000);
    CHECK(marked.reply.mark.after_ms == 9'000);
    CHECK(marked.reply.mark.action_time_ms >= selected);
    CHECK(marked.reply.mark.action_time_ms != selected);
    CHECK(!marked.reply.status.enabled);

    auto invalid_mark = send(
        options, mark_request(-1, 17'000, 9'000));
    CHECK(invalid_mark.failure == DoctorHostClientFailure::None);
    CHECK(invalid_mark.reply.failure ==
        DoctorStoreFailure::InvalidSchema);
    CHECK(!invalid_mark.reply.status.enabled);

    CHECK(disable_and_wait(options, owner.get()));
    owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    auto prepare_from_mark = command(DoctorHostCommand::Prepare);
    prepare_from_mark.mark = marked.reply.mark;
    auto prepared = send(options, prepare_from_mark);
    CHECK(prepared.failure == DoctorHostClientFailure::None);
    CHECK(prepared.reply.failure == DoctorStoreFailure::None);
    CHECK(prepared.reply.snapshot.epoch ==
        status.reply.status.epoch);
    CHECK(prepared.reply.snapshot.handle != 0);

    auto preview = send(
        options,
        snapshot_request(
            DoctorHostCommand::Preview, prepared.reply.snapshot));
    CHECK(preview.failure == DoctorHostClientFailure::None);
    CHECK(preview.reply.failure == DoctorStoreFailure::None);
    CHECK(preview.reply.preview.epoch ==
        prepared.reply.snapshot.epoch);
    CHECK(preview.reply.preview.record_count == 0);
    CHECK(!preview.reply.status.enabled);

    const fs::path destination =
        temporary.path() / L"selected-\u6e2c\u8a66.zip";
    auto save_request_value =
        snapshot_request(
            DoctorHostCommand::Save, prepared.reply.snapshot);
    save_request_value.destination = destination.wstring();
    auto saved = send(options, save_request_value);
    CHECK(saved.failure == DoctorHostClientFailure::None);
    CHECK(saved.reply.failure == DoctorStoreFailure::None);
    CHECK(fs::file_size(destination) > 0);
    CHECK(read_bytes(destination).find(
        "\"actionTimeMs\":" +
            std::to_string(marked.reply.mark.action_time_ms)) !=
        std::string::npos);
    CHECK(!saved.reply.status.enabled);

    auto second = send(
        options, prepare_request(selected, 1'000, 1'000));
    CHECK(second.failure == DoctorHostClientFailure::None);
    CHECK(second.reply.failure == DoctorStoreFailure::None);
    auto cancelled = send(
        options,
        snapshot_request(
            DoctorHostCommand::Cancel, second.reply.snapshot));
    CHECK(cancelled.failure == DoctorHostClientFailure::None);
    CHECK(cancelled.reply.failure == DoctorStoreFailure::None);
    CHECK(!cancelled.reply.status.enabled);

    const auto old_epoch = cancelled.reply.status.epoch;
    auto cleared = send(
        options, command(DoctorHostCommand::Clear));
    CHECK(cleared.failure == DoctorHostClientFailure::None);
    CHECK(cleared.reply.failure == DoctorStoreFailure::None);
    CHECK(cleared.reply.status.epoch == old_epoch + 1);
    CHECK(!cleared.reply.status.enabled);
    CHECK(!cleared.reply.status.collecting);
    CHECK(!registry.has_value(kDoctorHostRunValueName));

    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int test_enable_disable_uses_only_temporary_run_key() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    CHECK(registry.set_value(L"OtherVendorSentinel", L"keep-me"));

    auto disabled_collector = start_helper(
        owner_arguments(options, "collector"));
    CHECK(disabled_collector);
    CHECK(disabled_collector->wait(kHostTimeoutMs));
    CHECK(disabled_collector->exit_code() ==
        static_cast<DWORD>(DoctorHostExitCode::Success));
    CHECK(!registry.has_value(kDoctorHostRunValueName));

    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    CHECK(enable_for_test(options));
    std::wstring run_value;
    CHECK(registry.query_value(kDoctorHostRunValueName, &run_value));
    CHECK(run_value ==
        quote(options.settings_exe_path.wstring()) +
            L" --doctor-collector");
    CHECK(!registry.has_value(L"ArbitraryRoot"));

    CHECK(registry.query_value(
        L"OtherVendorSentinel", &run_value));
    CHECK(run_value == L"keep-me");
    CHECK(disable_and_wait(options, owner.get()));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    CHECK(registry.query_value(
        L"OtherVendorSentinel", &run_value));
    CHECK(run_value == L"keep-me");

    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        options.root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    CHECK(!store->status().enabled);
    return 0;
}

static int test_client_auto_starts_same_exe_once_owner() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    const std::wstring arguments =
        L"--doctor-test-auto-start-client " +
        quote(options.root.wstring()) + L" " +
        quote(options.run_key_path);
    auto client = start_helper(arguments);
    CHECK(client);
    KillOnCloseJob job;
    CHECK(job.assign(*client));
    CHECK(client->wait(2 * kHostTimeoutMs));
    CHECK(client->exit_code() == 0);
    CHECK(wait_for_endpoint_unavailable(options));

    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        options.root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    CHECK(!store->status().enabled);
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    return 0;
}

static int test_same_user_launch_has_one_non_waiting_writer() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto first = start_helper(owner_arguments(options, "once"));
    CHECK(first);
    auto second = start_helper(owner_arguments(options, "once"));
    CHECK(second);
    CHECK(wait_for_owner(options));
    Sleep(100);

    const bool first_running = first->running();
    const bool second_running = second->running();
    CHECK(first_running != second_running);
    ChildProcess *loser = first_running ? second.get() : first.get();
    ChildProcess *winner = first_running ? first.get() : second.get();
    CHECK(loser->wait(kHostTimeoutMs));
    CHECK(loser->exit_code() ==
        static_cast<DWORD>(DoctorHostExitCode::OwnerConflict));

    const auto collector_start = GetTickCount64();
    auto collector = start_helper(
        owner_arguments(options, "collector"));
    CHECK(collector);
    CHECK(collector->wait(2'000));
    CHECK(GetTickCount64() - collector_start < 2'000);
    CHECK(collector->exit_code() ==
        static_cast<DWORD>(DoctorHostExitCode::OwnerConflict));
    CHECK(winner->running());
    CHECK(disable_and_wait(options, winner));
    return 0;
}

static bool check_protected_dacl(HANDLE pipe) {
    PSECURITY_DESCRIPTOR security = nullptr;
    if (GetSecurityInfo(
            pipe,
            SE_KERNEL_OBJECT,
            OWNER_SECURITY_INFORMATION |
                DACL_SECURITY_INFORMATION |
                LABEL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &security) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    do {
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(
                security, &control, &revision) ||
            (control & SE_DACL_PROTECTED) == 0) {
            break;
        }

        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) break;
        std::vector<unsigned char> user_storage;
        const HRESULT user_result =
            detail::token_sid_copy(token, &user_storage);
        CloseHandle(token);
        if (FAILED(user_result) || user_storage.empty()) break;
        PSID system_sid = nullptr;
        if (!ConvertStringSidToSidW(
                L"S-1-5-18", &system_sid) || !system_sid) {
            break;
        }

        PSID owner = nullptr;
        BOOL defaulted = FALSE;
        if (!GetSecurityDescriptorOwner(
                security, &owner, &defaulted) ||
            !same_sid(owner, reinterpret_cast<PSID>(user_storage.data()))) {
            LocalFree(system_sid);
            break;
        }

        PACL dacl = nullptr;
        BOOL present = FALSE;
        if (!GetSecurityDescriptorDacl(
                security, &present, &dacl, &defaulted) ||
            !present || !dacl || dacl->AceCount != 2) {
            LocalFree(system_sid);
            break;
        }
        bool user_seen = false;
        bool system_seen = false;
        for (DWORD index = 0; index < dacl->AceCount; ++index) {
            void *raw_ace = nullptr;
            if (!GetAce(dacl, index, &raw_ace) || !raw_ace) {
                ok = false;
                break;
            }
            const auto *header =
                static_cast<ACE_HEADER *>(raw_ace);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                ok = false;
                break;
            }
            const auto *ace =
                static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
            const PSID trustee =
                reinterpret_cast<PSID>(
                    const_cast<DWORD *>(&ace->SidStart));
            const bool user =
                same_sid(
                    trustee,
                    reinterpret_cast<PSID>(user_storage.data()));
            const bool system = same_sid(trustee, system_sid);
            if ((!user && !system) ||
                (ace->Mask & (FILE_GENERIC_READ | FILE_GENERIC_WRITE)) ==
                    0) {
                ok = false;
                break;
            }
            user_seen = user_seen || user;
            system_seen = system_seen || system;
            ok = index + 1 == dacl->AceCount;
        }
        if (!user_seen || !system_seen) ok = false;

        PACL labels = nullptr;
        present = FALSE;
        if (!ok ||
            !GetSecurityDescriptorSacl(
                security, &present, &labels, &defaulted) ||
            !present || !labels || labels->AceCount != 1) {
            ok = false;
        } else {
            void *raw_label = nullptr;
            if (!GetAce(labels, 0, &raw_label)) {
                ok = false;
            } else {
                const auto *header =
                    static_cast<ACE_HEADER *>(raw_label);
                if (header->AceType !=
                    SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
                    ok = false;
                } else {
                    const auto *label =
                        static_cast<
                            SYSTEM_MANDATORY_LABEL_ACE *>(raw_label);
                    const PSID sid =
                        reinterpret_cast<PSID>(
                            const_cast<DWORD *>(&label->SidStart));
                    const UCHAR count = *GetSidSubAuthorityCount(sid);
                    ok = ok && count != 0 &&
                        *GetSidSubAuthority(sid, count - 1) ==
                            SECURITY_MANDATORY_MEDIUM_RID &&
                        (label->Mask &
                            SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) != 0;
                }
            }
        }
        LocalFree(system_sid);
    } while (false);
    LocalFree(security);
    return ok;
}

static int test_pipe_security_and_low_integrity_denial() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    const auto pipe_name = DoctorHostEndpoint::pipe_name(user_sid);
    CHECK(wait_for_pipe(pipe_name, kHostTimeoutMs));
    HANDLE pipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    CHECK(pipe != INVALID_HANDLE_VALUE);
    DWORD server_pid = 0;
    const BOOL pid_read =
        GetNamedPipeServerProcessId(pipe, &server_pid);
    const bool security_ok = check_protected_dacl(pipe);
    CloseHandle(pipe);
    CHECK(pid_read);
    CHECK(server_pid == owner->pid());
    CHECK(security_ok);

    auto low = start_helper(L"--doctor-test-low-client");
    CHECK(low);
    CHECK(low->wait(kHostTimeoutMs));
    CHECK(low->exit_code() == 0);
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int test_endpoint_path_rejects_copied_image_spoof() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto copied_executable = temporary.path() / "copied" /
        self_executable().filename();
    fs::create_directories(copied_executable.parent_path());
    CHECK(fs::copy_file(
        self_executable(),
        copied_executable,
        fs::copy_options::overwrite_existing));

    auto spoof = start_helper(
        copied_executable, owner_arguments(options, "once"));
    CHECK(spoof);
    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    const auto pipe_name = DoctorHostEndpoint::pipe_name(user_sid);
    CHECK(wait_for_pipe(pipe_name, kHostTimeoutMs));

    const auto result = send(
        options, command(DoctorHostCommand::Status), false, 2'000);
    CHECK(result.failure ==
        DoctorHostClientFailure::EndpointUntrusted);
    CHECK(spoof->running());
    CHECK(spoof->terminate());
    return 0;
}

static int test_protocol_rejects_malformed_frames_without_mutation() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    const auto before = snapshot_files(options.root);
    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    const auto pipe_name = DoctorHostEndpoint::pipe_name(user_sid);

    auto invalid = wire_request(
        command(DoctorHostCommand::Status));
    invalid.header.version =
        static_cast<std::uint16_t>(
            kDoctorHostProtocolVersion + 1);
    auto exchange = raw_exchange(
        pipe_name, &invalid, sizeof(invalid));
    CHECK(exchange.connected && exchange.written);
    DoctorHostWireReply reply{};
    CHECK(exchange.read && decode_reply(exchange.reply, &reply));
    CHECK(reply.protocol_failure !=
        DoctorHostProtocolFailure::None);
    CHECK(snapshot_files(options.root) == before);

    invalid = wire_request(command(DoctorHostCommand::Status));
    invalid.command = 0x7fff'fffeU;
    exchange = raw_exchange(
        pipe_name, &invalid, sizeof(invalid));
    CHECK(exchange.connected && exchange.written);
    CHECK(exchange.read && decode_reply(exchange.reply, &reply));
    CHECK(reply.protocol_failure !=
        DoctorHostProtocolFailure::None);
    CHECK(snapshot_files(options.root) == before);

    invalid = wire_request(command(DoctorHostCommand::Status));
    invalid.header.total_bytes += 1;
    std::vector<unsigned char> oversized(
        sizeof(invalid) + 1, 0);
    std::memcpy(oversized.data(), &invalid, sizeof(invalid));
    exchange = raw_exchange(
        pipe_name,
        oversized.data(),
        static_cast<DWORD>(oversized.size()));
    CHECK(exchange.connected && exchange.written);
    CHECK(snapshot_files(options.root) == before);

    invalid = wire_request(command(DoctorHostCommand::Status));
    invalid.command = static_cast<std::uint32_t>(DoctorHostCommand::Save);
    invalid.snapshot.handle = 1;
    invalid.destination_code_units =
        kDoctorHostMaximumPathCodeUnits + 1;
    exchange = raw_exchange(
        pipe_name, &invalid, sizeof(invalid));
    CHECK(exchange.connected && exchange.written);
    if (exchange.read) {
        CHECK(decode_reply(exchange.reply, &reply));
        CHECK(reply.protocol_failure !=
            DoctorHostProtocolFailure::None);
    }
    CHECK(snapshot_files(options.root) == before);

    for (const auto lifecycle : {
            DoctorHostCommand::StopForUpgrade,
            DoctorHostCommand::StopForRemoval,
            DoctorHostCommand::ResumeForUpgrade}) {
        invalid = wire_request(command(lifecycle));
        invalid.mark.epoch = 1;
        exchange = raw_exchange(
            pipe_name, &invalid, sizeof(invalid));
        CHECK(exchange.connected && exchange.written && exchange.read);
        CHECK(decode_reply(exchange.reply, &reply));
        CHECK(reply.protocol_failure ==
            DoctorHostProtocolFailure::Snapshot);
        CHECK(snapshot_files(options.root) == before);
    }

    const std::vector<unsigned char> truncated(sizeof(std::uint32_t), 0);
    exchange = raw_exchange(
        pipe_name,
        truncated.data(),
        static_cast<DWORD>(truncated.size()),
        1'000);
    CHECK(exchange.connected && exchange.written);
    CHECK(snapshot_files(options.root) == before);

    const auto status = send(
        options, command(DoctorHostCommand::Status));
    CHECK(status.failure == DoctorHostClientFailure::None);
    CHECK(status.reply.failure == DoctorStoreFailure::None);
    CHECK(!status.reply.status.enabled);
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int seed_history(const fs::path &root) {
    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    CHECK(store->enable().ok());
    DoctorEventRecord event{};
    event.producer_id = 71;
    event.session_id = 1;
    event.epoch = store->status().epoch;
    event.operation_id = 901;
    event.sequence = 1;
    event.monotonic_ns = clock.monotonic_ns();
    event.wall_utc_ms = clock.wall_utc_ms();
    event.event_code = DoctorEventCode::KeyDown;
    event.callback_source = DoctorCallbackSource::KeyAction;
    event.scope = DoctorInputScope::Ordinary;
    event.phase = DoctorEventPhase::Composition;
    event.result_code = DoctorResultCode::Ok;
    CHECK(store->append(event).ok());
    CHECK(store->disable().ok());
    store.reset();
    return 0;
}

static int count_records(
    const fs::path &root, std::uint64_t *count) {
    if (!count) return 1;
    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    const auto records = store->read_records();
    CHECK(records.ok());
    *count = static_cast<std::uint64_t>(records.value.size());
    return 0;
}

static int test_disable_preserves_live_preview_until_save_or_cancel() {
    for (const bool save : {true, false}) {
        OwnedTempDirectory temporary;
        TemporaryRegistryKey registry;
        auto options = host_options(temporary, registry);
        CHECK(seed_history(options.root) == 0);
        auto owner = start_helper(owner_arguments(options, "once"));
        CHECK(owner && wait_for_owner(options));
        CHECK(enable_for_test(options));
        const auto prepared = send(
            options, prepare_request(wall_now_ms(), 5'000, 1'000));
        CHECK(prepared.failure == DoctorHostClientFailure::None);
        CHECK(prepared.reply.failure == DoctorStoreFailure::None);

        const auto disabled = send(options, command(DoctorHostCommand::Disable));
        CHECK(disabled.failure == DoctorHostClientFailure::None);
        CHECK(disabled.reply.failure == DoctorStoreFailure::None);
        CHECK(!disabled.reply.status.enabled && !disabled.reply.status.collecting);
        CHECK(owner->running());
        const auto preview = send(options, snapshot_request(
            DoctorHostCommand::Preview, prepared.reply.snapshot));
        CHECK(preview.failure == DoctorHostClientFailure::None);
        CHECK(preview.reply.failure == DoctorStoreFailure::None);
        CHECK(preview.reply.preview.record_count == 1);

        auto finish = snapshot_request(
            save ? DoctorHostCommand::Save : DoctorHostCommand::Cancel,
            prepared.reply.snapshot);
        const auto destination = temporary.path() / "saved.zip";
        if (save) finish.destination = destination.wstring();
        const auto finished = send(options, finish);
        CHECK(finished.failure == DoctorHostClientFailure::None);
        CHECK(finished.reply.failure == DoctorStoreFailure::None);
        CHECK(owner->wait(kHostTimeoutMs));
        CHECK(fs::exists(destination) == save);
        std::uint64_t count = 0;
        CHECK(count_records(options.root, &count) == 0 && count == 1);
    }
    return 0;
}

static int test_upgrade_stop_cancels_preview_and_preserves_consent() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    CHECK(seed_history(options.root) == 0);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    CHECK(enable_for_test(options));

    const auto prepared = send(
        options, prepare_request(wall_now_ms(), 5'000, 1'000));
    CHECK(prepared.failure == DoctorHostClientFailure::None);
    CHECK(prepared.reply.failure == DoctorStoreFailure::None);
    const auto foreign_file = options.root / L"foreign.keep";
    CHECK(write_bytes(foreign_file, "do not delete"));
    CHECK(registry.set_value(L"OtherVendorSentinel", L"keep-me"));

    const auto stopped = doctor_host_test::maintenance_request(
        options,
        DoctorHostMaintenanceAction::StopForUpgrade,
        kHostTimeoutMs);
    CHECK(stopped.failure == DoctorHostClientFailure::None);
    CHECK(stopped.reply.failure == DoctorStoreFailure::None);
    CHECK(stopped.reply.status.enabled);
    CHECK(owner->wait(kHostTimeoutMs));
    CHECK(wait_for_endpoint_unavailable(options));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    std::wstring foreign_value;
    CHECK(registry.query_value(
        L"OtherVendorSentinel", &foreign_value));
    CHECK(foreign_value == L"keep-me");
    CHECK(read_bytes(foreign_file) == "do not delete");
    CHECK(snapshot_files(options.root / L"temp").empty());

    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        options.root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    CHECK(store->status().enabled);
    CHECK(store->status().temp_bytes == 0);
    const auto records = store->read_records();
    CHECK(records.ok());
    CHECK(records.value.size() == 1);
    return 0;
}

static int test_removal_stop_cancels_preview_and_preserves_consent() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    CHECK(seed_history(options.root) == 0);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    CHECK(enable_for_test(options));

    const auto prepared = send(
        options, prepare_request(wall_now_ms(), 5'000, 1'000));
    CHECK(prepared.failure == DoctorHostClientFailure::None);
    CHECK(prepared.reply.failure == DoctorStoreFailure::None);
    const auto foreign_file = options.root / L"foreign.keep";
    CHECK(write_bytes(foreign_file, "do not delete"));
    CHECK(registry.set_value(L"OtherVendorSentinel", L"keep-me"));

    const auto stopped = doctor_host_test::maintenance_request(
        options,
        DoctorHostMaintenanceAction::StopForRemoval,
        kHostTimeoutMs);
    CHECK(stopped.failure == DoctorHostClientFailure::None);
    CHECK(stopped.reply.failure == DoctorStoreFailure::None);
    CHECK(stopped.reply.status.enabled);
    CHECK(owner->wait(kHostTimeoutMs));
    CHECK(wait_for_endpoint_unavailable(options));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    std::wstring foreign_value;
    CHECK(registry.query_value(
        L"OtherVendorSentinel", &foreign_value));
    CHECK(foreign_value == L"keep-me");
    CHECK(read_bytes(foreign_file) == "do not delete");
    CHECK(snapshot_files(options.root / L"temp").empty());

    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        options.root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    CHECK(store->status().enabled);
    CHECK(store->status().temp_bytes == 0);
    const auto records = store->read_records();
    CHECK(records.ok());
    CHECK(records.value.size() == 1);
    return 0;
}

static int test_pending_preview_is_recovered_after_ui_process_exit() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    CHECK(seed_history(options.root) == 0);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    const std::int64_t selected = wall_now_ms();
    const std::wstring arguments =
        L"--doctor-test-prepare-client " +
        quote(options.root.wstring()) + L" " +
        quote(options.run_key_path) + L" " +
        std::to_wstring(selected) + L" 1000 1000";
    auto crashing_ui = start_helper(arguments);
    CHECK(crashing_ui);
    CHECK(crashing_ui->wait(kHostTimeoutMs));
    CHECK(crashing_ui->exit_code() == 0);

    DoctorHostClientResult recovered;
    const auto deadline = GetTickCount64() + kHostTimeoutMs;
    do {
        recovered = send(
            options, prepare_request(selected, 1'000, 1'000));
        if (recovered.failure == DoctorHostClientFailure::None &&
            recovered.reply.failure == DoctorStoreFailure::None) {
            break;
        }
        CHECK(recovered.failure == DoctorHostClientFailure::None);
        CHECK(recovered.reply.failure ==
            DoctorStoreFailure::ExportExists);
        Sleep(25);
    } while (GetTickCount64() < deadline);
    CHECK(recovered.reply.failure == DoctorStoreFailure::None);
    const auto cancelled = send(
        options,
        snapshot_request(
            DoctorHostCommand::Cancel, recovered.reply.snapshot));
    CHECK(cancelled.failure == DoctorHostClientFailure::None);
    CHECK(cancelled.reply.failure == DoctorStoreFailure::None);
    CHECK(disable_and_wait(options, owner.get()));

    std::uint64_t records = 0;
    CHECK(count_records(options.root, &records) == 0);
    CHECK(records == 1);
    return 0;
}

static int test_server_crash_recovers_abandoned_preview() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    CHECK(seed_history(options.root) == 0);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    const std::int64_t selected = wall_now_ms();
    auto prepared = send(
        options, prepare_request(selected, 2'000, 2'000));
    CHECK(prepared.failure == DoctorHostClientFailure::None);
    CHECK(prepared.reply.failure == DoctorStoreFailure::None);
    const auto stale = prepared.reply.snapshot;
    CHECK(owner->terminate());

    auto replacement = start_helper(
        owner_arguments(options, "once"));
    CHECK(replacement);
    CHECK(wait_for_owner(options));
    auto stale_preview = send(
        options, snapshot_request(DoctorHostCommand::Preview, stale));
    CHECK(stale_preview.failure == DoctorHostClientFailure::None);
    CHECK(stale_preview.reply.failure != DoctorStoreFailure::None);

    auto recovered = send(
        options, prepare_request(selected, 2'000, 2'000));
    CHECK(recovered.failure == DoctorHostClientFailure::None);
    CHECK(recovered.reply.failure == DoctorStoreFailure::None);
    auto cancelled = send(
        options,
        snapshot_request(
            DoctorHostCommand::Cancel, recovered.reply.snapshot));
    CHECK(cancelled.failure == DoctorHostClientFailure::None);
    CHECK(cancelled.reply.failure == DoctorStoreFailure::None);
    CHECK(disable_and_wait(options, replacement.get()));

    std::uint64_t records = 0;
    CHECK(count_records(options.root, &records) == 0);
    CHECK(records == 1);
    return 0;
}

static EventRecord queue_event(
    ScopeClass scope,
    Event event,
    std::uint64_t operation_id) {
    EventRecord result{};
    result.operation_id = operation_id;
    result.monotonic_ns = static_cast<std::uint64_t>(
        SystemDoctorClock{}.monotonic_ns());
    result.schema_version = kSchemaVersion;
    result.event_code = static_cast<std::uint32_t>(event);
    result.callback_source =
        static_cast<std::uint32_t>(DoctorCallbackSource::KeyAction);
    result.scope = static_cast<std::uint32_t>(scope);
    result.result_code = S_OK;
    result.duration_us = 10;
    return result;
}

static int test_attached_producer_drains_only_ordinary_events_while_on() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    ProcessProducer producer;
    CHECK(producer.ensure());
    options.producer_attachments.push_back(
        DoctorHostTestProducer{
            GetCurrentProcessId(), producer.process_creation()});

    ScopeObservation sensitive{};
    sensitive.query_result = S_OK;
    sensitive.context_available = true;
    sensitive.token_count = 4;
    sensitive.tokens[0] = ScopeToken::Password;
    sensitive.tokens[1] = ScopeToken::Password;
    sensitive.tokens[2] = ScopeToken::Password;
    sensitive.tokens[3] = ScopeToken::Password;
    sensitive.raw_tokens[0] = 0x53534150UL;
    sensitive.raw_tokens[1] = 0x44524f57UL;
    sensitive.raw_tokens[2] = 0x54434552UL;
    sensitive.raw_tokens[3] = 0x21212121UL;
    producer.publish_scope(sensitive);

    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    CHECK(enable_for_test(options));

    const auto ordinary_operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(ordinary_operation != 0);
    CHECK(producer.try_write(queue_event(
        ScopeClass::Ordinary,
        Event::KeyDown,
        ordinary_operation)) == WriteResult::Written);
    CHECK(producer.try_write(queue_event(
        ScopeClass::Sensitive,
        Event::KeyDown,
        0x70000001ULL)) == WriteResult::Written);

    bool drained = false;
    const auto deadline = GetTickCount64() + kHostTimeoutMs;
    do {
        const auto status = send(
            options, command(DoctorHostCommand::Status));
        CHECK(status.failure == DoctorHostClientFailure::None);
        CHECK(status.reply.failure == DoctorStoreFailure::None);
        if (status.reply.status.raw_bytes != 0) {
            drained = true;
            break;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    CHECK(drained);

    const auto selected = wall_now_ms();
    auto prepared = send(
        options, prepare_request(selected, 5'000, 5'000));
    CHECK(prepared.failure == DoctorHostClientFailure::None);
    CHECK(prepared.reply.failure == DoctorStoreFailure::None);
    auto raw_request =
        snapshot_request(
            DoctorHostCommand::Preview, prepared.reply.snapshot);
    const auto frame = wire_request(raw_request);
    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    const auto raw = raw_exchange(
        DoctorHostEndpoint::pipe_name(user_sid),
        &frame,
        sizeof(frame));
    CHECK(raw.connected && raw.written && raw.read);
    DoctorHostWireReply raw_reply{};
    CHECK(decode_reply(raw.reply, &raw_reply));
    CHECK(raw_reply.protocol_failure ==
        DoctorHostProtocolFailure::None);
    CHECK(raw_reply.store_failure ==
        static_cast<std::uint32_t>(DoctorStoreFailure::None));
    CHECK(!contains_bytes(
        raw.reply,
        sensitive.raw_tokens.data(),
        sensitive.raw_tokens.size() *
            sizeof(sensitive.raw_tokens[0])));
    CHECK(!contains_bytes(
        raw.reply,
        options.root.wstring().c_str(),
        options.root.wstring().size() * sizeof(wchar_t)));

    const auto preview = send(options, raw_request);
    CHECK(preview.failure == DoctorHostClientFailure::None);
    CHECK(preview.reply.failure == DoctorStoreFailure::None);
    CHECK(preview.reply.preview.record_count == 1);
    const auto cancelled = send(options, snapshot_request(
        DoctorHostCommand::Cancel, prepared.reply.snapshot));
    CHECK(cancelled.failure == DoctorHostClientFailure::None);
    CHECK(cancelled.reply.failure == DoctorStoreFailure::None);
    CHECK(disable_and_wait(options, owner.get()));
    CHECK(producer.try_write(queue_event(
        ScopeClass::Ordinary,
        Event::KeyDown,
        producer.begin_per_key_operation(ScopeClass::Ordinary))) ==
        WriteResult::Disabled);

    SystemDoctorClock clock;
    auto store = DoctorStore::open(
        options.root, kBuild, clock, DoctorStoreLimits::defaults());
    CHECK(store);
    const auto records = store->read_records();
    CHECK(records.ok());
    CHECK(records.value.size() == 1);
    CHECK(records.value[0].operation_id ==
        static_cast<std::int64_t>(ordinary_operation));
    CHECK(records.value[0].scope == DoctorInputScope::Ordinary);
    return 0;
}

static int test_discovery_reaches_real_producer_after_512_prefix_entries() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    CHECK(options.producer_attachments.empty());
    ProcessProducer producer;
    CHECK(producer.ensure());

    ScopeObservation ordinary{};
    ordinary.context_available = true;
    ordinary.query_result = S_OK;
    ordinary.token_count = 1;
    ordinary.tokens[0] = ScopeToken::Default;
    producer.publish_scope(ordinary);

    const auto marker =
        temporary.path() / L"producer-target-returned.marker";
    SyntheticProcessEnumeration enumeration(
        GetCurrentProcessId(),
        512,
        marker);
    auto owner = start_helper(
        owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    CHECK(enable_for_test(options));

    const auto marker_deadline =
        GetTickCount64() + kHostTimeoutMs;
    do {
        if (GetFileAttributesW(marker.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            break;
        }
        Sleep(25);
    } while (GetTickCount64() < marker_deadline);
    CHECK(GetFileAttributesW(marker.c_str()) !=
        INVALID_FILE_ATTRIBUTES);

    const auto attach_deadline =
        GetTickCount64() + kHostTimeoutMs;
    do {
        if (producer.can_write()) break;
        Sleep(25);
    } while (GetTickCount64() < attach_deadline);
    CHECK(producer.can_write());

    const auto operation =
        producer.begin_per_key_operation(ScopeClass::Ordinary);
    CHECK(operation != 0);
    CHECK(producer.try_write(queue_event(
        ScopeClass::Ordinary,
        Event::KeyDown,
        operation)) == WriteResult::Written);

    bool drained = false;
    const auto drain_deadline =
        GetTickCount64() + kHostTimeoutMs;
    do {
        const auto status = send(
            options, command(DoctorHostCommand::Status));
        CHECK(status.failure == DoctorHostClientFailure::None);
        CHECK(status.reply.failure == DoctorStoreFailure::None);
        if (status.reply.status.raw_bytes != 0) {
            CHECK(!status.reply.status.producer_limit_reached);
            drained = true;
            break;
        }
        Sleep(25);
    } while (GetTickCount64() < drain_deadline);
    CHECK(drained);
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int test_stale_save_rejected_before_destination_touch() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    const auto prepared = send(
        options, prepare_request(wall_now_ms(), 1'000, 1'000));
    CHECK(prepared.failure == DoctorHostClientFailure::None);
    CHECK(prepared.reply.failure == DoctorStoreFailure::None);
    auto cleared = send(options, command(DoctorHostCommand::Clear));
    CHECK(cleared.failure == DoctorHostClientFailure::None);
    CHECK(cleared.reply.failure == DoctorStoreFailure::None);

    const auto destination = temporary.path() / "stale-destination.zip";
    CHECK(write_bytes(destination, "destination sentinel"));
    auto stale_save =
        snapshot_request(
            DoctorHostCommand::Save, prepared.reply.snapshot);
    stale_save.destination = destination.wstring();
    const auto saved = send(options, stale_save);
    CHECK(saved.failure == DoctorHostClientFailure::None);
    CHECK(saved.reply.failure == DoctorStoreFailure::EpochMismatch);
    CHECK(read_bytes(destination) == "destination sentinel");
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int test_delayed_client_receives_complete_reply() {
    constexpr const char kCase[] =
        "delayed-client-receives-complete-reply";
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    const auto expected = send(
        options, command(DoctorHostCommand::Status));
    CHECK(expected.failure == DoctorHostClientFailure::None);
    CHECK(expected.reply.failure == DoctorStoreFailure::None);
    CHECK(expected.server_pid == owner->pid());

    const auto frame =
        wire_request(command(DoctorHostCommand::Status));
    DoctorHostRequest decoded_request{};
    CHECK(doctor_host_detail::decode_request_wire(
        reinterpret_cast<const unsigned char *>(&frame),
        sizeof(frame),
        &decoded_request) == DoctorHostProtocolFailure::None);
    CHECK(decoded_request.command == DoctorHostCommand::Status);

    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    DWORD open_error = ERROR_SUCCESS;
    DWORD server_pid = 0;
    OwnedHandle pipe(open_raw_pipe_client(
        DoctorHostEndpoint::pipe_name(user_sid),
        kHostTimeoutMs,
        &server_pid,
        &open_error));
    CHECK(report_raw_pipe_stage(
        kCase, "open-raw-client", pipe.valid(), open_error));
    CHECK(server_pid == owner->pid());

    DWORD write_error = ERROR_SUCCESS;
    DWORD written = 0;
    const bool request_written = overlapped_io(
        pipe.get(),
        true,
        &frame,
        static_cast<DWORD>(sizeof(frame)),
        nullptr,
        &written,
        doctor_host_detail::kPipeIoTimeoutMs,
        &write_error);
    CHECK(report_raw_pipe_stage(
        kCase, "write-status", request_written, write_error));
    CHECK(report_raw_pipe_stage(
        kCase,
        "write-status-size",
        written == static_cast<DWORD>(sizeof(frame)),
        ERROR_SUCCESS));

    bool complete_reply_observed = false;
    bool disconnected_observed = false;
    DWORD observation_error = ERROR_SUCCESS;
    const auto observation_deadline =
        GetTickCount64() + doctor_host_detail::kPipeIoTimeoutMs;
    do {
        DWORD bytes_peeked = 0;
        DWORD bytes_available = 0;
        DWORD bytes_left_in_message = 0;
        const BOOL peeked = PeekNamedPipe(
            pipe.get(),
            nullptr,
            0,
            &bytes_peeked,
            &bytes_available,
            &bytes_left_in_message);
        if (peeked) {
            observation_error = ERROR_SUCCESS;
            if (bytes_available ==
                    static_cast<DWORD>(sizeof(DoctorHostWireReply)) &&
                bytes_left_in_message ==
                    static_cast<DWORD>(sizeof(DoctorHostWireReply))) {
                complete_reply_observed = true;
                break;
            }
        } else {
            observation_error = GetLastError();
            if (observation_error == ERROR_BROKEN_PIPE ||
                observation_error == ERROR_NO_DATA ||
                observation_error == ERROR_PIPE_NOT_CONNECTED) {
                disconnected_observed = true;
                break;
            }
            CHECK(report_raw_pipe_stage(
                kCase,
                "observe-complete-reply",
                false,
                observation_error));
        }
        Sleep(doctor_host_detail::kIoPollMs);
    } while (GetTickCount64() < observation_deadline);
    if (!complete_reply_observed && !disconnected_observed) {
        observation_error = ERROR_TIMEOUT;
    }
    CHECK(report_raw_pipe_stage(
        kCase,
        "observe-complete-reply",
        complete_reply_observed || disconnected_observed,
        observation_error));
    CHECK(owner->running());

    Sleep(kDelayedClientReadMs);

    CHECK(owner->running());
    std::vector<unsigned char> reply_bytes(
        sizeof(DoctorHostWireReply));
    DWORD read = 0;
    DWORD read_error = ERROR_SUCCESS;
    const bool reply_read = overlapped_io(
        pipe.get(),
        false,
        nullptr,
        static_cast<DWORD>(reply_bytes.size()),
        reply_bytes.data(),
        &read,
        doctor_host_detail::kPipeIoTimeoutMs,
        &read_error);
    if (!reply_read) {
        std::cerr
            << kCase
            << " observation="
            << (complete_reply_observed
                ? "complete-reply"
                : "disconnected")
            << " observationWin32Error="
            << static_cast<unsigned long>(observation_error)
            << '\n';
    }
    CHECK(report_raw_pipe_stage(
        kCase, "read-after-delay", reply_read, read_error));
    CHECK(report_raw_pipe_stage(
        kCase,
        "read-after-delay-size",
        read == static_cast<DWORD>(reply_bytes.size()),
        ERROR_SUCCESS));

    DoctorHostReply reply{};
    CHECK(report_raw_pipe_stage(
        kCase,
        "decode-complete-reply",
        doctor_host_detail::decode_reply_wire(
            reply_bytes.data(),
            reply_bytes.size(),
            &reply),
        ERROR_SUCCESS));
    CHECK(reply.protocol_failure ==
        DoctorHostProtocolFailure::None);
    CHECK(reply.failure == DoctorStoreFailure::None);
    CHECK(reply.platform_result_code == 0);
    CHECK(!reply.status.enabled);
    CHECK(!reply.status.collecting);
    CHECK(!reply.status.pending_clear);
    CHECK(reply.status.failure == DoctorStoreFailure::None);
    CHECK(reply.status.enabled == expected.reply.status.enabled);
    CHECK(reply.status.collecting ==
        expected.reply.status.collecting);
    CHECK(reply.status.pending_clear ==
        expected.reply.status.pending_clear);
    CHECK(reply.status.epoch == expected.reply.status.epoch);
    CHECK(reply.status.failure == expected.reply.status.failure);
    CHECK(reply.status.platform_result_code ==
        expected.reply.status.platform_result_code);
    CHECK(reply.status.raw_bytes ==
        expected.reply.status.raw_bytes);
    CHECK(reply.status.temp_bytes ==
        expected.reply.status.temp_bytes);
    CHECK(reply.status.sequence_gap ==
        expected.reply.status.sequence_gap);
    CHECK(reply.status.ignored_lines ==
        expected.reply.status.ignored_lines);
    CHECK(reply.status.producer_limit_reached ==
        expected.reply.status.producer_limit_reached);
    CHECK(reply.status.has_retained_range ==
        expected.reply.status.has_retained_range);
    CHECK(reply.status.oldest_retained_ms ==
        expected.reply.status.oldest_retained_ms);
    CHECK(reply.status.newest_retained_ms ==
        expected.reply.status.newest_retained_ms);
    CHECK(owner->running());
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int test_non_reading_client_is_bounded() {
    constexpr const char kCase[] =
        "non-reading-client-is-bounded";
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));

    const auto expected = send(
        options, command(DoctorHostCommand::Status));
    CHECK(expected.failure == DoctorHostClientFailure::None);
    CHECK(expected.reply.failure == DoctorStoreFailure::None);
    CHECK(expected.server_pid == owner->pid());

    const auto frame =
        wire_request(command(DoctorHostCommand::Status));
    DoctorHostRequest decoded_request{};
    CHECK(doctor_host_detail::decode_request_wire(
        reinterpret_cast<const unsigned char *>(&frame),
        sizeof(frame),
        &decoded_request) == DoctorHostProtocolFailure::None);
    CHECK(decoded_request.command == DoctorHostCommand::Status);

    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    DWORD open_error = ERROR_SUCCESS;
    DWORD server_pid = 0;
    OwnedHandle non_reading_client(open_raw_pipe_client(
        DoctorHostEndpoint::pipe_name(user_sid),
        kHostTimeoutMs,
        &server_pid,
        &open_error));
    CHECK(report_raw_pipe_stage(
        kCase,
        "open-non-reading-client",
        non_reading_client.valid(),
        open_error));
    CHECK(server_pid == owner->pid());

    DWORD write_error = ERROR_SUCCESS;
    DWORD written = 0;
    const bool request_written = overlapped_io(
        non_reading_client.get(),
        true,
        &frame,
        static_cast<DWORD>(sizeof(frame)),
        nullptr,
        &written,
        doctor_host_detail::kPipeIoTimeoutMs,
        &write_error);
    CHECK(report_raw_pipe_stage(
        kCase, "write-status", request_written, write_error));
    CHECK(report_raw_pipe_stage(
        kCase,
        "write-status-size",
        written == static_cast<DWORD>(sizeof(frame)),
        ERROR_SUCCESS));

    constexpr DWORD kRecoveryTimeoutMs =
        doctor_host_detail::kPipeIoTimeoutMs +
        kPipeRecoveryMarginMs;
    const auto recovery_started = GetTickCount64();
    const auto recovered = send(
        options,
        command(DoctorHostCommand::Status),
        false,
        kRecoveryTimeoutMs);
    const auto recovery_elapsed =
        GetTickCount64() - recovery_started;
    CHECK(recovered.failure == DoctorHostClientFailure::None);
    CHECK(recovered.reply.protocol_failure ==
        DoctorHostProtocolFailure::None);
    CHECK(recovered.reply.failure == DoctorStoreFailure::None);
    CHECK(recovered.server_pid == owner->pid());
    CHECK(recovery_elapsed <=
        static_cast<ULONGLONG>(kRecoveryTimeoutMs) +
            doctor_host_detail::kIoPollMs);
    CHECK(recovered.reply.status.enabled ==
        expected.reply.status.enabled);
    CHECK(recovered.reply.status.collecting ==
        expected.reply.status.collecting);
    CHECK(recovered.reply.status.epoch ==
        expected.reply.status.epoch);
    CHECK(!recovered.reply.status.enabled);
    CHECK(!recovered.reply.status.collecting);
    CHECK(recovered.reply.status.failure ==
        DoctorStoreFailure::None);
    CHECK(owner->running());
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

static int test_disconnect_is_bounded_and_next_owner_recovers() {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    auto options = host_options(temporary, registry);
    auto disconnect = start_helper(
        L"--doctor-test-disconnect-server");
    CHECK(disconnect);
    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    CHECK(wait_for_pipe(
        DoctorHostEndpoint::pipe_name(user_sid),
        kHostTimeoutMs));

    const auto started = GetTickCount64();
    const auto failed = send(
        options,
        command(DoctorHostCommand::Status),
        false,
        2'000);
    CHECK(failed.failure != DoctorHostClientFailure::None);
    CHECK(GetTickCount64() - started < 3'000);
    CHECK(disconnect->wait(kHostTimeoutMs));

    auto owner = start_helper(owner_arguments(options, "once"));
    CHECK(owner);
    CHECK(wait_for_owner(options));
    const auto recovered = send(
        options, command(DoctorHostCommand::Status));
    CHECK(recovered.failure == DoctorHostClientFailure::None);
    CHECK(recovered.reply.failure == DoctorStoreFailure::None);
    CHECK(disable_and_wait(options, owner.get()));
    return 0;
}

struct TestCase {
    const char *name;
    int (*run)();
};

constexpr TestCase kTests[] = {
    {"normal-medium-token", test_normal_medium_non_elevated_token},
    {"off-roundtrip", test_off_round_trip_never_enables},
    {
        "consent-and-temporary-run-key",
        test_enable_disable_uses_only_temporary_run_key,
    },
    {"client-auto-start-once", test_client_auto_starts_same_exe_once_owner},
    {
        "same-user-single-writer",
        test_same_user_launch_has_one_non_waiting_writer,
    },
    {
        "pipe-security-low-il",
        test_pipe_security_and_low_integrity_denial,
    },
    {
        "endpoint-image-spoof",
        test_endpoint_path_rejects_copied_image_spoof,
    },
    {
        "protocol-fail-closed",
        test_protocol_rejects_malformed_frames_without_mutation,
    },
    {
        "ui-process-exit-preview-recovery",
        test_pending_preview_is_recovered_after_ui_process_exit,
    },
    {
        "disable-preserves-live-preview",
        test_disable_preserves_live_preview_until_save_or_cancel,
    },
    {
        "upgrade-stop-cancels-preview-preserves-consent",
        test_upgrade_stop_cancels_preview_and_preserves_consent,
    },
    {
        "removal-stop-cancels-preview-preserves-consent",
        test_removal_stop_cancels_preview_and_preserves_consent,
    },
    {
        "server-crash-preview-recovery",
        test_server_crash_recovers_abandoned_preview,
    },
    {
        "producer-drain-and-privacy",
        test_attached_producer_drains_only_ordinary_events_while_on,
    },
    {
        "producer-discovery-after-512-prefix",
        test_discovery_reaches_real_producer_after_512_prefix_entries,
    },
    {
        "stale-epoch-save",
        test_stale_save_rejected_before_destination_touch,
    },
    {
        "delayed-client-receives-complete-reply",
        test_delayed_client_receives_complete_reply,
    },
    {
        "non-reading-client-is-bounded",
        test_non_reading_client_is_bounded,
    },
    {
        "bounded-disconnect-recovery",
        test_disconnect_is_bounded_and_next_owner_recovers,
    },
};

} // namespace

int main(int argc, char **argv) {
    std::cout << std::unitbuf;
    const std::wstring argument =
        argc > 1 ? ascii_wide(std::string(argv[1])) : std::wstring();
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-owner") {
        return owner_helper(argc, argv);
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-once") {
        return doctor_host_test::run_once_from_environment();
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-collector") {
        return doctor_host_test::run_collector_from_environment();
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-low-client") {
        return low_integrity_client_helper();
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-disconnect-server") {
        return disconnect_server_helper();
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-prepare-client") {
        return prepare_client_helper(argc, argv);
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-rpc") {
        return request_client_helper(argc, argv);
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-prepare-hold") {
        return held_prepare_client_helper(argc, argv);
    }
    if (argc > 1 &&
        std::string(argv[1]) == "--doctor-test-auto-start-client") {
        return auto_start_client_helper(argc, argv);
    }
    if (argc == 2 &&
        argument == kDoctorMaintenanceStopUpgradeArgument) {
        return doctor_host_test::run_maintenance_from_environment(
            DoctorHostMaintenanceAction::StopForUpgrade);
    }
    if (argc == 2 &&
        argument == kDoctorMaintenanceStopRemovalArgument) {
        return doctor_host_test::run_maintenance_from_environment(
            DoctorHostMaintenanceAction::StopForRemoval);
    }
    if (argc == 2 &&
        argument == kDoctorMaintenanceResumeArgument) {
        return doctor_host_test::run_maintenance_from_environment(
            DoctorHostMaintenanceAction::ResumeAfterUpgrade);
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
        std::cerr << "Unknown doctor host test case: " << selected << '\n';
        return 2;
    }
    return 0;
}
