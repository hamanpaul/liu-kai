#ifndef BOSHIAMY_TSF_DOCTOR_TRANSPORT_HPP
#define BOSHIAMY_TSF_DOCTOR_TRANSPORT_HPP

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <securityappcontainer.h>
#include <tlhelp32.h>

#include <array>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace boshiamy::doctor {

inline constexpr std::uint32_t kProtocolVersion = 2;
inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::size_t kScopeTokenCapacity = 8;
inline constexpr std::size_t kQueueCapacity = 256;
inline constexpr std::uint64_t kCollectorHeartbeatTimeoutMs = 5000;

enum class ScopeToken : std::uint32_t {
    Default = 0,
    Text = 1,
    Number = 2,
    Private = 3,
    Password = 4,
    Pin = 5,
    Unsupported = 6
};

enum class ScopeClass : std::uint32_t { Unknown = 0, Ordinary = 1, Sensitive = 2 };
enum class WriteResult : std::uint32_t { Written, Disabled, Full, Invalid };
enum class ReadResult : std::uint32_t { Read, Empty, Invalid };
enum class Event : std::uint32_t {
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
    ShiftUp = 13
};

enum class HostKind : std::uint32_t {
    Other = 0,
    WindowsTerminal = 1,
    Conhost = 2,
    Code = 3,
    Chrome = 4,
    Notepad = 5,
    SearchHost = 6,
    MicrosoftStore = 7,
    WslHost = 8
};

struct ScopeObservation {
    std::int32_t query_result = static_cast<std::int32_t>(E_UNEXPECTED);
    std::int32_t app_property_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t request_outer_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t request_inner_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t range_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t property_value_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t unpack_hr = static_cast<std::int32_t>(E_PENDING);
    std::uint32_t variant_type = 0;
    std::uint32_t token_count = 0;
    bool context_available = false;
    bool context_disabled = false;
    bool truncated = false;
    std::array<ScopeToken, kScopeTokenCapacity> tokens{};
    std::array<std::uint32_t, kScopeTokenCapacity> raw_tokens{};
};

inline ScopeClass classify_scope(const ScopeObservation &scope) noexcept {
    if (!scope.context_available) return ScopeClass::Unknown;
    if (scope.context_disabled) return ScopeClass::Sensitive;
    if (FAILED(static_cast<HRESULT>(scope.query_result)) ||
        scope.token_count == 0 || scope.token_count > kScopeTokenCapacity || scope.truncated)
        return ScopeClass::Unknown;
    bool unknown = false;
    bool sensitive = false;
    for (std::size_t i = 0; i < scope.token_count; ++i) {
        switch (scope.tokens[i]) {
        case ScopeToken::Private:
        case ScopeToken::Password:
        case ScopeToken::Pin:
            sensitive = true;
            break;
        case ScopeToken::Default:
        case ScopeToken::Text:
        case ScopeToken::Number:
            break;
        default:
            unknown = true;
            break;
        }
    }
    if (sensitive) return ScopeClass::Sensitive;
    return unknown ? ScopeClass::Unknown : ScopeClass::Ordinary;
}

struct alignas(8) EventRecord {
    std::uint64_t producer_id = 0;
    std::uint64_t operation_id = 0;
    std::uint64_t sequence = 0;
    std::uint64_t monotonic_ns = 0;
    std::uint32_t schema_version = 0;
    std::uint32_t event_code = 0;
    std::uint32_t callback_source = 0;
    std::uint32_t scope = 0;
    std::int32_t result_code = static_cast<std::int32_t>(E_PENDING);
    std::uint32_t duration_us = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(EventRecord) == 64, "Doctor event ABI must stay 64 bytes");
static_assert(std::is_trivially_copyable_v<EventRecord>);
static_assert(std::is_standard_layout_v<EventRecord>);

struct QueueSnapshot {
    bool valid = false;
    std::uint64_t epoch = 0;
    std::uint64_t heartbeat_ms = 0;
    std::uint64_t producer_id = 0;
    std::uint64_t process_creation = 0;
    std::uint32_t process_id = 0;
    std::uint32_t host_kind = 0;
    std::uint32_t integrity_rid = 0;
    std::uint32_t app_container = 0;
    std::int32_t token_result_hr = static_cast<std::int32_t>(E_UNEXPECTED);
    std::uint32_t scope_class = static_cast<std::uint32_t>(ScopeClass::Unknown);
    std::int32_t scope_query_hr = static_cast<std::int32_t>(E_UNEXPECTED);
    std::int32_t scope_app_property_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t scope_request_outer_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t scope_request_inner_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t scope_range_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t scope_property_value_hr = static_cast<std::int32_t>(E_PENDING);
    std::int32_t scope_unpack_hr = static_cast<std::int32_t>(E_PENDING);
    std::uint32_t scope_variant_type = 0;
    std::uint32_t scope_token_count = 0;
    std::uint32_t scope_truncated = 0;
    std::array<std::uint32_t, kScopeTokenCapacity> scope_raw_tokens{};
    std::uint64_t events_written = 0;
    std::uint64_t dropped = 0;
    std::uint64_t queued = 0;
};

namespace detail {

inline constexpr LONG64 kStorageReady = 0x524f43444f435631LL;
inline constexpr LONG64 kStorageInitializing = 0x494e49544f435631LL;

inline std::uint64_t read_i64(volatile LONG64 *value) noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(value, 0, 0));
}

inline LONG read_i32(volatile LONG *value) noexcept {
    return InterlockedCompareExchange(value, 0, 0);
}

inline HRESULT hresult_from_last_error() noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(error);
}

inline bool process_creation_stamp(std::uint64_t *stamp) noexcept {
    if (!stamp) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    *stamp = value.QuadPart;
    return true;
}

inline bool process_creation_stamp(DWORD pid, std::uint64_t *stamp) noexcept {
    if (!pid || !stamp) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (!ok) return false;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    *stamp = value.QuadPart;
    return true;
}

inline HostKind host_kind() noexcept {
    wchar_t image_path[2048]{};
    const DWORD length = GetModuleFileNameW(nullptr, image_path,
        static_cast<DWORD>(std::size(image_path)));
    if (!length || length >= std::size(image_path)) return HostKind::Other;
    const wchar_t *base = image_path;
    for (const wchar_t *p = image_path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    if (CompareStringOrdinal(base, -1, L"WindowsTerminal.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::WindowsTerminal;
    if (CompareStringOrdinal(base, -1, L"conhost.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::Conhost;
    if (CompareStringOrdinal(base, -1, L"Code.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::Code;
    if (CompareStringOrdinal(base, -1, L"wslhost.exe", -1, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(base, -1, L"wsl.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::WslHost;
    if (CompareStringOrdinal(base, -1, L"chrome.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::Chrome;
    if (CompareStringOrdinal(base, -1, L"notepad.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::Notepad;
    if (CompareStringOrdinal(base, -1, L"SearchHost.exe", -1, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(base, -1, L"SearchUI.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::SearchHost;
    if (CompareStringOrdinal(base, -1, L"WinStore.App.exe", -1, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(base, -1, L"MicrosoftStore.exe", -1, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(base, -1, L"StoreExperienceHost.exe", -1, TRUE) == CSTR_EQUAL)
        return HostKind::MicrosoftStore;
    return HostKind::Other;
}

inline std::uint64_t random_producer_id(std::uint64_t creation, DWORD pid) noexcept {
    std::uint64_t result = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&result),
            static_cast<ULONG>(sizeof(result)), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        result = creation ^ (static_cast<std::uint64_t>(pid) << 32) ^ GetTickCount64();
    result &= 0x7fffffffffffffffULL;
    return result == 0 ? 1 : result;
}

inline std::wstring mapping_leaf(DWORD pid, std::uint64_t creation) {
    wchar_t value[96]{};
    const int length = std::swprintf(value, std::size(value),
        L"LiuKaiDoctorP0-%lu-%016llx",
        static_cast<unsigned long>(pid), static_cast<unsigned long long>(creation));
    return length > 0 ? std::wstring(value, static_cast<std::size_t>(length)) : std::wstring();
}

inline std::wstring local_mapping_name(DWORD pid, std::uint64_t creation) {
    const auto leaf = mapping_leaf(pid, creation);
    return leaf.empty() ? std::wstring() : L"Local\\" + leaf;
}

inline std::wstring mutex_name(const std::wstring &user_sid) {
    return user_sid.empty()
        ? std::wstring()
        : L"Local\\LiuKaiDoctorP0-Collector-" + user_sid;
}

struct TokenIdentity {
    std::uint32_t integrity_rid = 0;
    std::uint32_t app_container = 0;
    std::int32_t result_hr = static_cast<std::int32_t>(E_UNEXPECTED);
};

inline TokenIdentity token_identity(HANDLE token) {
    TokenIdentity result;
    if (!token || token == INVALID_HANDLE_VALUE) {
        result.result_hr = static_cast<std::int32_t>(E_INVALIDARG);
        return result;
    }

    DWORD needed = 0;
    if (!GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        result.result_hr = static_cast<std::int32_t>(hresult_from_last_error());
        return result;
    }
    std::vector<unsigned char> label_storage(needed);
    if (!GetTokenInformation(token, TokenIntegrityLevel, label_storage.data(), needed, &needed)) {
        result.result_hr = static_cast<std::int32_t>(hresult_from_last_error());
        return result;
    }
    auto *label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(label_storage.data());
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (count == 0) {
        result.result_hr = static_cast<std::int32_t>(E_UNEXPECTED);
        return result;
    }
    result.integrity_rid = static_cast<std::uint32_t>(
        *GetSidSubAuthority(label->Label.Sid, count - 1));

    BOOL app_container = FALSE;
    DWORD copied = 0;
    if (GetTokenInformation(token, TokenIsAppContainer, &app_container, sizeof(app_container), &copied))
        result.app_container = app_container ? 1 : 0;
    else {
        result.result_hr = static_cast<std::int32_t>(detail::hresult_from_last_error());
        return result;
    }
    result.result_hr = static_cast<std::int32_t>(S_OK);
    return result;
}

inline TokenIdentity own_token_identity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return TokenIdentity{0, 0, static_cast<std::int32_t>(hresult_from_last_error())};
    const auto result = token_identity(token);
    CloseHandle(token);
    return result;
}

inline bool token_user_sid(HANDLE token, std::wstring *sid_text) {
    if (!token || !sid_text) return false;
    DWORD needed = 0;
    if (!GetTokenInformation(token, TokenUser, nullptr, 0, &needed) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return false;
    std::vector<unsigned char> storage(needed);
    if (!GetTokenInformation(token, TokenUser, storage.data(), needed, &needed)) return false;
    auto *owner = reinterpret_cast<TOKEN_USER *>(storage.data());
    wchar_t *text = nullptr;
    if (!ConvertSidToStringSidW(owner->User.Sid, &text) || !text) return false;
    sid_text->assign(text);
    LocalFree(text);
    return !sid_text->empty();
}

inline bool token_app_container_sid(HANDLE token, std::wstring *sid_text) {
    if (!token || !sid_text) return false;
    BOOL app_container = FALSE;
    DWORD copied = 0;
    if (!GetTokenInformation(token, TokenIsAppContainer,
            &app_container, sizeof(app_container), &copied) || !app_container) {
        sid_text->clear();
        return false;
    }
    DWORD needed = 0;
    if (!GetTokenInformation(token, TokenAppContainerSid,
            nullptr, 0, &needed) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        sid_text->clear();
        return false;
    }
    std::vector<unsigned char> storage(needed);
    if (!GetTokenInformation(token, TokenAppContainerSid,
            storage.data(), needed, &needed)) {
        sid_text->clear();
        return false;
    }
    struct AppContainerSidInformation { PSID value; };
    auto *information =
        reinterpret_cast<AppContainerSidInformation *>(storage.data());
    wchar_t *text = nullptr;
    if (!information->value ||
        !ConvertSidToStringSidW(information->value, &text) || !text) {
        sid_text->clear();
        return false;
    }
    sid_text->assign(text);
    LocalFree(text);
    return !sid_text->empty();
}

inline HRESULT token_sid_copy(HANDLE token, std::vector<unsigned char> *copy) {
    if (!token || !copy) return E_INVALIDARG;
    DWORD needed = 0;
    if (!GetTokenInformation(token, TokenUser, nullptr, 0, &needed) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return hresult_from_last_error();
    std::vector<unsigned char> storage(needed);
    if (!GetTokenInformation(token, TokenUser, storage.data(), needed, &needed))
        return hresult_from_last_error();
    const auto *owner = reinterpret_cast<TOKEN_USER *>(storage.data());
    const DWORD length = GetLengthSid(owner->User.Sid);
    copy->resize(length);
    return CopySid(length, copy->data(), owner->User.Sid) ? S_OK
        : hresult_from_last_error();
}

inline HRESULT token_session_id(HANDLE token, DWORD *session_id) {
    if (!token || !session_id) return E_INVALIDARG;
    DWORD copied = 0;
    return GetTokenInformation(token, TokenSessionId, session_id,
        sizeof(*session_id), &copied) ? S_OK : hresult_from_last_error();
}

inline HRESULT token_is_app_container(HANDLE token, bool *value) {
    if (!token || !value) return E_INVALIDARG;
    BOOL result = FALSE;
    DWORD copied = 0;
    const HRESULT hr = GetTokenInformation(token, TokenIsAppContainer,
        &result, sizeof(result), &copied) ? S_OK : hresult_from_last_error();
    if (SUCCEEDED(hr)) *value = result != FALSE;
    return hr;
}

inline bool same_user_session(const std::vector<unsigned char> &left_sid,
    DWORD left_session, const std::vector<unsigned char> &right_sid,
    DWORD right_session) noexcept {
    return !left_sid.empty() && left_sid.size() == right_sid.size() &&
        left_session == right_session &&
        EqualSid(const_cast<unsigned char *>(left_sid.data()),
            const_cast<unsigned char *>(right_sid.data())) != FALSE;
}

inline bool own_app_container_sid(std::wstring *sid_text) {
    if (!sid_text) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        sid_text->clear();
        return false;
    }
    const bool result = token_app_container_sid(token, sid_text);
    CloseHandle(token);
    return result;
}

inline bool own_user_sid(std::wstring *sid_text) {
    if (!sid_text) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        sid_text->clear();
        return false;
    }
    const bool result = token_user_sid(token, sid_text);
    CloseHandle(token);
    return result;
}

inline std::wstring mapping_security_sddl(
    const std::wstring &user_sid, const std::wstring &package_sid) {
    std::wstring result = L"D:P(A;;0x6;;;" + user_sid + L")";
    if (!package_sid.empty())
        result += L"(A;;0x20006;;;" + package_sid + L")";
    return result;
}

inline std::wstring mutex_security_sddl(const std::wstring &user_sid) {
    return L"D:P(A;;0x1f0001;;;" + user_sid + L")";
}

inline HRESULT app_container_named_object_path(HANDLE token,
    std::wstring *path) {
    if (!token || !path) return E_INVALIDARG;
    path->clear();
    ULONG required = 0;
    BOOL success = GetAppContainerNamedObjectPath(
        token, nullptr, 0, nullptr, &required);
    DWORD query_error = success ? ERROR_SUCCESS : GetLastError();
    if (!success && query_error != ERROR_INSUFFICIENT_BUFFER) {
        return HRESULT_FROM_WIN32(query_error);
    }
    if (required == 0) {
        return E_UNEXPECTED;
    }
    path->assign(required, L'\0');
    ULONG returned = 0;
    success = GetAppContainerNamedObjectPath(token, nullptr,
        required, path->data(), &returned);
    const HRESULT result = success ? S_OK : hresult_from_last_error();
    if (FAILED(result)) return result;
    if (returned > path->size()) return E_UNEXPECTED;
    path->resize(returned > 0 && path->at(returned - 1) == L'\0'
        ? returned - 1 : returned);
    return S_OK;
}

struct TargetMappingIdentity {
    std::wstring name;
    std::vector<unsigned char> owner_sid;
    bool app_container = false;
};

inline HRESULT process_target_mapping(DWORD pid, std::uint64_t expected_creation,
    bool require_same_user_session, TargetMappingIdentity *identity) {
    if (!pid || !expected_creation || !identity) return E_INVALIDARG;
    identity->name.clear();
    identity->owner_sid.clear();
    identity->app_container = false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return hresult_from_last_error();
    std::uint64_t actual_creation = 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL times_read = GetProcessTimes(
        process, &created, &exited, &kernel, &user);
    const HRESULT times_hr = times_read ? S_OK : hresult_from_last_error();
    if (FAILED(times_hr)) {
        CloseHandle(process);
        return times_hr;
    }
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    actual_creation = value.QuadPart;
    if (actual_creation != expected_creation) {
        CloseHandle(process);
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    }

    HANDLE target_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &target_token)) {
        const HRESULT hr = hresult_from_last_error();
        CloseHandle(process);
        return hr;
    }
    CloseHandle(process);

    HRESULT hr = token_sid_copy(target_token, &identity->owner_sid);
    DWORD target_session = 0;
    if (SUCCEEDED(hr)) hr = token_session_id(target_token, &target_session);
    if (SUCCEEDED(hr))
        hr = token_is_app_container(target_token, &identity->app_container);
    if (FAILED(hr)) {
        CloseHandle(target_token);
        return hr;
    }

    if (require_same_user_session) {
        HANDLE own_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_token)) {
            hr = hresult_from_last_error();
            CloseHandle(target_token);
            return hr;
        }
        std::vector<unsigned char> own_owner;
        DWORD own_session = 0;
        hr = token_sid_copy(own_token, &own_owner);
        if (SUCCEEDED(hr)) hr = token_session_id(own_token, &own_session);
        CloseHandle(own_token);
        if (FAILED(hr)) {
            CloseHandle(target_token);
            return hr;
        }
        if (!same_user_session(own_owner, own_session,
                identity->owner_sid, target_session)) {
            CloseHandle(target_token);
            return E_ACCESSDENIED;
        }
    }

    const auto leaf = mapping_leaf(pid, expected_creation);
    if (leaf.empty()) {
        CloseHandle(target_token);
        return E_OUTOFMEMORY;
    }
    if (identity->app_container && pid != GetCurrentProcessId()) {
        std::wstring prefix;
        hr = app_container_named_object_path(target_token, &prefix);
        if (SUCCEEDED(hr)) {
            if (!prefix.empty() && prefix.back() == L'\\') prefix.pop_back();
            identity->name = prefix + L"\\" + leaf;
        }
    } else {
        identity->name = L"Local\\" + leaf;
        hr = S_OK;
    }
    CloseHandle(target_token);
    return SUCCEEDED(hr) && !identity->name.empty() ? S_OK :
        (FAILED(hr) ? hr : E_OUTOFMEMORY);
}

inline HRESULT kernel_object_owner_sid(HANDLE object,
    std::vector<unsigned char> *sid_copy) {
    if (!object || !sid_copy) return E_INVALIDARG;
    DWORD needed = 0;
    if (!GetKernelObjectSecurity(object, OWNER_SECURITY_INFORMATION,
            nullptr, 0, &needed) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return hresult_from_last_error();
    if (!needed) return E_UNEXPECTED;
    std::vector<unsigned char> descriptor(needed);
    if (!GetKernelObjectSecurity(object, OWNER_SECURITY_INFORMATION,
            descriptor.data(), needed, &needed))
        return hresult_from_last_error();
    PSID owner = nullptr;
    BOOL owner_defaulted = FALSE;
    if (!GetSecurityDescriptorOwner(
            descriptor.data(), &owner, &owner_defaulted) || !owner)
        return hresult_from_last_error();
    sid_copy->resize(GetLengthSid(owner));
    return CopySid(static_cast<DWORD>(sid_copy->size()),
        sid_copy->data(), owner) ? S_OK : hresult_from_last_error();
}

} // namespace detail

struct UserSecurity {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    HRESULT result = E_UNEXPECTED;

    static UserSecurity create_mapping() {
        UserSecurity security;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            security.result = detail::hresult_from_last_error();
            return security;
        }
        std::wstring user_sid;
        const bool captured = detail::token_user_sid(token, &user_sid);
        bool app_container = false;
        const HRESULT app_container_hr =
            detail::token_is_app_container(token, &app_container);
        std::wstring app_container_sid;
        const bool captured_app_container =
            detail::token_app_container_sid(token, &app_container_sid);
        CloseHandle(token);
        if (!captured || FAILED(app_container_hr) ||
            (app_container && !captured_app_container)) {
            security.result = E_ACCESSDENIED;
            return security;
        }
        const std::wstring sddl =
            detail::mapping_security_sddl(user_sid, app_container_sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &security.descriptor, nullptr)) {
            security.result = detail::hresult_from_last_error();
            return security;
        }

        security.attributes.nLength = sizeof(security.attributes);
        security.attributes.lpSecurityDescriptor = security.descriptor;
        security.attributes.bInheritHandle = FALSE;
        security.result = S_OK;
        return security;
    }

    static UserSecurity create_mutex() {
        UserSecurity security;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            security.result = detail::hresult_from_last_error();
            return security;
        }
        std::wstring user_sid;
        const bool captured = detail::token_user_sid(token, &user_sid);
        CloseHandle(token);
        if (!captured) {
            security.result = E_ACCESSDENIED;
            return security;
        }
        const std::wstring sddl =
            detail::mutex_security_sddl(user_sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &security.descriptor, nullptr)) {
            security.result = detail::hresult_from_last_error();
            return security;
        }
        security.attributes.nLength = sizeof(security.attributes);
        security.attributes.lpSecurityDescriptor = security.descriptor;
        security.attributes.bInheritHandle = FALSE;
        security.result = S_OK;
        return security;
    }

    ~UserSecurity() {
        if (descriptor) LocalFree(descriptor);
    }

    UserSecurity(UserSecurity &&other) noexcept
        : attributes(other.attributes), descriptor(other.descriptor), result(other.result) {
        other.descriptor = nullptr;
    }

    UserSecurity &operator=(UserSecurity &&other) noexcept {
        if (this == &other) return *this;
        if (descriptor) LocalFree(descriptor);
        attributes = other.attributes;
        descriptor = other.descriptor;
        result = other.result;
        other.descriptor = nullptr;
        return *this;
    }

private:
    UserSecurity() = default;
    UserSecurity(const UserSecurity &) = delete;
    UserSecurity &operator=(const UserSecurity &) = delete;
};

template<std::size_t Capacity = kQueueCapacity, std::uint32_t Protocol = kProtocolVersion>
class SharedQueue {
public:
    struct Slot {
        volatile LONG64 sequence = 0;
        volatile LONG64 epoch = 0;
        volatile LONG accepted = 0;
        EventRecord record{};
    };

    struct Control {
        volatile LONG64 magic = 0;
        volatile LONG protocol = 0;
        volatile LONG capacity = 0;
        volatile LONG64 epoch = 0;
        volatile LONG enabled = 0;
        volatile LONG64 heartbeat_ms = 0;
        volatile LONG64 write_index = 0;
        volatile LONG64 read_index = 0;
        volatile LONG64 operation_index = 0;
        volatile LONG64 events_written = 0;
        volatile LONG64 dropped = 0;
        volatile LONG64 producer_id = 0;
        volatile LONG64 process_creation = 0;
        volatile LONG process_id = 0;
        volatile LONG host_kind = 0;
        volatile LONG integrity_rid = 0;
        volatile LONG app_container = 0;
        volatile LONG token_result_hr = static_cast<LONG>(E_UNEXPECTED);
        volatile LONG64 scope_generation = 0;
        volatile LONG scope_class = static_cast<LONG>(ScopeClass::Unknown);
        volatile LONG scope_query_hr = static_cast<LONG>(E_UNEXPECTED);
        volatile LONG scope_app_property_hr = static_cast<LONG>(E_PENDING);
        volatile LONG scope_request_outer_hr = static_cast<LONG>(E_PENDING);
        volatile LONG scope_request_inner_hr = static_cast<LONG>(E_PENDING);
        volatile LONG scope_range_hr = static_cast<LONG>(E_PENDING);
        volatile LONG scope_property_value_hr = static_cast<LONG>(E_PENDING);
        volatile LONG scope_unpack_hr = static_cast<LONG>(E_PENDING);
        volatile LONG scope_variant_type = 0;
        volatile LONG scope_token_count = 0;
        volatile LONG scope_truncated = 0;
        volatile LONG scope_raw_tokens[kScopeTokenCapacity]{};
    };

    struct Storage {
        Control control{};
        std::array<Slot, Capacity> slots{};
    };

    static constexpr std::size_t storage_bytes() noexcept { return sizeof(Storage); }
    static constexpr std::size_t capacity() noexcept { return Capacity; }
    static constexpr std::uint32_t protocol() noexcept { return Protocol; }

    explicit SharedQueue(void *storage) noexcept
        : storage_(storage ? static_cast<Storage *>(storage) : nullptr) {
        static_assert(Capacity > 0);
        static_assert(sizeof(Storage) < 0x100000000ULL);
    }

    bool valid() const noexcept {
        if (!storage_) return false;
        return static_cast<std::uint64_t>(detail::read_i64(&storage_->control.magic)) ==
                static_cast<std::uint64_t>(detail::kStorageReady) &&
            static_cast<std::uint32_t>(detail::read_i32(&storage_->control.protocol)) == Protocol &&
            static_cast<std::uint32_t>(detail::read_i32(&storage_->control.capacity)) ==
                static_cast<std::uint32_t>(Capacity);
    }

    bool initialize_if_empty() noexcept {
        if (!storage_) return false;
        const auto current = detail::read_i64(&storage_->control.magic);
        if (current == detail::kStorageReady) return valid();
        if (current != 0) return false;
        if (InterlockedCompareExchange64(
                &storage_->control.magic, detail::kStorageInitializing, 0) != 0)
            return valid();

        for (std::size_t i = 0; i < Capacity; ++i) {
            InterlockedExchange64(&storage_->slots[i].sequence, static_cast<LONG64>(i));
            InterlockedExchange64(&storage_->slots[i].epoch, 0);
            InterlockedExchange(&storage_->slots[i].accepted, 0);
        }
        InterlockedExchange(&storage_->control.protocol, static_cast<LONG>(Protocol));
        InterlockedExchange(&storage_->control.capacity, static_cast<LONG>(Capacity));
        InterlockedExchange64(&storage_->control.epoch, 0);
        InterlockedExchange(&storage_->control.enabled, 0);
        InterlockedExchange64(&storage_->control.heartbeat_ms, 0);
        InterlockedExchange64(&storage_->control.write_index, 0);
        InterlockedExchange64(&storage_->control.read_index, 0);
        InterlockedExchange64(&storage_->control.operation_index, 0);
        InterlockedExchange64(&storage_->control.events_written, 0);
        InterlockedExchange64(&storage_->control.dropped, 0);
        InterlockedExchange(&storage_->control.scope_class,
            static_cast<LONG>(ScopeClass::Unknown));
        InterlockedExchange(&storage_->control.scope_query_hr,
            static_cast<LONG>(E_UNEXPECTED));
        InterlockedExchange(&storage_->control.scope_app_property_hr,
            static_cast<LONG>(E_PENDING));
        InterlockedExchange(&storage_->control.scope_request_outer_hr,
            static_cast<LONG>(E_PENDING));
        InterlockedExchange(&storage_->control.scope_request_inner_hr,
            static_cast<LONG>(E_PENDING));
        InterlockedExchange(&storage_->control.scope_range_hr,
            static_cast<LONG>(E_PENDING));
        InterlockedExchange(&storage_->control.scope_property_value_hr,
            static_cast<LONG>(E_PENDING));
        InterlockedExchange(&storage_->control.scope_unpack_hr,
            static_cast<LONG>(E_PENDING));
        InterlockedExchange64(&storage_->control.magic, detail::kStorageReady);
        return true;
    }

    bool initialize(std::uint64_t epoch, bool enabled) noexcept {
        if (!initialize_if_empty() || !valid()) return false;
        const std::uint64_t now = GetTickCount64();
        const auto current_epoch = detail::read_i64(&storage_->control.epoch);
        if (current_epoch != epoch) {
            InterlockedExchange(&storage_->control.enabled, 0);
            InterlockedExchange64(&storage_->control.epoch,
                static_cast<LONG64>(epoch));
            reclaim_obsolete_slots();
        }
        InterlockedExchange64(&storage_->control.heartbeat_ms,
            static_cast<LONG64>(now));
        InterlockedExchange(&storage_->control.enabled, enabled ? 1 : 0);
        return true;
    }

    bool set_enabled(bool enabled) noexcept {
        if (!valid()) return false;
        InterlockedExchange(&storage_->control.enabled, enabled ? 1 : 0);
        return true;
    }

    bool refresh_heartbeat() noexcept {
        if (!valid()) return false;
        InterlockedExchange64(&storage_->control.heartbeat_ms,
            static_cast<LONG64>(GetTickCount64()));
        return true;
    }

    bool can_write() const noexcept {
        if (!valid() || detail::read_i32(&storage_->control.enabled) == 0) return false;
        const auto now = GetTickCount64();
        const auto heartbeat = detail::read_i64(&storage_->control.heartbeat_ms);
        if (heartbeat == 0 || now < heartbeat ||
            now - heartbeat > kCollectorHeartbeatTimeoutMs)
            return false;
        const auto write = detail::read_i64(&storage_->control.write_index);
        const auto read = detail::read_i64(&storage_->control.read_index);
        return write >= read && write - read < Capacity;
    }

    std::uint64_t begin_operation() noexcept {
        if (!can_write()) return 0;
        const auto value = static_cast<std::uint64_t>(
            InterlockedIncrement64(&storage_->control.operation_index));
        return value == 0 ? static_cast<std::uint64_t>(
            InterlockedIncrement64(&storage_->control.operation_index)) : value;
    }

    WriteResult try_write(const EventRecord &record) noexcept {
        if (!valid()) return WriteResult::Invalid;
        const auto captured_epoch = detail::read_i64(&storage_->control.epoch);
        if (detail::read_i32(&storage_->control.enabled) == 0) return WriteResult::Disabled;
        const auto now = GetTickCount64();
        const auto heartbeat = detail::read_i64(&storage_->control.heartbeat_ms);
        if (heartbeat == 0 || now < heartbeat ||
            now - heartbeat > kCollectorHeartbeatTimeoutMs) {
            InterlockedExchange(&storage_->control.enabled, 0);
            return WriteResult::Disabled;
        }

        auto position = detail::read_i64(&storage_->control.write_index);
        for (std::size_t attempt = 0; attempt < 16; ++attempt) {
            const auto read_position = detail::read_i64(&storage_->control.read_index);
            if (position - read_position >= Capacity) {
                InterlockedIncrement64(&storage_->control.dropped);
                return WriteResult::Full;
            }
            Slot &slot = storage_->slots[static_cast<std::size_t>(position % Capacity)];
            const auto sequence = detail::read_i64(&slot.sequence);
            const auto difference = static_cast<std::int64_t>(
                static_cast<LONG64>(sequence) - static_cast<LONG64>(position));
            if (difference < 0) {
                InterlockedIncrement64(&storage_->control.dropped);
                return WriteResult::Full;
            }
            if (difference > 0) {
                position = detail::read_i64(&storage_->control.write_index);
                continue;
            }
            if (InterlockedCompareExchange64(
                    &storage_->control.write_index,
                    static_cast<LONG64>(position + 1),
                    static_cast<LONG64>(position)) != static_cast<LONG64>(position)) {
                position = detail::read_i64(&storage_->control.write_index);
                continue;
            }

            EventRecord published = record;
            published.sequence = position + 1;
            std::memcpy(&slot.record, &published, sizeof(published));
            const bool still_current =
                detail::read_i64(&storage_->control.epoch) == captured_epoch &&
                detail::read_i32(&storage_->control.enabled) != 0;
            InterlockedExchange64(&slot.epoch,
                static_cast<LONG64>(captured_epoch));
            InterlockedExchange(&slot.accepted, still_current ? 1 : 0);
            MemoryBarrier();
            InterlockedExchange64(&slot.sequence, static_cast<LONG64>(position + 1));
            if (still_current)
                InterlockedIncrement64(&storage_->control.events_written);
            else
                InterlockedIncrement64(&storage_->control.dropped);
            return still_current ? WriteResult::Written : WriteResult::Disabled;
        }
        InterlockedIncrement64(&storage_->control.dropped);
        return WriteResult::Full;
    }

    ReadResult try_read(EventRecord *record, std::uint64_t *event_epoch = nullptr) noexcept {
        if (!record) return ReadResult::Invalid;
        if (!valid()) return ReadResult::Invalid;
        for (std::size_t attempt = 0; attempt < Capacity; ++attempt) {
            const auto write_position = detail::read_i64(&storage_->control.write_index);
            const auto position = detail::read_i64(&storage_->control.read_index);
            if (position >= write_position) return ReadResult::Empty;

            Slot &slot = storage_->slots[static_cast<std::size_t>(position % Capacity)];
            const auto sequence = detail::read_i64(&slot.sequence);
            if (sequence != position + 1) return ReadResult::Empty;
            if (InterlockedCompareExchange64(
                    &storage_->control.read_index,
                    static_cast<LONG64>(position + 1),
                    static_cast<LONG64>(position)) != static_cast<LONG64>(position))
                continue;

            const auto slot_epoch = detail::read_i64(&slot.epoch);
            const auto accepted = detail::read_i32(&slot.accepted) != 0;
            MemoryBarrier();
            const auto current_epoch = detail::read_i64(&storage_->control.epoch);
            if (!accepted || slot_epoch != current_epoch) {
                if (accepted)
                    InterlockedIncrement64(&storage_->control.dropped);
                InterlockedExchange64(&slot.sequence,
                    static_cast<LONG64>(position + Capacity));
                continue;
            }
            std::memcpy(record, &slot.record, sizeof(*record));
            if (event_epoch) *event_epoch = slot_epoch;
            MemoryBarrier();
            InterlockedExchange64(&slot.sequence,
                static_cast<LONG64>(position + Capacity));
            return ReadResult::Read;
        }
        return ReadResult::Empty;
    }

    bool publish_identity(std::uint64_t producer_id, DWORD process_id,
        std::uint64_t process_creation, HostKind kind, const detail::TokenIdentity &token) noexcept {
        if (!valid()) return false;
        InterlockedCompareExchange64(&storage_->control.producer_id,
            static_cast<LONG64>(producer_id), 0);
        InterlockedCompareExchange64(&storage_->control.process_creation,
            static_cast<LONG64>(process_creation), 0);
        InterlockedExchange(&storage_->control.process_id, static_cast<LONG>(process_id));
        InterlockedExchange(&storage_->control.host_kind, static_cast<LONG>(kind));
        InterlockedExchange(&storage_->control.integrity_rid,
            static_cast<LONG>(token.integrity_rid));
        InterlockedExchange(&storage_->control.app_container,
            static_cast<LONG>(token.app_container));
        InterlockedExchange(&storage_->control.token_result_hr,
            static_cast<LONG>(token.result_hr));
        return true;
    }

    bool publish_scope(const ScopeObservation &scope) noexcept {
        if (!valid()) return false;
        InterlockedIncrement64(&storage_->control.scope_generation);
        InterlockedExchange(&storage_->control.scope_query_hr, scope.query_result);
        InterlockedExchange(&storage_->control.scope_app_property_hr,
            scope.app_property_hr);
        InterlockedExchange(&storage_->control.scope_request_outer_hr,
            scope.request_outer_hr);
        InterlockedExchange(&storage_->control.scope_request_inner_hr,
            scope.request_inner_hr);
        InterlockedExchange(&storage_->control.scope_range_hr,
            scope.range_hr);
        InterlockedExchange(&storage_->control.scope_property_value_hr,
            scope.property_value_hr);
        InterlockedExchange(&storage_->control.scope_unpack_hr,
            scope.unpack_hr);
        InterlockedExchange(&storage_->control.scope_variant_type,
            static_cast<LONG>(scope.variant_type));
        InterlockedExchange(&storage_->control.scope_class,
            static_cast<LONG>(classify_scope(scope)));
        const auto count = static_cast<std::uint32_t>(
            scope.token_count > kScopeTokenCapacity ? kScopeTokenCapacity : scope.token_count);
        InterlockedExchange(&storage_->control.scope_token_count,
            static_cast<LONG>(count));
        InterlockedExchange(&storage_->control.scope_truncated,
            scope.truncated ? 1 : 0);
        for (std::size_t i = 0; i < kScopeTokenCapacity; ++i) {
            InterlockedExchange(&storage_->control.scope_raw_tokens[i],
                static_cast<LONG>(i < scope.token_count ? scope.raw_tokens[i] : 0));
        }
        InterlockedIncrement64(&storage_->control.scope_generation);
        return true;
    }

    ScopeClass scope_class() const noexcept {
        if (!valid()) return ScopeClass::Unknown;
        return static_cast<ScopeClass>(static_cast<std::uint32_t>(
            detail::read_i32(&storage_->control.scope_class)));
    }

    std::uint64_t producer_id() const noexcept {
        return valid() ? detail::read_i64(&storage_->control.producer_id) : 0;
    }

    std::uint64_t epoch() const noexcept {
        return valid() ? detail::read_i64(&storage_->control.epoch) : 0;
    }

    QueueSnapshot snapshot() const noexcept {
        QueueSnapshot result;
        if (!valid()) return result;
        for (std::size_t attempt = 0; attempt < 2; ++attempt) {
            const auto before = detail::read_i64(&storage_->control.scope_generation);
            if (before & 1) continue;
            result.epoch = detail::read_i64(&storage_->control.epoch);
            result.heartbeat_ms = detail::read_i64(&storage_->control.heartbeat_ms);
            result.producer_id = detail::read_i64(&storage_->control.producer_id);
            result.process_creation = detail::read_i64(&storage_->control.process_creation);
            result.process_id = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.process_id));
            result.host_kind = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.host_kind));
            result.integrity_rid = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.integrity_rid));
            result.app_container = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.app_container));
            result.token_result_hr = detail::read_i32(&storage_->control.token_result_hr);
            result.scope_class = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.scope_class));
            result.scope_query_hr = detail::read_i32(&storage_->control.scope_query_hr);
            result.scope_app_property_hr =
                detail::read_i32(&storage_->control.scope_app_property_hr);
            result.scope_request_outer_hr =
                detail::read_i32(&storage_->control.scope_request_outer_hr);
            result.scope_request_inner_hr =
                detail::read_i32(&storage_->control.scope_request_inner_hr);
            result.scope_range_hr =
                detail::read_i32(&storage_->control.scope_range_hr);
            result.scope_property_value_hr =
                detail::read_i32(&storage_->control.scope_property_value_hr);
            result.scope_unpack_hr =
                detail::read_i32(&storage_->control.scope_unpack_hr);
            result.scope_variant_type = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.scope_variant_type));
            result.scope_token_count = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.scope_token_count));
            result.scope_truncated = static_cast<std::uint32_t>(
                detail::read_i32(&storage_->control.scope_truncated));
            for (std::size_t i = 0; i < kScopeTokenCapacity; ++i)
                result.scope_raw_tokens[i] = static_cast<std::uint32_t>(
                    detail::read_i32(&storage_->control.scope_raw_tokens[i]));
            result.events_written = detail::read_i64(&storage_->control.events_written);
            result.dropped = detail::read_i64(&storage_->control.dropped);
            const auto write = detail::read_i64(&storage_->control.write_index);
            const auto read = detail::read_i64(&storage_->control.read_index);
            result.queued = write >= read ? write - read : 0;
            const auto after = detail::read_i64(&storage_->control.scope_generation);
            if (before == after) {
                result.valid = true;
                return result;
            }
        }
        return result;
    }

private:
    void reclaim_obsolete_slots() noexcept {
        const auto current_epoch =
            detail::read_i64(&storage_->control.epoch);
        const auto reserved = detail::read_i64(&storage_->control.read_index);
        if (reserved != 0) {
            const auto abandoned = reserved - 1;
            Slot &slot = storage_->slots[
                static_cast<std::size_t>(abandoned % Capacity)];
            const auto sequence = detail::read_i64(&slot.sequence);
            if (sequence == reserved &&
                detail::read_i64(&slot.epoch) != current_epoch &&
                InterlockedCompareExchange64(
                    &slot.sequence,
                    static_cast<LONG64>(abandoned + Capacity),
                    static_cast<LONG64>(reserved)) ==
                        static_cast<LONG64>(reserved) &&
                detail::read_i32(&slot.accepted) != 0) {
                InterlockedIncrement64(&storage_->control.dropped);
            }
        }
        for (std::size_t attempt = 0; attempt < Capacity; ++attempt) {
            const auto position = detail::read_i64(&storage_->control.read_index);
            const auto write = detail::read_i64(&storage_->control.write_index);
            if (position >= write) return;
            Slot &slot = storage_->slots[static_cast<std::size_t>(position % Capacity)];
            if (detail::read_i64(&slot.sequence) != position + 1) return;
            if (detail::read_i64(&slot.epoch) ==
                current_epoch)
                return;
            const bool accepted =
                detail::read_i32(&slot.accepted) != 0;
            if (InterlockedCompareExchange64(&storage_->control.read_index,
                    static_cast<LONG64>(position + 1),
                    static_cast<LONG64>(position)) != static_cast<LONG64>(position))
                continue;
            if (accepted)
                InterlockedIncrement64(&storage_->control.dropped);
            InterlockedExchange64(&slot.sequence,
                static_cast<LONG64>(position + Capacity));
        }
    }

    Storage *storage_ = nullptr;
};

inline std::uint64_t monotonic_nanoseconds() noexcept {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency) || !frequency.QuadPart ||
        !QueryPerformanceCounter(&counter))
        return 0;
    const auto whole = static_cast<std::uint64_t>(counter.QuadPart) /
        static_cast<std::uint64_t>(frequency.QuadPart);
    const auto remainder = static_cast<std::uint64_t>(counter.QuadPart) %
        static_cast<std::uint64_t>(frequency.QuadPart);
    return whole * 1000000000ULL +
        (remainder * 1000000000ULL) / static_cast<std::uint64_t>(frequency.QuadPart);
}

class ProcessProducer {
public:
    using Queue = SharedQueue<kQueueCapacity>;

    ProcessProducer() = default;
    ProcessProducer(const ProcessProducer &) = delete;
    ProcessProducer &operator=(const ProcessProducer &) = delete;
    ~ProcessProducer() { close(); }

    bool ensure() noexcept {
        try {
            if (view_ && mapping_) return true;
            close();
            std::uint64_t creation = 0;
            if (!detail::process_creation_stamp(&creation)) {
                last_hr_ = detail::hresult_from_last_error();
                return false;
            }
            process_creation_ = creation;
            mapping_name_ = detail::local_mapping_name(
                GetCurrentProcessId(), creation);
            if (mapping_name_.empty()) {
                last_hr_ = E_OUTOFMEMORY;
                return false;
            }
            auto security = UserSecurity::create_mapping();
            if (FAILED(security.result)) {
                last_hr_ = security.result;
                return false;
            }
            SetLastError(ERROR_SUCCESS);
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security.attributes,
                PAGE_READWRITE, 0, static_cast<DWORD>(Queue::storage_bytes()),
                mapping_name_.c_str());
            const DWORD create_error = GetLastError();
            bool existing = create_error == ERROR_ALREADY_EXISTS;
            if (!mapping_ || existing) {
                const HRESULT create_hr = mapping_
                    ? HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)
                    : detail::hresult_from_last_error();
                if (create_error != ERROR_ALREADY_EXISTS &&
                    create_hr != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
                    last_hr_ = create_hr;
                    return false;
                }
                if (mapping_) {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
                mapping_ = OpenFileMappingW(0x20006, FALSE,
                    mapping_name_.c_str());
                if (!mapping_) {
                    last_hr_ = detail::hresult_from_last_error();
                    return false;
                }
                existing = true;
            }
            HANDLE own_token = nullptr;
            std::vector<unsigned char> own_sid;
            HRESULT owner_setup_hr = OpenProcessToken(
                GetCurrentProcess(), TOKEN_QUERY, &own_token)
                ? detail::token_sid_copy(own_token, &own_sid)
                : detail::hresult_from_last_error();
            if (own_token) CloseHandle(own_token);
            std::vector<unsigned char> owner_sid;
            const HRESULT owner_hr = SUCCEEDED(owner_setup_hr)
                ? detail::kernel_object_owner_sid(mapping_, &owner_sid)
                : owner_setup_hr;
            if (FAILED(owner_hr) || own_sid.size() != owner_sid.size() ||
                !EqualSid(own_sid.data(), owner_sid.data())) {
                last_hr_ = FAILED(owner_hr) ? owner_hr : E_ACCESSDENIED;
                close();
                return false;
            }
            view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                Queue::storage_bytes());
            if (!view_) {
                last_hr_ = detail::hresult_from_last_error();
                close();
                return false;
            }
            Queue queue(view_);
            if (!existing && !queue.initialize_if_empty()) {
                last_hr_ = E_UNEXPECTED;
                close();
                return false;
            }
            for (std::size_t attempt = 0;
                    existing && !queue.valid() && attempt < 32; ++attempt)
                SwitchToThread();
            if (existing && !queue.valid()) {
                last_hr_ = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
                close();
                return false;
            }
            const auto identity = detail::own_token_identity();
            const auto candidate_id = detail::random_producer_id(creation,
                GetCurrentProcessId());
            if (!queue.publish_identity(candidate_id, GetCurrentProcessId(), creation,
                    detail::host_kind(), identity)) {
                last_hr_ = E_UNEXPECTED;
                close();
                return false;
            }
            const auto snapshot = queue.snapshot();
            if (snapshot.process_id != GetCurrentProcessId() ||
                snapshot.process_creation != creation) {
                last_hr_ = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
                close();
                return false;
            }
            last_hr_ = S_OK;
            return true;
        } catch (...) {
            close();
            last_hr_ = E_OUTOFMEMORY;
            return false;
        }
    }

    bool can_write() const noexcept {
        return view_ && Queue(view_).can_write();
    }

    std::uint64_t begin_operation() noexcept {
        return view_ ? Queue(view_).begin_operation() : 0;
    }

    std::uint64_t begin_per_key_operation(ScopeClass scope) noexcept {
        return scope == ScopeClass::Ordinary ? begin_operation() : 0;
    }

    WriteResult try_write(EventRecord record) noexcept {
        if (!view_) return WriteResult::Disabled;
        Queue queue(view_);
        record.producer_id = queue.producer_id();
        record.sequence = 0;
        record.schema_version = kSchemaVersion;
        return queue.try_write(record);
    }

    void publish_scope(const ScopeObservation &scope) noexcept {
        if (!view_) return;
        Queue(view_).publish_scope(scope);
    }

    ScopeClass scope_class() const noexcept {
        if (!view_) return ScopeClass::Unknown;
        return Queue(view_).scope_class();
    }

    std::uint64_t producer_id() const noexcept {
        return view_ ? Queue(view_).producer_id() : 0;
    }

    std::uint64_t process_creation() const noexcept { return process_creation_; }
    HRESULT last_hr() const noexcept { return last_hr_; }

private:
    void close() noexcept {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }

    HANDLE mapping_ = nullptr;
    void *view_ = nullptr;
    std::wstring mapping_name_;
    std::uint64_t process_creation_ = 0;
    HRESULT last_hr_ = E_UNEXPECTED;
};

class ProcessCollectorView {
public:
    using Queue = SharedQueue<kQueueCapacity>;

    explicit ProcessCollectorView(DWORD process_id,
        std::uint64_t process_creation, bool require_same_user_session = true) noexcept
        : process_id_(process_id), process_creation_(process_creation),
          require_same_user_session_(require_same_user_session) {}
    ProcessCollectorView(const ProcessCollectorView &) = delete;
    ProcessCollectorView &operator=(const ProcessCollectorView &) = delete;
    ~ProcessCollectorView() { close(); }

    bool open() {
        close();
        target_hr_ = static_cast<HRESULT>(E_PENDING);
        mapping_open_hr_ = static_cast<HRESULT>(E_PENDING);
        owner_hr_ = static_cast<HRESULT>(E_PENDING);
        detail::TargetMappingIdentity identity;
        const auto name_result = detail::process_target_mapping(
            process_id_, process_creation_, require_same_user_session_,
            &identity);
        target_hr_ = name_result;
        if (FAILED(name_result)) {
            last_hr_ = name_result;
            return false;
        }
        if (identity.name.empty()) {
            last_hr_ = E_OUTOFMEMORY;
            return false;
        }
        const DWORD desired_access =
            FILE_MAP_READ | FILE_MAP_WRITE | READ_CONTROL;
        mapping_ = OpenFileMappingW(desired_access, FALSE, identity.name.c_str());
        if (!mapping_) {
            mapping_open_hr_ = detail::hresult_from_last_error();
            last_hr_ = mapping_open_hr_;
            return false;
        }
        mapping_open_hr_ = S_OK;
        view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            Queue::storage_bytes());
        if (!view_) {
            last_hr_ = detail::hresult_from_last_error();
            close();
            return false;
        }
        std::vector<unsigned char> owner_sid;
        const HRESULT owner_hr =
            detail::kernel_object_owner_sid(mapping_, &owner_sid);
        if (FAILED(owner_hr) || owner_sid.size() != identity.owner_sid.size() ||
            !EqualSid(owner_sid.data(), identity.owner_sid.data())) {
            owner_hr_ = FAILED(owner_hr) ? owner_hr : E_ACCESSDENIED;
            last_hr_ = owner_hr_;
            close();
            return false;
        }
        owner_hr_ = S_OK;
        Queue queue(view_);
        const auto snapshot = queue.snapshot();
        if (!snapshot.valid || snapshot.process_id != process_id_ ||
            snapshot.process_creation != process_creation_) {
            last_hr_ = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
            close();
            return false;
        }
        last_hr_ = S_OK;
        return true;
    }

    bool initialize(std::uint64_t epoch, bool enabled) noexcept {
        return view_ && Queue(view_).initialize(epoch, enabled);
    }

    bool refresh_heartbeat() noexcept {
        return view_ && Queue(view_).refresh_heartbeat();
    }

    bool disable() noexcept {
        return view_ && Queue(view_).set_enabled(false);
    }

    ReadResult read(EventRecord *record, std::uint64_t *epoch = nullptr) noexcept {
        return view_ ? Queue(view_).try_read(record, epoch) : ReadResult::Invalid;
    }

    QueueSnapshot snapshot() const noexcept {
        return view_ ? Queue(view_).snapshot() : QueueSnapshot{};
    }

    HRESULT last_hr() const noexcept { return last_hr_; }
    HRESULT target_hr() const noexcept { return target_hr_; }
    HRESULT mapping_open_hr() const noexcept { return mapping_open_hr_; }
    HRESULT owner_hr() const noexcept { return owner_hr_; }

private:
    void close() noexcept {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }

    DWORD process_id_;
    std::uint64_t process_creation_;
    bool require_same_user_session_;
    HANDLE mapping_ = nullptr;
    void *view_ = nullptr;
    HRESULT last_hr_ = E_UNEXPECTED;
    HRESULT target_hr_ = static_cast<HRESULT>(E_PENDING);
    HRESULT mapping_open_hr_ = static_cast<HRESULT>(E_PENDING);
    HRESULT owner_hr_ = static_cast<HRESULT>(E_PENDING);
};

} // namespace boshiamy::doctor

#endif
