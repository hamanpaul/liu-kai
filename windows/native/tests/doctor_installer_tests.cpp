#define WIN32_LEAN_AND_MEAN
#define BOSHIAMY_DOCTOR_HOST_TESTING 1
#define BOSHIAMY_DOCTOR_INSTALLER_TESTING 1
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace split_token_shell {

enum class Mode : std::uint32_t {
    Delegate = 0,
    Missing,
    OwnProcessExplorer,
    WrongImage,
    ImageQueryFailure,
};

using ShellWindowFunction = HWND (WINAPI *)();
using WindowThreadProcessIdFunction =
    DWORD (WINAPI *)(HWND, LPDWORD);
using QueryProcessImageFunction =
    BOOL (WINAPI *)(HANDLE, DWORD, LPWSTR, PDWORD);

const ShellWindowFunction real_shell_window =
    &::GetShellWindow;
const WindowThreadProcessIdFunction real_window_thread_process_id =
    &::GetWindowThreadProcessId;
const QueryProcessImageFunction real_query_process_image =
    &::QueryFullProcessImageNameW;

static Mode mode = Mode::Delegate;
static std::uint32_t shell_window_calls = 0;
static std::uint32_t process_id_calls = 0;
static std::uint32_t image_query_calls = 0;

static bool write_image(
    const std::wstring &path,
    LPWSTR output,
    PDWORD length) {
    if (!output || !length) return false;
    const DWORD requested = static_cast<DWORD>(path.size() + 1);
    if (*length < requested) {
        *length = requested;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    std::memcpy(
        output,
        path.c_str(),
        requested * sizeof(wchar_t));
    *length = static_cast<DWORD>(path.size());
    return true;
}

static HWND WINAPI observed_shell_window() {
    ++shell_window_calls;
    if (mode == Mode::Delegate) return real_shell_window();
    if (mode == Mode::Missing) return nullptr;
    return reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
}

static DWORD WINAPI observed_window_thread_process_id(
    HWND window, LPDWORD process_id) {
    ++process_id_calls;
    if (mode == Mode::Delegate)
        return real_window_thread_process_id(window, process_id);
    if (!process_id) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    *process_id = GetCurrentProcessId();
    return GetCurrentThreadId();
}

static std::wstring explorer_image() {
    wchar_t directory[32768]{};
    const UINT length = GetWindowsDirectoryW(
        directory, static_cast<UINT>(std::size(directory)));
    if (!length || length >= std::size(directory)) return {};
    std::wstring path(directory, length);
    if (path.back() != L'\\') path += L'\\';
    path += L"explorer.exe";
    return path;
}

static BOOL WINAPI observed_query_process_image(
    HANDLE process,
    DWORD flags,
    LPWSTR output,
    PDWORD length) {
    ++image_query_calls;
    if (mode == Mode::Delegate)
        return real_query_process_image(
            process, flags, output, length);
    if (mode == Mode::ImageQueryFailure) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const std::wstring path =
        mode == Mode::WrongImage
            ? [] {
                wchar_t executable[32768]{};
                const DWORD size = GetModuleFileNameW(
                    nullptr,
                    executable,
                    static_cast<DWORD>(std::size(executable)));
                return size && size < std::size(executable)
                    ? std::wstring(executable, size)
                    : std::wstring();
            }()
            : explorer_image();
    if (path.empty()) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return write_image(path, output, length);
}

static void reset(Mode next) noexcept {
    mode = next;
    shell_window_calls = 0;
    process_id_calls = 0;
    image_query_calls = 0;
}

static std::uint32_t shell_calls() noexcept {
    return shell_window_calls;
}

} // namespace split_token_shell

#define GetShellWindow split_token_shell::observed_shell_window
#define GetWindowThreadProcessId \
    split_token_shell::observed_window_thread_process_id
#define QueryFullProcessImageNameW \
    split_token_shell::observed_query_process_image

namespace review7_winapi {

using CreateMutexFunction =
    HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using OpenMutexFunction =
    HANDLE (WINAPI *)(DWORD, BOOL, LPCWSTR);

const CreateMutexFunction kRealCreateMutexW =
    &::CreateMutexW;
const OpenMutexFunction kRealOpenMutexW =
    &::OpenMutexW;

static std::atomic<unsigned> create_mutex_calls{0};
static std::atomic<unsigned> open_mutex_calls{0};

static HANDLE WINAPI observed_create_mutex_w(
    LPSECURITY_ATTRIBUTES attributes,
    BOOL initial_owner,
    LPCWSTR name) {
    create_mutex_calls.fetch_add(1, std::memory_order_relaxed);
    return kRealCreateMutexW(attributes, initial_owner, name);
}

static HANDLE WINAPI observed_open_mutex_w(
    DWORD desired_access,
    BOOL inherit_handle,
    LPCWSTR name) {
    open_mutex_calls.fetch_add(1, std::memory_order_relaxed);
    return kRealOpenMutexW(
        desired_access, inherit_handle, name);
}

static void reset_mutex_observation() noexcept {
    create_mutex_calls.store(0, std::memory_order_relaxed);
    open_mutex_calls.store(0, std::memory_order_relaxed);
}

static unsigned observed_create_mutex_calls() noexcept {
    return create_mutex_calls.load(std::memory_order_relaxed);
}

static unsigned observed_open_mutex_calls() noexcept {
    return open_mutex_calls.load(std::memory_order_relaxed);
}

} // namespace review7_winapi

#define CreateMutexW review7_winapi::observed_create_mutex_w
#define OpenMutexW review7_winapi::observed_open_mutex_w
#include "../doctor_host.hpp"
#include "../wix/doctor_lifecycle.hpp"
#undef QueryFullProcessImageNameW
#undef GetWindowThreadProcessId
#undef GetShellWindow
#undef OpenMutexW
#undef CreateMutexW

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
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
namespace installer = boshiamy::doctor::installer;

namespace {

const DoctorBuildMetadata kBuild{
    "0.1.0-installer-test",
    "506dcded002b2b3fa9edbe6b387d3e6797aa2383",
};

constexpr DWORD kOwnerTimeoutMs = 5'000;
constexpr DWORD kLifecycleTimeoutMs = 10'000;

class OwnedTempDirectory {
public:
    OwnedTempDirectory() {
        std::error_code error;
        base_ = fs::weakly_canonical(fs::temp_directory_path(), error);
        if (error || !base_.is_absolute()) {
            throw std::runtime_error("Cannot resolve the system temp directory");
        }
        prefix_ =
            L"liu-kai-doctor-installer-" +
            std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-";
        for (std::uint32_t attempt = 0; attempt != 32; ++attempt) {
            const auto candidate =
                base_ / (prefix_ + std::to_wstring(counter() + attempt));
            if (fs::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            error.clear();
        }
        throw std::runtime_error(
            "Cannot create an owned doctor installer test directory");
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
        path_ = L"Software\\LiuKaiDoctorInstallerTests\\" +
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
            throw std::runtime_error(
                "Cannot create temporary HKCU installer test key");
        }
    }

    TemporaryRegistryKey(const TemporaryRegistryKey &) = delete;
    TemporaryRegistryKey &operator=(const TemporaryRegistryKey &) = delete;

    ~TemporaryRegistryKey() {
        if (path_.rfind(
                L"Software\\LiuKaiDoctorInstallerTests\\", 0) == 0) {
            RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        }
    }

    const std::wstring &path() const noexcept { return path_; }

    bool set_value(
        const std::wstring &name, const std::wstring &value) const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                path_.c_str(),
                0,
                KEY_SET_VALUE,
                &key) != ERROR_SUCCESS) {
            return false;
        }
        const LSTATUS written = RegSetValueExW(
            key,
            name.c_str(),
            0,
            REG_SZ,
            reinterpret_cast<const BYTE *>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return written == ERROR_SUCCESS;
    }

    bool query_value(
        const std::wstring &name, std::wstring *value) const {
        if (!value) return false;
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                path_.c_str(),
                0,
                KEY_QUERY_VALUE,
                &key) != ERROR_SUCCESS) {
            return false;
        }
        DWORD type = 0;
        DWORD bytes = 0;
        LSTATUS status = RegQueryValueExW(
            key, name.c_str(), nullptr, &type, nullptr, &bytes);
        if (status != ERROR_SUCCESS || type != REG_SZ || bytes == 0 ||
            bytes % sizeof(wchar_t) != 0) {
            RegCloseKey(key);
            return false;
        }
        std::vector<wchar_t> storage(bytes / sizeof(wchar_t));
        status = RegQueryValueExW(
            key,
            name.c_str(),
            nullptr,
            &type,
            reinterpret_cast<BYTE *>(storage.data()),
            &bytes);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS || storage.empty()) return false;
        if (storage.back() == L'\0') storage.pop_back();
        value->assign(storage.begin(), storage.end());
        return true;
    }

    bool has_value(const std::wstring &name) const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                path_.c_str(),
                0,
                KEY_QUERY_VALUE,
                &key) != ERROR_SUCCESS) {
            return false;
        }
        DWORD type = 0;
        const LSTATUS status = RegQueryValueExW(
            key, name.c_str(), nullptr, &type, nullptr, nullptr);
        RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    bool delete_value(const std::wstring &name) const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                path_.c_str(),
                0,
                KEY_SET_VALUE,
                &key) != ERROR_SUCCESS) {
            return false;
        }
        const LSTATUS removed =
            RegDeleteValueW(key, name.c_str());
        RegCloseKey(key);
        return removed == ERROR_SUCCESS ||
            removed == ERROR_FILE_NOT_FOUND;
    }

private:
    static std::uint64_t counter() {
        static volatile LONG64 value = 0;
        return static_cast<std::uint64_t>(InterlockedIncrement64(&value));
    }

    std::wstring path_;
};

static std::wstring quote(const std::wstring &value) {
    return L"\"" + value + L"\"";
}

class ChildProcess {
public:
    ChildProcess(
        const fs::path &executable, const std::wstring &arguments) {
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
            CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
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
                WaitForSingleObject(process_.hProcess, 3'000);
            }
            CloseHandle(process_.hProcess);
            if (process_.hThread) CloseHandle(process_.hThread);
        }
    }

    bool created() const noexcept { return created_; }
    DWORD pid() const noexcept {
        return created_ ? process_.dwProcessId : 0;
    }

    bool wait(DWORD milliseconds) const {
        return created_ &&
            WaitForSingleObject(
                process_.hProcess, milliseconds) == WAIT_OBJECT_0;
    }

    bool running() const {
        if (!created_) return false;
        DWORD exit = 0;
        return GetExitCodeProcess(process_.hProcess, &exit) &&
            exit == STILL_ACTIVE;
    }

    DWORD exit_code() const {
        DWORD exit = 0xffffffffUL;
        if (created_) {
            GetExitCodeProcess(process_.hProcess, &exit);
        }
        return exit;
    }

private:
    bool created_ = false;
    PROCESS_INFORMATION process_{};
};

static fs::path test_canonical(const fs::path &path) {
    std::error_code error;
    const auto result = fs::weakly_canonical(path, error);
    return !error && result.is_absolute() ? result : fs::path();
}

static bool contained_by(
    const fs::path &root, const fs::path &candidate) {
    const auto canonical_root = test_canonical(root);
    const auto canonical_candidate = test_canonical(candidate);
    if (canonical_root.empty() || canonical_candidate.empty()) return false;
    const auto relative =
        canonical_candidate.lexically_relative(canonical_root);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto &part : relative) {
        if (part == L"." || part == L"..") return false;
    }
    return true;
}

static fs::path copy_fixture(
    const fs::path &source,
    const fs::path &destination,
    const fs::path &product_root) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) return {};
    error.clear();
    if (!fs::copy_file(
            source,
            destination,
            fs::copy_options::overwrite_existing,
            error) ||
        error ||
        !contained_by(product_root, destination)) {
        return {};
    }
    return test_canonical(destination);
}

static doctor_host_test::Options host_options(
    const fs::path &doctor_root,
    const fs::path &settings_exe,
    const TemporaryRegistryKey &registry) {
    doctor_host_test::Options options;
    options.root = doctor_root;
    options.build = kBuild;
    options.settings_exe_path = settings_exe;
    options.run_key_path = registry.path();
    options.idle_timeout_ms = 30'000;
    return options;
}

static installer::test::Options lifecycle_options(
    const fs::path &product_root,
    const fs::path &settings_exe,
    const fs::path &doctor_root,
    const TemporaryRegistryKey &registry) {
    return installer::test::Options{
        product_root,
        settings_exe,
        doctor_root,
        kBuild,
        registry.path(),
        kLifecycleTimeoutMs,
    };
}

inline constexpr std::uint32_t kTestRpcResultMagic =
    0x4c4b5254UL;
inline constexpr std::uint32_t kTestRpcResultVersion = 1;

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

static fs::path unique_rpc_sidecar(
    const fs::path &settings_exe, const std::wstring &prefix) {
    const auto parent = test_canonical(settings_exe.parent_path());
    if (parent.empty() || !parent.is_absolute()) return {};
    static volatile LONG64 value = 0;
    const auto leaf =
        prefix +
        std::to_wstring(static_cast<unsigned long>(
            GetCurrentProcessId())) +
        L"-" + std::to_wstring(GetTickCount64()) +
        L"-" + std::to_wstring(static_cast<unsigned long long>(
            InterlockedIncrement64(&value))) +
        L".bin";
    const auto path = parent / leaf;
    if (path.wstring().size() >
        kDoctorHostMaximumPathCodeUnits) {
        return {};
    }
    const DWORD attributes =
        GetFileAttributesW(path.c_str());
    const DWORD error =
        attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError()
            : ERROR_SUCCESS;
    return attributes == INVALID_FILE_ATTRIBUTES &&
        (error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND)
        ? path
        : fs::path();
}

static bool read_test_rpc_result(
    const fs::path &path,
    DoctorHostClientResult *output) {
    if (!output) return false;
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart != sizeof(TestRpcResult)) {
        CloseHandle(file);
        return false;
    }
    TestRpcResult envelope{};
    DWORD read = 0;
    const BOOL complete = ReadFile(
        file,
        &envelope,
        static_cast<DWORD>(sizeof(envelope)),
        &read,
        nullptr);
    CloseHandle(file);
    if (!complete ||
        read != sizeof(envelope) ||
        envelope.magic != kTestRpcResultMagic ||
        envelope.version != kTestRpcResultVersion) {
        return false;
    }
    *output = envelope.result;
    return true;
}

static bool await_test_rpc_result(
    ChildProcess *process,
    const fs::path &path,
    DWORD timeout_ms,
    DoctorHostClientResult *output) {
    if (!process || !output) return false;
    const auto deadline =
        GetTickCount64() +
        static_cast<std::uint64_t>(timeout_ms) +
        1'000ULL;
    do {
        if (read_test_rpc_result(path, output)) return true;
        if (!process->running()) return false;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}

static DoctorHostRequest command(DoctorHostCommand value) {
    DoctorHostRequest result;
    result.command = value;
    return result;
}

static DoctorHostClientResult send(
    const doctor_host_test::Options &options,
    const DoctorHostRequest &request,
    bool auto_start = false,
    DWORD timeout_ms = kOwnerTimeoutMs) {
    DoctorHostClientResult failure;
    failure.failure = DoctorHostClientFailure::InvalidRequest;
    failure.platform_result_code =
        static_cast<std::int32_t>(E_INVALIDARG);
    if (auto_start ||
        timeout_ms == 0 ||
        timeout_ms > kOwnerTimeoutMs ||
        options.settings_exe_path.empty()) {
        return failure;
    }
    const wchar_t *name = nullptr;
    switch (request.command) {
    case DoctorHostCommand::Status:
        name = L"status";
        break;
    case DoctorHostCommand::Enable:
        name = L"enable";
        break;
    case DoctorHostCommand::Disable:
        name = L"disable";
        break;
    default:
        return failure;
    }
    const auto response = unique_rpc_sidecar(
        options.settings_exe_path, L".liu-kai-doctor-rpc-");
    if (response.empty()) return failure;
    const std::wstring arguments =
        L"--doctor-test-rpc " +
        quote(options.root.wstring()) + L" " +
        quote(options.run_key_path) + L" " +
        quote(response.wstring()) + L" " +
        std::wstring(name) + L" " +
        std::to_wstring(timeout_ms);
    ChildProcess child(options.settings_exe_path, arguments);
    if (!child.created()) {
        failure.failure =
            DoctorHostClientFailure::ProcessStartFailed;
        failure.platform_result_code =
            static_cast<std::int32_t>(
                HRESULT_FROM_WIN32(GetLastError()));
        return failure;
    }
    DoctorHostClientResult result;
    const bool received = await_test_rpc_result(
        &child, response, timeout_ms, &result);
    DeleteFileW(response.c_str());
    if (!received || !child.wait(1'000) ||
        child.exit_code() != 0) {
        failure.failure = DoctorHostClientFailure::Timeout;
        failure.platform_result_code =
            static_cast<std::int32_t>(
                HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        return failure;
    }
    return result;
}

static bool wait_for_owner(const doctor_host_test::Options &options) {
    const auto deadline = GetTickCount64() + kOwnerTimeoutMs;
    do {
        const auto status = send(
            options, command(DoctorHostCommand::Status), false, 250);
        if (status.failure == DoctorHostClientFailure::None &&
            status.reply.failure == DoctorStoreFailure::None) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

static bool wait_for_endpoint_unavailable(
    const doctor_host_test::Options &options) {
    const auto deadline = GetTickCount64() + 3'000;
    do {
        const auto status = send(
            options, command(DoctorHostCommand::Status), false, 250);
        if (status.failure ==
            DoctorHostClientFailure::EndpointUnavailable) {
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

static std::unique_ptr<ChildProcess> start_owner(
    const doctor_host_test::Options &options) {
    const std::wstring arguments =
        L"--doctor-test-owner once " +
        quote(options.root.wstring()) + L" " +
        quote(options.run_key_path);
    auto child = std::make_unique<ChildProcess>(
        options.settings_exe_path, arguments);
    return child->created() ? std::move(child) : nullptr;
}

static bool enable(const doctor_host_test::Options &options) {
    const auto result = send(
        options, command(DoctorHostCommand::Enable));
    return result.failure == DoctorHostClientFailure::None &&
        result.reply.failure == DoctorStoreFailure::None;
}

static DoctorHostRequest prepare_request() {
    SYSTEMTIME system{};
    GetSystemTime(&system);
    FILETIME file{};
    SystemTimeToFileTime(&system, &file);
    ULARGE_INTEGER now{};
    now.LowPart = file.dwLowDateTime;
    now.HighPart = file.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks =
        116444736000000000ULL;
    const std::int64_t selected =
        now.QuadPart > kUnixEpochTicks
            ? static_cast<std::int64_t>(
                (now.QuadPart - kUnixEpochTicks) / 10000ULL)
            : 0;
    auto result = command(DoctorHostCommand::Prepare);
    result.mark.selected_issue_time_ms = selected;
    result.mark.before_ms = 5'000;
    result.mark.after_ms = 1'000;
    return result;
}

static bool write_file(
    const fs::path &path, const std::wstring &value) {
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD bytes =
        static_cast<DWORD>(value.size() * sizeof(wchar_t));
    DWORD written = 0;
    const BOOL result = WriteFile(
        file,
        value.data(),
        bytes,
        &written,
        nullptr);
    CloseHandle(file);
    return result && written == bytes;
}

static bool read_file(
    const fs::path &path, std::wstring *value) {
    if (!value) return false;
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const LARGE_INTEGER size = {};
    LARGE_INTEGER mutable_size = size;
    if (!GetFileSizeEx(file, &mutable_size) ||
        mutable_size.QuadPart < 0 ||
        mutable_size.QuadPart > 4096 ||
        mutable_size.QuadPart % sizeof(wchar_t) != 0) {
        CloseHandle(file);
        return false;
    }
    std::vector<wchar_t> storage(
        static_cast<std::size_t>(
            mutable_size.QuadPart / sizeof(wchar_t)));
    DWORD read = 0;
    const BOOL result = ReadFile(
        file,
        storage.data(),
        static_cast<DWORD>(mutable_size.QuadPart),
        &read,
        nullptr);
    CloseHandle(file);
    if (!result || read != static_cast<DWORD>(mutable_size.QuadPart)) {
        return false;
    }
    value->assign(storage.begin(), storage.end());
    return true;
}

class HeldPrepareClient {
public:
    HeldPrepareClient(
        const doctor_host_test::Options &options,
        const DoctorHostRequest &request)
        : response_(unique_rpc_sidecar(
            options.settings_exe_path,
            L".liu-kai-doctor-rpc-")),
          release_(unique_rpc_sidecar(
            options.settings_exe_path,
            L".liu-kai-doctor-release-")) {
        if (request.command != DoctorHostCommand::Prepare ||
            response_.empty() ||
            release_.empty()) {
            return;
        }
        const std::wstring arguments =
            L"--doctor-test-prepare-hold " +
            quote(options.root.wstring()) + L" " +
            quote(options.run_key_path) + L" " +
            quote(response_.wstring()) + L" " +
            quote(release_.wstring()) + L" " +
            std::to_wstring(
                request.mark.selected_issue_time_ms) + L" " +
            std::to_wstring(request.mark.before_ms) + L" " +
            std::to_wstring(request.mark.after_ms);
        process_ = std::make_unique<ChildProcess>(
            options.settings_exe_path, arguments);
        if (!process_->created()) return;
        if (!await_test_rpc_result(
                process_.get(),
                response_,
                kOwnerTimeoutMs,
                &result_) ||
            result_.failure != DoctorHostClientFailure::None ||
            result_.reply.failure != DoctorStoreFailure::None ||
            !process_->running()) {
            return;
        }
        valid_ = true;
    }

    HeldPrepareClient(const HeldPrepareClient &) = delete;
    HeldPrepareClient &operator=(
        const HeldPrepareClient &) = delete;

    ~HeldPrepareClient() {
        release();
        if (!response_.empty()) DeleteFileW(response_.c_str());
        if (!release_.empty()) DeleteFileW(release_.c_str());
    }

    bool valid() const noexcept { return valid_; }
    bool client_running() const noexcept {
        return process_ && process_->running();
    }
    const DoctorHostClientResult &result() const noexcept {
        return result_;
    }

    bool release() {
        if (!process_ || !process_->created()) return true;
        if (process_->running()) {
            if (!released_) {
                if (!write_file(release_, L"1")) return false;
                released_ = true;
            }
            if (!process_->wait(kOwnerTimeoutMs)) return false;
        }
        return process_->exit_code() == 0;
    }

private:
    fs::path response_;
    fs::path release_;
    std::unique_ptr<ChildProcess> process_;
    DoctorHostClientResult result_{};
    bool valid_ = false;
    bool released_ = false;
};

static int seed_history(const fs::path &root) {
    doctor_host_detail::SystemDoctorClock clock;
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
    return 0;
}

static bool record_count(
    const fs::path &root, std::size_t *count) {
    if (!count) return false;
    doctor_host_detail::SystemDoctorClock clock;
    auto store = DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    if (!store) return false;
    const auto records = store->read_records();
    if (!records.ok()) return false;
    *count = records.value.size();
    return true;
}

static bool stored_enabled(
    const fs::path &root, bool *enabled) {
    if (!enabled) return false;
    doctor_host_detail::SystemDoctorClock clock;
    auto store = DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    if (!store) return false;
    *enabled = store->status().enabled;
    return true;
}

static bool current_process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const BOOL read = GetTokenInformation(
        token,
        TokenElevation,
        &elevation,
        sizeof(elevation),
        &bytes);
    CloseHandle(token);
    return read && elevation.TokenIsElevated != 0;
}

static bool current_process_is_normal_medium() {
    const auto identity = detail::own_token_identity();
    return identity.result_hr ==
            static_cast<std::int32_t>(S_OK) &&
        identity.app_container == 0 &&
        identity.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID) &&
        !current_process_elevated();
}

class ScopedShellFixture {
public:
    explicit ScopedShellFixture(
        split_token_shell::Mode mode) {
        split_token_shell::reset(mode);
    }

    ScopedShellFixture(const ScopedShellFixture &) = delete;
    ScopedShellFixture &operator=(
        const ScopedShellFixture &) = delete;

    ~ScopedShellFixture() {
        split_token_shell::reset(
            split_token_shell::Mode::Delegate);
    }
};

static bool own_token_identity(
    std::wstring *sid, DWORD *session) {
    if (!sid || !session) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    const auto context =
        installer::doctor_lifecycle_detail::token_context(
            token);
    CloseHandle(token);
    if (!SUCCEEDED(context.result)) return false;
    *sid = context.user_sid;
    *session = context.session_id;
    return true;
}

static bool open_own_process_token(
    installer::doctor_lifecycle_detail::UniqueHandle *token) {
    return token &&
        OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_DUPLICATE,
            &token->value) != FALSE;
}

static bool duplicate_own_token(
    SECURITY_IMPERSONATION_LEVEL level,
    TOKEN_TYPE type,
    installer::doctor_lifecycle_detail::UniqueHandle *result) {
    if (!result) return false;
    installer::doctor_lifecycle_detail::UniqueHandle source;
    if (!open_own_process_token(&source)) return false;
    return DuplicateTokenEx(
        source.value,
        TOKEN_ASSIGN_PRIMARY |
            TOKEN_QUERY |
            TOKEN_DUPLICATE,
        nullptr,
        level,
        type,
        &result->value) != FALSE;
}

static bool token_type(
    HANDLE token, TOKEN_TYPE *type) {
    if (!token || !type) return false;
    DWORD copied = 0;
    return GetTokenInformation(
        token,
        TokenType,
        type,
        sizeof(*type),
        &copied) != FALSE;
}

static bool token_impersonation_level(
    HANDLE token, SECURITY_IMPERSONATION_LEVEL *level) {
    if (!token || !level) return false;
    DWORD copied = 0;
    return GetTokenInformation(
        token,
        TokenImpersonationLevel,
        level,
        sizeof(*level),
        &copied) != FALSE;
}

static bool primary_token_context(
    HANDLE token,
    const std::wstring &expected_sid,
    DWORD expected_session) {
    if (!token) return false;
    const auto context =
        installer::doctor_lifecycle_detail::token_context(token);
    return installer::doctor_lifecycle_detail::normal_same_user(
        context, expected_sid, expected_session);
}

static int test_medium_primary_token_accepts_normal_primary(
    const fs::path &) {
    OwnedTempDirectory temporary;
    const auto doctor_root =
        temporary.path() / L"doctor-not-created";
    std::wstring sid;
    DWORD session = 0;
    CHECK(own_token_identity(&sid, &session));
    installer::doctor_lifecycle_detail::UniqueHandle source;
    CHECK(open_own_process_token(&source));
    TOKEN_TYPE source_type{};
    CHECK(token_type(source.value, &source_type));
    CHECK(source_type == TokenPrimary);

    ScopedShellFixture shell(
        split_token_shell::Mode::Missing);
    installer::doctor_lifecycle_detail::UniqueHandle primary;
    bool used_shell = true;
    const HRESULT result =
        installer::doctor_lifecycle_detail::medium_primary_token(
            source.value,
            sid,
            session,
            &primary,
            &used_shell);
    CHECK(result == S_OK);
    CHECK(primary.value);
    CHECK(!used_shell);
    CHECK(split_token_shell::shell_calls() == 0);
    TOKEN_TYPE result_type{};
    CHECK(token_type(primary.value, &result_type));
    CHECK(result_type == TokenPrimary);
    CHECK(primary_token_context(
        primary.value, sid, session));
    CHECK(!fs::exists(doctor_root));
    return 0;
}

static int test_medium_primary_token_accepts_impersonation(
    const fs::path &) {
    OwnedTempDirectory temporary;
    const auto doctor_root =
        temporary.path() / L"doctor-not-created";
    std::wstring sid;
    DWORD session = 0;
    CHECK(own_token_identity(&sid, &session));
    installer::doctor_lifecycle_detail::UniqueHandle impersonation;
    CHECK(duplicate_own_token(
        SecurityImpersonation,
        TokenImpersonation,
        &impersonation));
    TOKEN_TYPE source_type{};
    SECURITY_IMPERSONATION_LEVEL level{};
    CHECK(token_type(impersonation.value, &source_type));
    CHECK(token_impersonation_level(
        impersonation.value, &level));
    CHECK(source_type == TokenImpersonation);
    CHECK(level == SecurityImpersonation);

    ScopedShellFixture shell(
        split_token_shell::Mode::Missing);
    installer::doctor_lifecycle_detail::UniqueHandle primary;
    bool used_shell = true;
    const HRESULT result =
        installer::doctor_lifecycle_detail::medium_primary_token(
            impersonation.value,
            sid,
            session,
            &primary,
            &used_shell);
    CHECK(result == S_OK);
    CHECK(primary.value);
    CHECK(!used_shell);
    CHECK(split_token_shell::shell_calls() == 0);
    CHECK(primary_token_context(
        primary.value, sid, session));
    CHECK(!fs::exists(doctor_root));
    return 0;
}

static int test_medium_primary_token_uses_shell_for_identification(
    const fs::path &) {
    OwnedTempDirectory temporary;
    const auto doctor_root =
        temporary.path() / L"doctor-not-created";
    std::wstring sid;
    DWORD session = 0;
    CHECK(own_token_identity(&sid, &session));
    installer::doctor_lifecycle_detail::UniqueHandle identification;
    CHECK(duplicate_own_token(
        SecurityIdentification,
        TokenImpersonation,
        &identification));
    TOKEN_TYPE source_type{};
    SECURITY_IMPERSONATION_LEVEL level{};
    CHECK(token_type(identification.value, &source_type));
    CHECK(token_impersonation_level(
        identification.value, &level));
    CHECK(source_type == TokenImpersonation);
    CHECK(level == SecurityIdentification);

    installer::doctor_lifecycle_detail::UniqueHandle direct_primary;
    SetLastError(ERROR_SUCCESS);
    CHECK(DuplicateTokenEx(
        identification.value,
        TOKEN_ASSIGN_PRIMARY |
            TOKEN_QUERY |
            TOKEN_DUPLICATE,
        nullptr,
        SecurityImpersonation,
        TokenPrimary,
        &direct_primary.value) == FALSE);
    CHECK(GetLastError() ==
        ERROR_BAD_IMPERSONATION_LEVEL);
    CHECK(!direct_primary.value);

    ScopedShellFixture shell(
        split_token_shell::Mode::OwnProcessExplorer);
    installer::doctor_lifecycle_detail::UniqueHandle primary;
    bool used_shell = false;
    const HRESULT result =
        installer::doctor_lifecycle_detail::medium_primary_token(
            identification.value,
            sid,
            session,
            &primary,
            &used_shell);
    CHECK(result == S_OK);
    CHECK(primary.value);
    CHECK(used_shell);
    CHECK(split_token_shell::shell_calls() == 1);
    CHECK(primary_token_context(
        primary.value, sid, session));
    CHECK(!fs::exists(doctor_root));
    return 0;
}

static int test_medium_primary_token_rejects_unverified_sources(
    const fs::path &) {
    OwnedTempDirectory temporary;
    const auto doctor_root =
        temporary.path() / L"doctor-not-created";
    std::wstring sid;
    DWORD session = 0;
    CHECK(own_token_identity(&sid, &session));
    installer::doctor_lifecycle_detail::UniqueHandle identification;
    CHECK(duplicate_own_token(
        SecurityIdentification,
        TokenImpersonation,
        &identification));

    {
        ScopedShellFixture shell(
            split_token_shell::Mode::Missing);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                identification.value,
                sid,
                session,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
        CHECK(split_token_shell::shell_calls() == 1);
    }
    {
        ScopedShellFixture shell(
            split_token_shell::Mode::WrongImage);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                identification.value,
                sid,
                session,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
    }
    {
        ScopedShellFixture shell(
            split_token_shell::Mode::ImageQueryFailure);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                identification.value,
                sid,
                session,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
    }
    {
        ScopedShellFixture shell(
            split_token_shell::Mode::OwnProcessExplorer);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        CHECK(duplicate_own_token(
            SecurityImpersonation,
            TokenPrimary,
            &primary));
        CHECK(primary.value);
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                identification.value,
                sid + L"-wrong",
                session,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
        CHECK(split_token_shell::shell_calls() == 0);
    }
    {
        ScopedShellFixture shell(
            split_token_shell::Mode::OwnProcessExplorer);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                identification.value,
                sid,
                session + 1,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
        CHECK(split_token_shell::shell_calls() == 0);
    }
    {
        ScopedShellFixture shell(
            split_token_shell::Mode::OwnProcessExplorer);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                nullptr,
                sid,
                session,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
        CHECK(split_token_shell::shell_calls() == 0);
    }
    {
        ScopedShellFixture shell(
            split_token_shell::Mode::OwnProcessExplorer);
        installer::doctor_lifecycle_detail::UniqueHandle primary;
        bool used_shell = true;
        CHECK(FAILED(
            installer::doctor_lifecycle_detail::medium_primary_token(
                INVALID_HANDLE_VALUE,
                sid,
                session,
                &primary,
                &used_shell)));
        CHECK(!primary.value);
        CHECK(!used_shell);
        CHECK(split_token_shell::shell_calls() == 0);
    }
    CHECK(!fs::exists(doctor_root));
    return 0;
}

static bool launch_is_normal_medium(
    const installer::DoctorLifecycleResult &result) {
    return SUCCEEDED(result.result) &&
        result.disposition ==
            installer::DoctorLifecycleDisposition::HelperCompleted &&
        result.ok() &&
        result.helper_exit_code == 0 &&
        result.same_user &&
        !result.elevated &&
        result.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID) &&
        (current_process_elevated()
            ? result.used_linked_token
            : true);
}

static bool launch_is_not_needed(
    const installer::DoctorLifecycleResult &result) {
    return SUCCEEDED(result.result) &&
        result.disposition ==
            installer::DoctorLifecycleDisposition::StopNotNeeded &&
        result.ok() &&
        result.helper_exit_code == 0xffffffffUL &&
        result.same_user &&
        !result.used_linked_token &&
        !result.elevated &&
        result.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID);
}

static bool process_is_normal_medium(DWORD process_id) {
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        process_id);
    if (!process) return false;
    HANDLE token = nullptr;
    const BOOL opened =
        OpenProcessToken(process, TOKEN_QUERY, &token);
    CloseHandle(process);
    if (!opened) return false;
    const auto identity = detail::token_identity(token);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const BOOL read = GetTokenInformation(
        token,
        TokenElevation,
        &elevation,
        sizeof(elevation),
        &bytes);
    CloseHandle(token);
    return identity.result_hr ==
            static_cast<std::int32_t>(S_OK) &&
        identity.app_container == 0 &&
        identity.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID) &&
        read && elevation.TokenIsElevated == 0;
}

class EndpointCleanup {
public:
    EndpointCleanup(
        doctor_host_test::Options options,
        fs::path product_root)
        : options_(std::move(options)),
          product_root_(std::move(product_root)) {}

    EndpointCleanup(const EndpointCleanup &) = delete;
    EndpointCleanup &operator=(const EndpointCleanup &) = delete;

    ~EndpointCleanup() {
        if (!active_) return;
        const auto status = send(
            options_, command(DoctorHostCommand::Status), false, 500);
        if (status.failure ==
            DoctorHostClientFailure::EndpointUnavailable) {
            return;
        }
        if (status.failure == DoctorHostClientFailure::None) {
            installer::test::Options lifecycle{
                product_root_,
                options_.settings_exe_path,
                options_.root,
                options_.build,
                options_.run_key_path,
                2'000,
            };
            const auto stopped = installer::test::launch(
                lifecycle,
                installer::DoctorLifecycleAction::StopForRemoval);
            if (stopped.ok() &&
                wait_for_endpoint_unavailable(options_)) {
                return;
            }
        }
        if (!status.server_pid ||
            !contained_by(product_root_, options_.settings_exe_path)) {
            return;
        }
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            status.server_pid);
        if (!process) return;
        std::vector<wchar_t> image(4096);
        DWORD length = static_cast<DWORD>(image.size());
        const BOOL read = QueryFullProcessImageNameW(
            process, 0, image.data(), &length);
        if (read) {
            const auto actual = test_canonical(
                fs::path(std::wstring(image.data(), length)));
            const auto expected = test_canonical(options_.settings_exe_path);
            if (!actual.empty() && !expected.empty() &&
                contained_by(product_root_, actual) &&
                actual.wstring() == expected.wstring()) {
                TerminateProcess(process, 0xfeedd00eUL);
                WaitForSingleObject(process, 1'000);
            }
        }
        CloseHandle(process);
    }

    void release() noexcept { active_ = false; }

private:
    doctor_host_test::Options options_;
    fs::path product_root_;
    bool active_ = true;
};

static int test_upgrade_retargets_enabled_owner_at_medium_integrity(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root =
        temporary.path() / L"product";
    const auto old_exe = copy_fixture(
        fixture_exe,
        product_root / L"versions" / L"old" /
            L"x64" / L"LiuKaiSettings.exe",
        product_root);
    const auto new_exe = copy_fixture(
        fixture_exe,
        product_root / L"versions" / L"new" /
            L"x64" / L"LiuKaiSettings.exe",
        product_root);
    CHECK(!old_exe.empty());
    CHECK(!new_exe.empty());
    const auto doctor_root = temporary.path() / L"doctor";
    CHECK(seed_history(doctor_root) == 0);
    const auto old_options =
        host_options(doctor_root, old_exe, registry);
    auto owner = start_owner(old_options);
    CHECK(owner);
    CHECK(wait_for_owner(old_options));
    CHECK(enable(old_options));
    HeldPrepareClient prepared(old_options, prepare_request());
    CHECK(prepared.valid());
    CHECK(prepared.result().failure ==
        DoctorHostClientFailure::None);
    CHECK(prepared.result().reply.failure ==
        DoctorStoreFailure::None);
    CHECK(prepared.result().server_pid == owner->pid());
    CHECK(prepared.client_running());
    CHECK(registry.delete_value(
        kDoctorHostRunValueName));
    CHECK(!registry.has_value(
        kDoctorHostRunValueName));
    CHECK(registry.set_value(
        L"OtherVendorSentinel", L"keep-me"));
    const auto foreign_file = temporary.path() / L"foreign.keep";
    CHECK(write_file(foreign_file, L"foreign"));

    review7_winapi::reset_mutex_observation();
    const auto stopped = installer::test::launch(
        lifecycle_options(
            product_root, old_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::StopForUpgrade);
    CHECK(launch_is_normal_medium(stopped));
    CHECK(review7_winapi::observed_open_mutex_calls() == 1);
    CHECK(review7_winapi::observed_create_mutex_calls() == 0);
    CHECK(owner->wait(kOwnerTimeoutMs));
    CHECK(wait_for_endpoint_unavailable(old_options));
    CHECK(prepared.release());
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    bool enabled = false;
    CHECK(stored_enabled(doctor_root, &enabled));
    CHECK(enabled);
    std::size_t records = 0;
    CHECK(record_count(doctor_root, &records));
    CHECK(records == 1);
    std::error_code error;
    CHECK(!fs::exists(doctor_root / L"temp", error) ||
        fs::is_empty(doctor_root / L"temp", error));
    CHECK(!error);
    std::wstring foreign_value;
    CHECK(registry.query_value(
        L"OtherVendorSentinel", &foreign_value));
    CHECK(foreign_value == L"keep-me");
    CHECK(read_file(foreign_file, &foreign_value));
    CHECK(foreign_value == L"foreign");

    const auto new_options =
        host_options(doctor_root, new_exe, registry);
    EndpointCleanup cleanup(new_options, product_root);
    const auto resumed = installer::test::launch(
        lifecycle_options(
            product_root, new_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::ResumeAfterUpgrade);
    CHECK(launch_is_normal_medium(resumed));
    CHECK(wait_for_owner(new_options));
    const auto status = send(
        new_options, command(DoctorHostCommand::Status));
    CHECK(status.failure == DoctorHostClientFailure::None);
    CHECK(status.reply.failure == DoctorStoreFailure::None);
    CHECK(status.reply.status.enabled);
    CHECK(status.server_pid != 0);
    CHECK(process_is_normal_medium(status.server_pid));
    std::wstring run_value;
    CHECK(registry.query_value(
        kDoctorHostRunValueName, &run_value));
    CHECK(run_value ==
        quote(new_exe.wstring()) + L" --doctor-collector");
    const auto removed = installer::test::launch(
        lifecycle_options(
            product_root, new_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::StopForRemoval);
    CHECK(launch_is_normal_medium(removed));
    CHECK(wait_for_endpoint_unavailable(new_options));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    CHECK(stored_enabled(doctor_root, &enabled));
    CHECK(enabled);
    records = 0;
    CHECK(record_count(doctor_root, &records));
    CHECK(records == 1);
    cleanup.release();
    return 0;
}

static int test_upgrade_disabled_stays_off(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root =
        temporary.path() / L"product";
    const auto old_exe = copy_fixture(
        fixture_exe,
        product_root / L"old" / L"LiuKaiSettings.exe",
        product_root);
    const auto new_exe = copy_fixture(
        fixture_exe,
        product_root / L"new" / L"LiuKaiSettings.exe",
        product_root);
    CHECK(!old_exe.empty());
    CHECK(!new_exe.empty());
    const auto doctor_root = temporary.path() / L"doctor";
    {
        doctor_host_detail::SystemDoctorClock clock;
        auto store = DoctorStore::open(
            doctor_root,
            kBuild,
            clock,
            DoctorStoreLimits::defaults());
        CHECK(store);
        CHECK(!store->status().enabled);
    }
    CHECK(registry.set_value(
        kDoctorHostRunValueName,
        quote(old_exe.wstring()) + L" --doctor-collector"));
    const auto old_options =
        host_options(doctor_root, old_exe, registry);
    const auto new_options =
        host_options(doctor_root, new_exe, registry);
    CHECK(wait_for_endpoint_unavailable(old_options));
    CHECK(registry.has_value(kDoctorHostRunValueName));
    EndpointCleanup old_cleanup(old_options, product_root);
    const auto stopped = installer::test::launch(
        lifecycle_options(
            product_root, old_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::StopForUpgrade);
    CHECK(launch_is_normal_medium(stopped));
    CHECK(wait_for_endpoint_unavailable(old_options));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    bool enabled = true;
    CHECK(stored_enabled(doctor_root, &enabled));
    CHECK(!enabled);
    old_cleanup.release();

    EndpointCleanup new_cleanup(new_options, product_root);
    const auto resumed = installer::test::launch(
        lifecycle_options(
            product_root, new_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::ResumeAfterUpgrade);
    CHECK(launch_is_normal_medium(resumed));
    CHECK(wait_for_endpoint_unavailable(new_options));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    CHECK(stored_enabled(doctor_root, &enabled));
    CHECK(!enabled);
    new_cleanup.release();
    return 0;
}

static int test_removal_cancels_preview_and_preserves_foreign_files(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root = temporary.path() / L"product";
    const auto settings_exe = copy_fixture(
        fixture_exe,
        product_root / L"versions" / L"current" /
            L"x64" / L"LiuKaiSettings.exe",
        product_root);
    CHECK(!settings_exe.empty());
    const auto doctor_root = temporary.path() / L"doctor";
    CHECK(seed_history(doctor_root) == 0);
    const auto options =
        host_options(doctor_root, settings_exe, registry);
    auto owner = start_owner(options);
    CHECK(owner);
    CHECK(wait_for_owner(options));
    CHECK(enable(options));
    HeldPrepareClient prepared(options, prepare_request());
    CHECK(prepared.valid());
    CHECK(prepared.result().failure ==
        DoctorHostClientFailure::None);
    CHECK(prepared.result().reply.failure ==
        DoctorStoreFailure::None);
    CHECK(prepared.result().server_pid == owner->pid());
    CHECK(prepared.client_running());
    CHECK(registry.delete_value(
        kDoctorHostRunValueName));
    CHECK(!registry.has_value(
        kDoctorHostRunValueName));
    CHECK(registry.set_value(
        L"OtherVendorSentinel", L"keep-me"));
    const auto foreign_file = doctor_root / L"foreign.keep";
    CHECK(write_file(foreign_file, L"foreign"));

    const auto removed = installer::test::launch(
        lifecycle_options(
            product_root, settings_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::StopForRemoval);
    CHECK(launch_is_normal_medium(removed));
    CHECK(owner->wait(kOwnerTimeoutMs));
    CHECK(wait_for_endpoint_unavailable(options));
    CHECK(prepared.release());
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    bool enabled = true;
    CHECK(stored_enabled(doctor_root, &enabled));
    CHECK(enabled);
    std::size_t records = 0;
    CHECK(record_count(doctor_root, &records));
    CHECK(records == 1);
    std::error_code error;
    CHECK(!fs::exists(doctor_root / L"temp", error) ||
        fs::is_empty(doctor_root / L"temp", error));
    CHECK(!error);
    std::wstring foreign_value;
    CHECK(registry.query_value(
        L"OtherVendorSentinel", &foreign_value));
    CHECK(foreign_value == L"keep-me");
    CHECK(read_file(foreign_file, &foreign_value));
    CHECK(foreign_value == L"foreign");
    return 0;
}

static int test_failed_helper_is_not_reported_as_lifecycle_success(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root = temporary.path() / L"product";
    const auto settings_exe = copy_fixture(
        fixture_exe,
        product_root / L"versions" / L"broken" /
            L"x64" / L"LiuKaiSettings.exe",
        product_root);
    CHECK(!settings_exe.empty());
    const auto blocked_parent = temporary.path() / L"blocked";
    CHECK(write_file(blocked_parent, L"not a directory"));
    const auto doctor_root = blocked_parent / L"doctor";
    auto options = lifecycle_options(
        product_root, settings_exe, doctor_root, registry);
    const auto failed = installer::test::launch(
        options,
        installer::DoctorLifecycleAction::ResumeAfterUpgrade);
    CHECK(FAILED(failed.result));
    CHECK(!failed.ok());
    CHECK(failed.disposition ==
        installer::DoctorLifecycleDisposition::Failed);
    CHECK(failed.helper_exit_code != 0);
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    const auto host_options_value =
        host_options(doctor_root, settings_exe, registry);
    CHECK(wait_for_endpoint_unavailable(host_options_value));
    return 0;
}

static int test_uncontained_settings_path_is_rejected(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root = temporary.path() / L"product";
    CHECK(fs::create_directory(product_root));
    const auto doctor_root = temporary.path() / L"doctor";
    const auto rejected = installer::test::launch(
        lifecycle_options(
            product_root, fixture_exe, doctor_root, registry),
        installer::DoctorLifecycleAction::StopForUpgrade);
    CHECK(FAILED(rejected.result));
    CHECK(!rejected.ok());
    CHECK(rejected.disposition ==
        installer::DoctorLifecycleDisposition::Failed);
    CHECK(!rejected.same_user);
    CHECK(!rejected.used_linked_token);
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    return 0;
}

static int test_off_no_owner_no_run_stop_is_not_needed(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root =
        temporary.path() / L"product";
    const auto settings_exe = copy_fixture(
        fixture_exe,
        product_root / L"versions" / L"off" /
            L"x64" / L"LiuKaiSettings.exe",
        product_root);
    CHECK(!settings_exe.empty());
    const auto doctor_root =
        temporary.path() / L"doctor-does-not-exist";
    CHECK(!fs::exists(doctor_root));
    CHECK(!registry.has_value(kDoctorHostRunValueName));

    for (const auto action : {
            installer::DoctorLifecycleAction::StopForUpgrade,
            installer::DoctorLifecycleAction::StopForRemoval}) {
        review7_winapi::reset_mutex_observation();
        const auto stopped = installer::test::launch(
            lifecycle_options(
                product_root,
                settings_exe,
                doctor_root,
                registry),
            action);
        CHECK(launch_is_not_needed(stopped));
        CHECK(review7_winapi::observed_open_mutex_calls() == 1);
        CHECK(review7_winapi::observed_create_mutex_calls() == 0);
        CHECK(!fs::exists(doctor_root));
        CHECK(!registry.has_value(
            kDoctorHostRunValueName));
    }
    return 0;
}

static int test_stop_preflight_error_is_not_treated_as_absence(
    const fs::path &fixture_exe) {
    OwnedTempDirectory temporary;
    TemporaryRegistryKey registry;
    const auto product_root =
        temporary.path() / L"product";
    const auto settings_exe = copy_fixture(
        fixture_exe,
        product_root / L"versions" / L"invalid-run" /
            L"x64" / L"LiuKaiSettings.exe",
        product_root);
    CHECK(!settings_exe.empty());
    const auto doctor_root =
        temporary.path() / L"doctor-does-not-exist";
    auto options = lifecycle_options(
        product_root, settings_exe, doctor_root, registry);
    options.run_key_path = L"\\invalid-test-run-path";
    const auto stopped = installer::test::launch(
        options,
        installer::DoctorLifecycleAction::StopForRemoval);
    CHECK(FAILED(stopped.result));
    CHECK(!stopped.ok());
    CHECK(stopped.disposition ==
        installer::DoctorLifecycleDisposition::Failed);
    CHECK(stopped.helper_exit_code == 0xffffffffUL);
    CHECK(!fs::exists(doctor_root));
    CHECK(!registry.has_value(kDoctorHostRunValueName));
    return 0;
}

struct TestCase {
    const wchar_t *name;
    int (*run)(const fs::path &);
};

static int test_original_token_policy(const fs::path &) {
    // Pure policy boundary only; this is not a claimed elevated process run.
    installer::doctor_lifecycle_detail::TokenContext context;
    context.user_sid = L"S-1-5-21-1-2-3-1000";
    context.session_id = 1;
    context.integrity_rid = SECURITY_MANDATORY_HIGH_RID;
    context.app_container = false;
    context.elevated = false;
    context.result = S_OK;
    CHECK(!installer::doctor_lifecycle_detail::trusted_original_caller(
        context, context.user_sid, true));
    installer::DoctorLifecycleResult result;
    result.result = S_OK;
    result.same_user = true;
    result.integrity_rid = SECURITY_MANDATORY_HIGH_RID;
    result.elevated = false;
    result.disposition = installer::DoctorLifecycleDisposition::StopNotNeeded;
    CHECK(!result.ok());
    return 0;
}

constexpr TestCase kTests[] = {
    {L"original-token-policy", test_original_token_policy},
    {
        L"primary-token-no-shell",
        test_medium_primary_token_accepts_normal_primary,
    },
    {
        L"impersonation-token-no-shell",
        test_medium_primary_token_accepts_impersonation,
    },
    {
        L"identification-token-shell-fallback",
        test_medium_primary_token_uses_shell_for_identification,
    },
    {
        L"medium-primary-rejects-unverified",
        test_medium_primary_token_rejects_unverified_sources,
    },
    {
        L"off-no-owner-no-run-stop-not-needed",
        test_off_no_owner_no_run_stop_is_not_needed,
    },
    {
        L"stop-preflight-error-not-absence",
        test_stop_preflight_error_is_not_treated_as_absence,
    },
    {
        L"upgrade-enabled-retarget-medium",
        test_upgrade_retargets_enabled_owner_at_medium_integrity,
    },
    {
        L"upgrade-disabled-stays-off",
        test_upgrade_disabled_stays_off,
    },
    {
        L"removal-cancels-preview-preserves-foreign",
        test_removal_cancels_preview_and_preserves_foreign_files,
    },
    {
        L"failed-helper-is-not-success",
        test_failed_helper_is_not_reported_as_lifecycle_success,
    },
    {
        L"uncontained-settings-path-rejected",
        test_uncontained_settings_path_is_rejected,
    },
};

} // namespace

int wmain(int argc, wchar_t **argv) {
    std::cout << std::unitbuf;
    if (!current_process_is_normal_medium() ||
        (argc != 2 && argc != 3)) {
        return 2;
    }
    const auto fixture_exe = test_canonical(fs::path(argv[1]));
    if (fixture_exe.empty() || !fixture_exe.is_absolute() ||
        !fs::is_regular_file(fixture_exe)) {
        return 2;
    }
    const std::wstring selected =
        argc == 3 ? argv[2] : std::wstring();
    bool found = selected.empty();
    for (const auto &test : kTests) {
        if (!selected.empty() && selected != test.name) continue;
        found = true;
        const int result = test.run(fixture_exe);
        if (result != 0) {
            std::wcerr << L"FAILED: " << test.name << L'\n';
            return result;
        }
        std::wcout << L"PASS: " << test.name << L'\n';
    }
    if (!found) {
        std::wcerr << L"Unknown doctor installer test case: "
                   << selected << L'\n';
        return 2;
    }
    return 0;
}
