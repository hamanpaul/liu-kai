#pragma once

#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>

namespace liukai::installer {

inline constexpr wchar_t kInstallerSessionGuardName[] =
    L"Global\\LiuKai-Burn-94C1F608-8C0F-424C-A572-E0F4BCB10327-v1";

namespace detail {

inline DWORD guard_last_error() noexcept {
    const DWORD error = GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
}

inline HRESULT guard_win32_hresult(DWORD error) noexcept {
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

struct GuardHandle {
    explicit GuardHandle(HANDLE initial = nullptr) noexcept : value(initial) {}
    GuardHandle(const GuardHandle&) = delete;
    GuardHandle& operator=(const GuardHandle&) = delete;
    ~GuardHandle() { reset(); }
    void reset(HANDLE next = nullptr) noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
    HANDLE release() noexcept {
        const HANDLE result = value;
        value = nullptr;
        return result;
    }
    HANDLE value;
};

constexpr DWORD kTokenUserBufferSize =
    static_cast<DWORD>(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE);

inline DWORD guard_process_user_sid(
        HANDLE process, BYTE* storage, DWORD capacity, PSID* sid) noexcept {
    *sid = nullptr;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token))
        return guard_last_error();
    GuardHandle token(raw_token);
    DWORD written = 0;
    if (!GetTokenInformation(
            token.value, TokenUser, storage, capacity, &written)) {
        return guard_last_error();
    }
    if (written < sizeof(TOKEN_USER) || written > capacity)
        return ERROR_INVALID_DATA;
    auto* user = reinterpret_cast<TOKEN_USER*>(storage);
    if (!user->User.Sid || !IsValidSid(user->User.Sid) ||
        GetLengthSid(user->User.Sid) > SECURITY_MAX_SID_SIZE)
        return ERROR_INVALID_SID;
    *sid = user->User.Sid;
    return ERROR_SUCCESS;
}

inline HRESULT guard_process_live(HANDLE process) noexcept {
    const DWORD result = WaitForSingleObject(process, 0);
    if (result == WAIT_TIMEOUT) return S_OK;
    if (result == WAIT_FAILED)
        return guard_win32_hresult(guard_last_error());
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

} // namespace detail

inline HRESULT OpenInstallerGuardParent(HANDLE* parent) noexcept {
    if (!parent) return E_POINTER;
    *parent = nullptr;

    detail::GuardHandle snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE)
        return detail::guard_win32_hresult(detail::guard_last_error());

    const DWORD own_pid = GetCurrentProcessId();
    DWORD parent_pid = 0;
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry))
        return detail::guard_win32_hresult(detail::guard_last_error());
    for (;;) {
        if (entry.th32ProcessID == own_pid) {
            if (found) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            parent_pid = entry.th32ParentProcessID;
            found = true;
        }
        if (Process32NextW(snapshot.value, &entry)) continue;
        const DWORD enumeration_error = GetLastError();
        if (enumeration_error != ERROR_NO_MORE_FILES)
            return detail::guard_win32_hresult(enumeration_error);
        break;
    }
    if (!found || !parent_pid || parent_pid == own_pid)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    detail::GuardHandle candidate(OpenProcess(
        PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, parent_pid));
    if (!candidate.value)
        return detail::guard_win32_hresult(detail::guard_last_error());
    HRESULT result = detail::guard_process_live(candidate.value);
    if (FAILED(result)) return result;

    FILETIME own_created{}, own_exit{}, own_kernel{}, own_user{};
    FILETIME parent_created{}, parent_exit{}, parent_kernel{}, parent_user{};
    if (!GetProcessTimes(
            GetCurrentProcess(), &own_created, &own_exit, &own_kernel, &own_user) ||
        !GetProcessTimes(
            candidate.value, &parent_created, &parent_exit,
            &parent_kernel, &parent_user))
        return detail::guard_win32_hresult(detail::guard_last_error());
    if (CompareFileTime(&parent_created, &own_created) > 0)
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

    alignas(TOKEN_USER) BYTE own_storage[detail::kTokenUserBufferSize]{};
    alignas(TOKEN_USER) BYTE parent_storage[detail::kTokenUserBufferSize]{};
    PSID own_sid = nullptr;
    PSID parent_sid = nullptr;
    DWORD error = detail::guard_process_user_sid(
        GetCurrentProcess(), own_storage, detail::kTokenUserBufferSize, &own_sid);
    if (error) return detail::guard_win32_hresult(error);
    error = detail::guard_process_user_sid(
        candidate.value, parent_storage, detail::kTokenUserBufferSize, &parent_sid);
    if (error) return detail::guard_win32_hresult(error);
    if (!EqualSid(own_sid, parent_sid)) return E_ACCESSDENIED;
    result = detail::guard_process_live(candidate.value);
    if (FAILED(result)) return result;

    *parent = candidate.release();
    return S_OK;
}

inline HRESULT AcquireInstallerSessionGuard(
        const wchar_t* name, HANDLE keeperProcess, HANDLE* localHandle) noexcept {
    if (!localHandle) return E_POINTER;
    *localHandle = nullptr;
    if (!name || !*name || !keeperProcess) return E_INVALIDARG;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;AU)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        const DWORD error = detail::guard_last_error();
        if (descriptor) LocalFree(descriptor);
        return detail::guard_win32_hresult(error);
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    SetLastError(ERROR_SUCCESS);
    HANDLE local = CreateMutexExW(
        &attributes, name, 0, SYNCHRONIZE);
    const DWORD create_error = GetLastError();
    LocalFree(descriptor);

    detail::GuardHandle mutex(local);
    if (!mutex.value)
        return detail::guard_win32_hresult(create_error);

    // Existence is the busy signal; the mutex has no thread ownership.
    if (create_error == ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING);

    if (create_error != ERROR_SUCCESS)
        return detail::guard_win32_hresult(create_error);

    HANDLE remote = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(), mutex.value, keeperProcess, &remote,
            0, FALSE, DUPLICATE_SAME_ACCESS))
        return detail::guard_win32_hresult(detail::guard_last_error());

    *localHandle = mutex.release();
    return S_OK;
}

} // namespace liukai::installer
