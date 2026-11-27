#include <windows.h>
#include <commctrl.h>
#include <unknwn.h>
#include <msiquery.h>

#include <cstdint>
#include <cwchar>

#ifdef __MINGW32__
#pragma push_macro("__in")
#pragma push_macro("__out")
#pragma push_macro("__in_z")
#pragma push_macro("__in_z_opt")
#pragma push_macro("__out_ecount_opt")
#undef __in
#undef __out
#undef __in_z
#undef __in_z_opt
#undef __out_ecount_opt
#define __in _In_
#define __out _Out_
#define __in_z _In_z_
#define __in_z_opt _In_opt_z_
#define __out_ecount_opt(count) _Out_writes_opt_(count)
#endif
#include "sdk/BootstrapperApplicationTypes.h"
#include "sdk/IBootstrapperEngine.h"
#include "sdk/BAFunctions.h"
#ifdef __MINGW32__
#pragma pop_macro("__out_ecount_opt")
#pragma pop_macro("__in_z_opt")
#pragma pop_macro("__in_z")
#pragma pop_macro("__out")
#pragma pop_macro("__in")
#endif

#include "installer-session-guard.hpp"

namespace {
// WiX 6.0.2 StdBA LoadBAFunctions requests MAKEQWORDVERSION(2024,1,1,0).
constexpr DWORD64 kBaFunctionsApiVersion =
    (static_cast<DWORD64>(2024) << 48) |
    (static_cast<DWORD64>(1) << 32) |
    (static_cast<DWORD64>(1) << 16);
HANDLE gLocalGuard = nullptr;

HRESULT WINAPI NoopBaFunction(
    BA_FUNCTIONS_MESSAGE, const LPVOID, LPVOID, LPVOID) noexcept {
    return S_OK;
}

void LogGuard(
    IBootstrapperEngine* engine, const wchar_t* marker,
    const wchar_t* stage, HRESULT hr) noexcept {
    wchar_t message[256]{};
    std::swprintf(
        message, sizeof(message) / sizeof(message[0]),
        L"%ls stage=%ls hr=0x%08lx",
        marker, stage, static_cast<unsigned long>(static_cast<DWORD>(hr)));
    engine->Log(BOOTSTRAPPER_LOG_LEVEL_STANDARD, message);
}

void LogAcquired(IBootstrapperEngine* engine, DWORD keeperPid) noexcept {
    wchar_t message[128]{};
    std::swprintf(
        message, sizeof(message) / sizeof(message[0]),
        L"LIUKAI_GUARD_ACQUIRED keeper_pid=%lu",
        static_cast<unsigned long>(keeperPid));
    engine->Log(BOOTSTRAPPER_LOG_LEVEL_STANDARD, message);
}

void PublishResult(BA_FUNCTIONS_CREATE_RESULTS* results) noexcept {
    results->pfnBAFunctionsProc = NoopBaFunction;
    results->pvBAFunctionsProcContext = nullptr;
}

bool IsEmbeddedUpgradeRestore(const BOOTSTRAPPER_COMMAND& command) noexcept {
    return command.relationType == BOOTSTRAPPER_RELATION_UPGRADE &&
        command.display == BOOTSTRAPPER_DISPLAY_EMBEDDED &&
        (command.action == BOOTSTRAPPER_ACTION_UNINSTALL ||
         command.action == BOOTSTRAPPER_ACTION_INSTALL);
}

void ShowGuardFailure(const BOOTSTRAPPER_COMMAND& command, bool busy) noexcept {
    if (command.display != BOOTSTRAPPER_DISPLAY_FULL) return;
    MessageBoxW(
        nullptr,
        busy
            ? L"已有嘸．改安裝或解除安裝正在執行。請等待完成後，再重新開啟本安裝程式。"
            : L"無法確認安裝保護狀態，本次安裝已停止。",
        L"嘸．改安裝程式",
        MB_OK | (busy ? MB_ICONWARNING : MB_ICONERROR));
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI BAFunctionsCreate(
    const BA_FUNCTIONS_CREATE_ARGS* args,
    BA_FUNCTIONS_CREATE_RESULTS* results) noexcept {
    if (!args || !results) return E_POINTER;
    if (args->cbSize != sizeof(*args) ||
        results->cbSize != sizeof(*results)) return E_INVALIDARG;
    if (!args->pEngine) return E_INVALIDARG;
    if (!args->pCommand ||
        args->pCommand->cbSize != sizeof(*args->pCommand)) {
        LogGuard(args->pEngine, L"LIUKAI_GUARD_ERROR", L"command", E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (args->qwBAFunctionsAPIVersion != kBaFunctionsApiVersion) {
        constexpr HRESULT error =
            HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        LogGuard(
            args->pEngine, L"LIUKAI_GUARD_ERROR", L"api-version", error);
        return error;
    }

    const BOOTSTRAPPER_COMMAND& command = *args->pCommand;
    if (IsEmbeddedUpgradeRestore(command)) {
        args->pEngine->Log(
            BOOTSTRAPPER_LOG_LEVEL_STANDARD,
            L"LIUKAI_GUARD_EMBEDDED_UPGRADE");
        PublishResult(results);
        return S_OK;
    }
    if (gLocalGuard) {
        constexpr HRESULT error = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        LogGuard(args->pEngine, L"LIUKAI_GUARD_ERROR", L"local-state", error);
        ShowGuardFailure(command, false);
        return error;
    }

    HANDLE parent = nullptr;
    HRESULT hr = liukai::installer::OpenInstallerGuardParent(&parent);
    if (FAILED(hr)) {
        if (parent) CloseHandle(parent);
        LogGuard(args->pEngine, L"LIUKAI_GUARD_ERROR", L"parent", hr);
        ShowGuardFailure(command, false);
        return hr;
    }
    const DWORD keeperPid = GetProcessId(parent);
    HANDLE local = nullptr;
    hr = liukai::installer::AcquireInstallerSessionGuard(
        liukai::installer::kInstallerSessionGuardName, parent, &local);
    CloseHandle(parent);
    if (FAILED(hr)) {
        if (local) CloseHandle(local);
        const bool busy =
            hr == HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING);
        LogGuard(
            args->pEngine,
            busy ? L"LIUKAI_GUARD_BUSY" : L"LIUKAI_GUARD_ERROR",
            L"acquire",
            hr);
        ShowGuardFailure(command, busy);
        return hr;
    }
    if (!local) {
        constexpr HRESULT error = E_HANDLE;
        LogGuard(
            args->pEngine, L"LIUKAI_GUARD_ERROR", L"local-handle", error);
        ShowGuardFailure(command, false);
        return error;
    }
    gLocalGuard = local;
    LogAcquired(args->pEngine, keeperPid);
    PublishResult(results);
    return S_OK;
}

extern "C" __declspec(dllexport) void WINAPI BAFunctionsDestroy(
    const BA_FUNCTIONS_DESTROY_ARGS*, BA_FUNCTIONS_DESTROY_RESULTS*) noexcept {
    HANDLE local = gLocalGuard;
    gLocalGuard = nullptr;
    if (local) CloseHandle(local);
}
