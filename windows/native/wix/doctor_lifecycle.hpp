#ifndef BOSHIAMY_WINDOWS_WIX_DOCTOR_LIFECYCLE_HPP
#define BOSHIAMY_WINDOWS_WIX_DOCTOR_LIFECYCLE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../doctor_host.hpp"
#include "../installer/file_deployment.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace boshiamy::doctor::installer {

enum class DoctorLifecycleAction : std::uint32_t {
    StopForUpgrade = 1,
    StopForRemoval = 2,
    ResumeAfterUpgrade = 3,
};

enum class DoctorLifecycleDisposition : std::uint32_t {
    Failed = 0,
    HelperCompleted = 1,
    StopNotNeeded = 2,
};

struct DoctorLifecycleResult {
    HRESULT result = E_UNEXPECTED;
    DWORD helper_exit_code = 0xffffffffUL;
    bool same_user = false;
    bool used_linked_token = false;
    bool used_shell_token = false;
    bool elevated = true;
    std::uint32_t integrity_rid = 0;
    DoctorLifecycleDisposition disposition =
        DoctorLifecycleDisposition::Failed;

    bool ok() const noexcept {
        if (FAILED(result) || !same_user) return false;
        if (disposition ==
            DoctorLifecycleDisposition::HelperCompleted) {
            return helper_exit_code == 0 &&
                !elevated &&
                integrity_rid ==
                    static_cast<std::uint32_t>(
                        SECURITY_MANDATORY_MEDIUM_RID);
        }
        if (disposition ==
            DoctorLifecycleDisposition::StopNotNeeded) {
            if (helper_exit_code != 0xffffffffUL ||
                used_linked_token ||
                used_shell_token) {
                return false;
            }
            return (elevated && integrity_rid ==
                    static_cast<std::uint32_t>(
                        SECURITY_MANDATORY_HIGH_RID)) ||
                (!elevated &&
                    integrity_rid ==
                        static_cast<std::uint32_t>(
                            SECURITY_MANDATORY_MEDIUM_RID));
        }
        return false;
    }
};

struct DoctorLifecycleTarget {
    std::filesystem::path product_root;
    std::filesystem::path settings_exe_path;
    std::string settings_sha256;
    std::wstring expected_user_sid;
    DWORD timeout_ms = 0;
};

namespace doctor_lifecycle_detail {

struct TokenContext {
    std::wstring user_sid;
    DWORD session_id = 0;
    bool elevated = true;
    std::uint32_t integrity_rid = 0;
    bool app_container = true;
    HRESULT result = E_UNEXPECTED;
};

struct UniqueHandle {
    HANDLE value = nullptr;

    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) {
            if (value) CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (value) CloseHandle(value);
        value = replacement;
    }
    ~UniqueHandle() {
        reset();
    }
};

class UserEnvironment {
public:
    UserEnvironment() = default;
    UserEnvironment(const UserEnvironment &) = delete;
    UserEnvironment &operator=(
        const UserEnvironment &) = delete;

    ~UserEnvironment() {
        reset();
        if (module_) FreeLibrary(module_);
    }

    bool create(HANDLE token) {
        reset();
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        module_ = LoadLibraryExW(
            L"userenv.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return false;
        create_ = reinterpret_cast<CreateFunction>(
            GetProcAddress(module_, "CreateEnvironmentBlock"));
        destroy_ = reinterpret_cast<DestroyFunction>(
            GetProcAddress(module_, "DestroyEnvironmentBlock"));
        if (!create_ || !destroy_) return false;
        if (!create_(&block_, token, FALSE)) {
            block_ = nullptr;
            return false;
        }
        return true;
    }

    void *block() const noexcept { return block_; }

private:
    using CreateFunction =
        BOOL (WINAPI *)(LPVOID *, HANDLE, BOOL);
    using DestroyFunction = BOOL (WINAPI *)(LPVOID);

    void reset() {
        if (block_) {
            if (destroy_) destroy_(block_);
            block_ = nullptr;
        }
        create_ = nullptr;
        destroy_ = nullptr;
    }

    HMODULE module_ = nullptr;
    LPVOID block_ = nullptr;
    CreateFunction create_ = nullptr;
    DestroyFunction destroy_ = nullptr;
};

inline HRESULT last_hresult() noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS
        ? E_FAIL
        : HRESULT_FROM_WIN32(error);
}

inline TokenContext token_context(HANDLE token) {
    TokenContext result;
    if (!token) {
        result.result = E_INVALIDARG;
        return result;
    }
    if (!boshiamy::doctor::detail::token_user_sid(
            token, &result.user_sid)) {
        result.result = last_hresult();
        return result;
    }
    HRESULT status = boshiamy::doctor::detail::token_session_id(
        token, &result.session_id);
    if (FAILED(status)) {
        result.result = status;
        return result;
    }
    const auto identity =
        boshiamy::doctor::detail::token_identity(token);
    if (identity.result_hr != static_cast<std::int32_t>(S_OK)) {
        result.result =
            static_cast<HRESULT>(identity.result_hr);
        return result;
    }
    result.integrity_rid = identity.integrity_rid;
    result.app_container = identity.app_container != 0;
    TOKEN_ELEVATION elevation{};
    DWORD copied = 0;
    if (!GetTokenInformation(
            token,
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &copied)) {
        result.result = last_hresult();
        return result;
    }
    result.elevated = elevation.TokenIsElevated != 0;
    result.result = S_OK;
    return result;
}

inline bool normal_same_user(
    const TokenContext &context,
    const std::wstring &expected_sid,
    DWORD expected_session) {
    return SUCCEEDED(context.result) &&
        !context.user_sid.empty() &&
        context.user_sid == expected_sid &&
        context.session_id != 0 &&
        context.session_id == expected_session &&
        !context.elevated &&
        !context.app_container &&
        context.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_MEDIUM_RID);
}

inline bool same_path(
    const std::filesystem::path &left,
    const std::filesystem::path &right);

inline HRESULT token_type(
    HANDLE token, TOKEN_TYPE *type) {
    if (!token || !type) return E_INVALIDARG;
    DWORD copied = 0;
    return GetTokenInformation(
        token, TokenType, type, sizeof(*type), &copied)
        ? S_OK
        : last_hresult();
}

inline HRESULT token_impersonation_level(
    HANDLE token,
    SECURITY_IMPERSONATION_LEVEL *level) {
    if (!token || !level) return E_INVALIDARG;
    DWORD copied = 0;
    return GetTokenInformation(
        token,
        TokenImpersonationLevel,
        level,
        sizeof(*level),
        &copied)
        ? S_OK
        : last_hresult();
}

inline HRESULT duplicate_verified_primary_token(
    HANDLE selected,
    const std::wstring &expected_sid,
    DWORD expected_session,
    UniqueHandle *primary) {
    if (!selected ||
        expected_sid.empty() ||
        expected_session == 0 ||
        !primary) {
        return E_INVALIDARG;
    }
    primary->reset();
    UniqueHandle candidate;
    // The observed CreateProcessWithTokenW guest path requires both adjustment
    // rights on the fresh primary handle.
    if (!DuplicateTokenEx(
            selected,
            TOKEN_ASSIGN_PRIMARY |
                TOKEN_QUERY |
                TOKEN_DUPLICATE |
                TOKEN_ADJUST_DEFAULT |
                TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &candidate.value)) {
        return last_hresult();
    }
    const auto context = token_context(candidate.value);
    TOKEN_TYPE type{};
    const HRESULT type_result =
        token_type(candidate.value, &type);
    if (FAILED(type_result)) return type_result;
    if (type != TokenPrimary ||
        !normal_same_user(
            context,
            expected_sid,
            expected_session)) {
        return E_ACCESSDENIED;
    }
    *primary = std::move(candidate);
    return S_OK;
}

inline HRESULT process_image_path(
    HANDLE process,
    std::filesystem::path *path) {
    if (!process || !path) return E_INVALIDARG;
    std::vector<wchar_t> buffer(4096);
    for (;;) {
        DWORD length = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(
                process, 0, buffer.data(), &length)) {
            *path = std::filesystem::path(
                std::wstring(buffer.data(), length));
            return S_OK;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            buffer.size() >= 32768) {
            return last_hresult();
        }
        buffer.resize(buffer.size() * 2);
    }
}

inline HRESULT windows_explorer_image(
    std::filesystem::path *path) {
    if (!path) return E_INVALIDARG;
    std::vector<wchar_t> directory(32768);
    const UINT length = GetWindowsDirectoryW(
        directory.data(),
        static_cast<UINT>(directory.size()));
    if (!length || length >= directory.size()) {
        return last_hresult();
    }
    const auto expected =
        std::filesystem::path(
            std::wstring(directory.data(), length)) /
        L"explorer.exe";
    if (!boshiamy::installer::files::detail::absolute_no_reparse(
            expected, true)) {
        return E_ACCESSDENIED;
    }
    std::error_code error;
    *path = std::filesystem::weakly_canonical(
        expected, error);
    if (error || !path->is_absolute()) {
        return error
            ? HRESULT_FROM_WIN32(
                static_cast<DWORD>(error.value()))
            : E_ACCESSDENIED;
    }
    return S_OK;
}

inline HRESULT medium_primary_token(
    HANDLE selected,
    const std::wstring &expected_sid,
    DWORD expected_session,
    UniqueHandle *primary,
    bool *used_shell_token) {
    if (used_shell_token) *used_shell_token = false;
    if (primary) primary->reset();
    if (!used_shell_token || !primary) return E_POINTER;
    if (!selected ||
        expected_sid.empty() ||
        expected_session == 0) {
        return E_INVALIDARG;
    }

    const auto selected_context = token_context(selected);
    if (!normal_same_user(
            selected_context,
            expected_sid,
            expected_session)) {
        return E_ACCESSDENIED;
    }

    TOKEN_TYPE type{};
    HRESULT result = token_type(selected, &type);
    if (FAILED(result)) return result;
    if (type == TokenPrimary) {
        return duplicate_verified_primary_token(
            selected,
            expected_sid,
            expected_session,
            primary);
    }
    if (type != TokenImpersonation) return E_ACCESSDENIED;

    SECURITY_IMPERSONATION_LEVEL level =
        SecurityAnonymous;
    result = token_impersonation_level(selected, &level);
    if (FAILED(result)) return result;
    if (level >= SecurityImpersonation) {
        return duplicate_verified_primary_token(
            selected,
            expected_sid,
            expected_session,
            primary);
    }
    if (level != SecurityIdentification) {
        return E_ACCESSDENIED;
    }

    const HWND shell_window = GetShellWindow();
    if (!shell_window) return last_hresult();
    DWORD shell_pid = 0;
    if (!GetWindowThreadProcessId(
            shell_window, &shell_pid) ||
        shell_pid == 0) {
        return last_hresult();
    }
    DWORD shell_session = 0;
    if (!ProcessIdToSessionId(
            shell_pid, &shell_session) ||
        shell_session != expected_session) {
        return E_ACCESSDENIED;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        shell_pid));
    if (!process.value) return last_hresult();
    std::filesystem::path actual_image;
    result = process_image_path(
        process.value, &actual_image);
    if (FAILED(result)) return result;
    std::filesystem::path expected_image;
    result = windows_explorer_image(&expected_image);
    if (FAILED(result)) return result;
    if (!boshiamy::installer::files::detail::absolute_no_reparse(
            actual_image, true) ||
        !same_path(actual_image, expected_image)) {
        return E_ACCESSDENIED;
    }

    UniqueHandle shell_token;
    if (!OpenProcessToken(
            process.value,
            TOKEN_QUERY | TOKEN_DUPLICATE,
            &shell_token.value)) {
        return last_hresult();
    }
    const auto shell_context =
        token_context(shell_token.value);
    if (!normal_same_user(
            shell_context,
            expected_sid,
            expected_session)) {
        return E_ACCESSDENIED;
    }
    TOKEN_TYPE shell_type{};
    result = token_type(
        shell_token.value, &shell_type);
    if (FAILED(result)) return result;
    if (shell_type != TokenPrimary) return E_ACCESSDENIED;

    result = duplicate_verified_primary_token(
        shell_token.value,
        expected_sid,
        expected_session,
        primary);
    if (FAILED(result)) return result;
    *used_shell_token = true;
    return S_OK;
}

inline bool trusted_original_caller(
    const TokenContext &context,
    const std::wstring &expected_sid,
    bool for_stop_preflight) {
    if (FAILED(context.result) ||
        context.user_sid.empty() ||
        context.user_sid != expected_sid ||
        context.session_id == 0 ||
        context.app_container ||
        context.user_sid == L"S-1-5-18" ||
        context.user_sid == L"S-1-5-19" ||
        context.user_sid == L"S-1-5-20") {
        return false;
    }
    if (!for_stop_preflight) return true;
    return (context.elevated && context.integrity_rid ==
            static_cast<std::uint32_t>(
                SECURITY_MANDATORY_HIGH_RID)) ||
        (!context.elevated &&
            context.integrity_rid ==
                static_cast<std::uint32_t>(
                    SECURITY_MANDATORY_MEDIUM_RID));
}

inline HRESULT product_run_value_absent(
    const std::wstring &user_sid,
    const std::wstring &run_key_path,
    bool *absent) {
    if (!absent ||
        user_sid.empty() ||
        run_key_path.empty() ||
        run_key_path.front() == L'\\') {
        return E_INVALIDARG;
    }
    *absent = false;
    const std::wstring full_path =
        user_sid + L"\\" + run_key_path;
    HKEY key = nullptr;
    const LONG opened = RegOpenKeyExW(
        HKEY_USERS,
        full_path.c_str(),
        0,
        KEY_QUERY_VALUE,
        &key);
    if (opened == ERROR_FILE_NOT_FOUND ||
        opened == ERROR_PATH_NOT_FOUND) {
        *absent = true;
        return S_OK;
    }
    if (opened != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(opened);
    }
    DWORD type = 0;
    DWORD bytes = 0;
    const LONG queried = RegQueryValueExW(
        key,
        kDoctorHostRunValueName,
        nullptr,
        &type,
        nullptr,
        &bytes);
    RegCloseKey(key);
    if (queried == ERROR_FILE_NOT_FOUND ||
        queried == ERROR_PATH_NOT_FOUND) {
        *absent = true;
        return S_OK;
    }
    return queried == ERROR_SUCCESS
        ? S_FALSE
        : HRESULT_FROM_WIN32(queried);
}

enum class StopPreflightState : std::uint32_t {
    StopNotNeeded = 1,
    HelperRequired = 2,
    Failed = 3,
};

struct StopPreflightResult {
    StopPreflightState state =
        StopPreflightState::Failed;
    HRESULT result = E_UNEXPECTED;
};

inline StopPreflightResult stop_preflight(
    const std::wstring &user_sid,
    const std::wstring &run_key_path) {
    const auto mutex_name =
        DoctorRuntimeOwnership::mutex_name(user_sid);
    if (mutex_name.empty()) {
        return {
            StopPreflightState::Failed,
            E_INVALIDARG,
        };
    }
    UniqueHandle writer;
    writer.value = OpenMutexW(
        SYNCHRONIZE | READ_CONTROL,
        FALSE,
        mutex_name.c_str());
    if (!writer.value) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            return {
                StopPreflightState::Failed,
                HRESULT_FROM_WIN32(error),
            };
        }
    } else {
        auto security =
            boshiamy::doctor::UserSecurity::create_mutex();
        if (FAILED(security.result) ||
            !doctor_runtime_detail::last_dacl_matches(
                writer.value, security.descriptor)) {
            return {
                StopPreflightState::Failed,
                SUCCEEDED(security.result)
                    ? E_ACCESSDENIED
                    : security.result,
            };
        }
    }
    bool absent = false;
    const HRESULT queried =
        product_run_value_absent(
            user_sid, run_key_path, &absent);
    if (FAILED(queried)) {
        return {
            StopPreflightState::Failed,
            queried,
        };
    }
    return {
        !writer.value && absent
            ? StopPreflightState::StopNotNeeded
            : StopPreflightState::HelperRequired,
        S_OK,
    };
}

inline bool action_argument(
    DoctorLifecycleAction action,
    const wchar_t **argument) {
    if (!argument) return false;
    switch (action) {
    case DoctorLifecycleAction::StopForUpgrade:
        *argument = kDoctorMaintenanceStopUpgradeArgument;
        return true;
    case DoctorLifecycleAction::StopForRemoval:
        *argument = kDoctorMaintenanceStopRemovalArgument;
        return true;
    case DoctorLifecycleAction::ResumeAfterUpgrade:
        *argument = kDoctorMaintenanceResumeArgument;
        return true;
    }
    return false;
}

inline bool canonical_product_file(
    const DoctorLifecycleTarget &target,
    bool verify_hash,
    std::filesystem::path *product_root,
    std::filesystem::path *settings_exe) {
    if (!product_root || !settings_exe ||
        !target.product_root.is_absolute() ||
        !target.settings_exe_path.is_absolute() ||
        !boshiamy::installer::files::detail::absolute_no_reparse(
            target.product_root, true) ||
        !boshiamy::installer::files::detail::absolute_no_reparse(
            target.settings_exe_path, true)) {
        return false;
    }
    std::error_code error;
    *product_root =
        std::filesystem::weakly_canonical(
            target.product_root, error);
    if (error) return false;
    error.clear();
    *settings_exe =
        std::filesystem::weakly_canonical(
            target.settings_exe_path, error);
    if (error ||
        !std::filesystem::is_directory(*product_root, error) ||
        error ||
        !std::filesystem::is_regular_file(*settings_exe, error) ||
        error ||
        !boshiamy::installer::files::detail::is_within(
            *product_root, *settings_exe) ||
        settings_exe->filename().wstring() !=
            L"LiuKaiSettings.exe") {
        return false;
    }
    if (verify_hash) {
        std::vector<std::filesystem::path> relative_parts;
        for (const auto &part :
                settings_exe->lexically_relative(*product_root)) {
            relative_parts.push_back(part);
        }
        if (relative_parts.size() != 4 ||
            relative_parts[0] != L"versions" ||
            relative_parts[1].empty() ||
            relative_parts[1] == L"." ||
            relative_parts[1] == L".." ||
            relative_parts[2] != L"x64" ||
            relative_parts[3] != L"LiuKaiSettings.exe") {
            return false;
        }
        if (!boshiamy::installer::files::detail::valid_hash(
                target.settings_sha256) ||
            boshiamy::installer::files::detail::sha256_file(
                *settings_exe) != target.settings_sha256) {
            return false;
        }
    }
    return true;
}

inline bool same_path(
    const std::filesystem::path &left,
    const std::filesystem::path &right) {
    std::error_code error;
    const auto canonical_left =
        std::filesystem::weakly_canonical(left, error);
    if (error) return false;
    error.clear();
    const auto canonical_right =
        std::filesystem::weakly_canonical(right, error);
    return !error &&
        CompareStringOrdinal(
            canonical_left.c_str(),
            static_cast<int>(canonical_left.wstring().size()),
            canonical_right.c_str(),
            static_cast<int>(canonical_right.wstring().size()),
            TRUE) == CSTR_EQUAL;
}

inline bool query_child_context(
    HANDLE process,
    const std::filesystem::path &expected_executable,
    const std::wstring &expected_sid,
    DWORD expected_session,
    DoctorLifecycleResult *result) {
    if (!process || !result) return false;
    std::vector<wchar_t> image(4096);
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(
            process, 0, image.data(), &length)) {
        result->result = last_hresult();
        return false;
    }
    if (!same_path(
            std::filesystem::path(
                std::wstring(image.data(), length)),
            expected_executable)) {
        result->result = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return false;
    }
    UniqueHandle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) {
        result->result = last_hresult();
        return false;
    }
    const auto context = token_context(token.value);
    result->same_user =
        normal_same_user(
            context, expected_sid, expected_session);
    result->elevated = context.elevated;
    result->integrity_rid = context.integrity_rid;
    if (!result->same_user) {
        result->result = E_ACCESSDENIED;
        return false;
    }
    return true;
}

inline bool make_command_line(
    const std::filesystem::path &executable,
    const wchar_t *argument,
    std::vector<wchar_t> *command) {
    if (!argument || !command) return false;
    const auto path = executable.wstring();
    if (path.empty() ||
        path.find(L'"') != std::wstring::npos) {
        return false;
    }
    const std::wstring value =
        L"\"" + path + L"\" " + argument;
    command->assign(value.begin(), value.end());
    command->push_back(L'\0');
    return true;
}

inline DoctorLifecycleResult launch_impl(
    const DoctorLifecycleTarget &target,
    DoctorLifecycleAction action,
    const std::vector<wchar_t> *test_environment,
    bool verify_hash,
    const std::wstring *test_run_key_path = nullptr) {
    DoctorLifecycleResult result;
    const wchar_t *argument = nullptr;
    if (!action_argument(action, &argument) ||
        target.expected_user_sid.empty() ||
        target.timeout_ms == 0) {
        result.result = E_INVALIDARG;
        return result;
    }
    std::filesystem::path product_root;
    std::filesystem::path settings_exe;
    if (!canonical_product_file(
            target,
            verify_hash,
            &product_root,
            &settings_exe)) {
        result.result = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return result;
    }
    std::vector<wchar_t> command;
    if (!make_command_line(settings_exe, argument, &command)) {
        result.result = E_INVALIDARG;
        return result;
    }

    UniqueHandle process_token;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_DUPLICATE,
            &process_token.value)) {
        result.result = last_hresult();
        return result;
    }
    const auto original =
        token_context(process_token.value);
    const bool stop_action =
        action == DoctorLifecycleAction::StopForUpgrade ||
        action == DoctorLifecycleAction::StopForRemoval;
    if (!trusted_original_caller(
            original,
            target.expected_user_sid,
            stop_action)) {
        result.result = E_ACCESSDENIED;
        return result;
    }

#ifdef BOSHIAMY_DOCTOR_INSTALLER_TESTING
    const std::wstring run_key_path =
        test_run_key_path
            ? *test_run_key_path
            : std::wstring(kDoctorHostProductRunKey);
#else
    (void)test_run_key_path;
    const std::wstring run_key_path =
        kDoctorHostProductRunKey;
#endif
    result.same_user = true;
    result.elevated = original.elevated;
    result.integrity_rid = original.integrity_rid;
    if (stop_action) {
        const auto preflight =
            stop_preflight(
                original.user_sid, run_key_path);
        if (preflight.state ==
            StopPreflightState::Failed) {
            result.result = preflight.result;
            return result;
        }
        if (preflight.state ==
            StopPreflightState::StopNotNeeded) {
            result.result = S_OK;
            result.disposition =
                DoctorLifecycleDisposition::StopNotNeeded;
            return result;
        }
    }

    HANDLE selected_token = process_token.value;
    UniqueHandle linked_token;
    bool use_linked_token = false;
    if (original.elevated) {
        TOKEN_LINKED_TOKEN linked{};
        DWORD copied = 0;
        if (!GetTokenInformation(
                process_token.value,
                TokenLinkedToken,
                &linked,
                sizeof(linked),
                &copied)) {
            result.result = last_hresult();
            return result;
        }
        linked_token.value = linked.LinkedToken;
        selected_token = linked_token.value;
        use_linked_token = true;
    }
    const auto selected = token_context(selected_token);
    if (!normal_same_user(
            selected,
            target.expected_user_sid,
            original.session_id)) {
        result.result = E_ACCESSDENIED;
        return result;
    }

    result.used_linked_token = use_linked_token;
    result.elevated = false;
    result.integrity_rid = selected.integrity_rid;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    DWORD creation_flags =
        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    BOOL created = FALSE;

    if (use_linked_token) {
        UniqueHandle primary_token;
        bool used_shell_token = false;
        const HRESULT converted =
            medium_primary_token(
                selected_token,
                target.expected_user_sid,
                original.session_id,
                &primary_token,
                &used_shell_token);
        result.used_shell_token =
            used_shell_token;
        if (FAILED(converted)) {
            result.result = converted;
            return result;
        }
        UserEnvironment environment;
        if (!environment.create(primary_token.value)) {
            result.result = last_hresult();
            return result;
        }
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
        created = CreateProcessAsUserW(
            primary_token.value,
            settings_exe.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            creation_flags,
            environment.block(),
            nullptr,
            &startup,
            &process);
        if (!created &&
            (GetLastError() == ERROR_ACCESS_DENIED ||
                GetLastError() == ERROR_PRIVILEGE_NOT_HELD)) {
            created = CreateProcessWithTokenW(
                primary_token.value,
                0,
                settings_exe.c_str(),
                command.data(),
                creation_flags,
                environment.block(),
                nullptr,
                &startup,
                &process);
        }
    } else {
        LPVOID environment = nullptr;
        if (test_environment) {
            creation_flags |= CREATE_UNICODE_ENVIRONMENT;
            environment =
                const_cast<wchar_t *>(test_environment->data());
        }
        created = CreateProcessW(
            settings_exe.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            creation_flags,
            environment,
            nullptr,
            &startup,
            &process);
    }
    if (!created) {
        result.result = last_hresult();
        return result;
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    if (!query_child_context(
            process_handle.value,
            settings_exe,
            target.expected_user_sid,
            original.session_id,
            &result)) {
        return result;
    }

    const DWORD wait = WaitForSingleObject(
        process_handle.value, target.timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        result.result = wait == WAIT_FAILED
            ? last_hresult()
            : HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        return result;
    }
    if (!GetExitCodeProcess(
            process_handle.value, &result.helper_exit_code)) {
        result.result = last_hresult();
        return result;
    }
    if (result.helper_exit_code != 0) {
        result.result = E_FAIL;
        return result;
    }
    result.result = S_OK;
    result.disposition =
        DoctorLifecycleDisposition::HelperCompleted;
    return result;
}

} // namespace doctor_lifecycle_detail

inline DoctorLifecycleResult launch_as_same_user_medium(
    const DoctorLifecycleTarget &target,
    DoctorLifecycleAction action) {
    return doctor_lifecycle_detail::launch_impl(
        target, action, nullptr, true, nullptr);
}

#ifdef BOSHIAMY_DOCTOR_INSTALLER_TESTING
namespace test {

struct Options {
    std::filesystem::path product_root;
    std::filesystem::path settings_exe_path;
    std::filesystem::path doctor_root;
    boshiamy::doctor::DoctorBuildMetadata build;
    std::wstring run_key_path;
    DWORD timeout_ms = 0;
};

inline DoctorLifecycleResult launch(
    const Options &options,
    DoctorLifecycleAction action) {
    std::wstring user_sid;
    if (!boshiamy::doctor::detail::own_user_sid(&user_sid)) {
        DoctorLifecycleResult result;
        result.result = E_ACCESSDENIED;
        return result;
    }
    doctor_host_test::Options host_options;
    host_options.root = options.doctor_root;
    host_options.build = options.build;
    host_options.settings_exe_path =
        options.settings_exe_path;
    host_options.run_key_path = options.run_key_path;
    host_options.idle_timeout_ms = 30'000;
    const auto environment =
        doctor_host_detail::test_environment_block(
            doctor_host_test::host_options(host_options));
    return doctor_lifecycle_detail::launch_impl(
        DoctorLifecycleTarget{
            options.product_root,
            options.settings_exe_path,
            {},
            user_sid,
            options.timeout_ms,
        },
        action,
        &environment,
        false,
        &options.run_key_path);
}

} // namespace test
#endif

} // namespace boshiamy::doctor::installer

#endif
