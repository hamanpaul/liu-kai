#include <windows.h>
#include <commctrl.h>
#include <unknwn.h>
#include <msiquery.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <new>
#include <string>
#include <vector>

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
#include "../wix/sdk/BootstrapperApplicationTypes.h"
#include "../wix/sdk/IBootstrapperEngine.h"
#include "../wix/sdk/BAFunctions.h"
#ifdef __MINGW32__
#pragma pop_macro("__out_ecount_opt")
#pragma pop_macro("__in_z_opt")
#pragma pop_macro("__in_z")
#pragma pop_macro("__out")
#pragma pop_macro("__in")
#endif

#include "../wix/installer-session-guard.hpp"

namespace {
constexpr DWORD kWait = 15'000;
constexpr DWORD kForcedExit = 0xfeedfaceUL;
constexpr HRESULT kBusy = HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING);
constexpr HRESULT kLocalState = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
constexpr HRESULT kRevisionMismatch =
    HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
// WiX 6.0.2 StdBA LoadBAFunctions requests MAKEQWORDVERSION(2024,1,1,0).
constexpr DWORD64 kBaFunctionsApiVersion =
    (static_cast<DWORD64>(2024) << 48) |
    (static_cast<DWORD64>(1) << 32) |
    (static_cast<DWORD64>(1) << 16);

int fail(
    const char* test, const char* stage,
    DWORD win32 = ERROR_GEN_FAILURE, HRESULT hr = E_FAIL) {
    std::fprintf(
        stderr, "BAF %s: %s Win32=%lu HRESULT=0x%08lx\n",
        test, stage, static_cast<unsigned long>(win32),
        static_cast<unsigned long>(static_cast<DWORD>(hr)));
    return 1;
}

DWORD win32(HRESULT hr) {
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : 0;
}

struct Handle {
    explicit Handle(HANDLE initial = nullptr) noexcept : value(initial) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
    HANDLE value;
};

struct Child {
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (!process) return;
        DWORD exit = 0;
        if (!GetExitCodeProcess(process, &exit) || exit == STILL_ACTIVE) {
            TerminateProcess(process, kForcedExit);
            WaitForSingleObject(process, kWait);
        }
        CloseHandle(process);
    }
    HANDLE process{};
};

struct Job {
    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() {
        if (value) CloseHandle(value);
    }
    bool create() {
        value = CreateJobObjectW(nullptr, nullptr);
        if (!value) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(
                value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits))) {
            return true;
        }
        CloseHandle(value);
        value = nullptr;
        return false;
    }
    HANDLE value{};
};

struct LoadedDll {
    LoadedDll() = default;
    LoadedDll(const LoadedDll&) = delete;
    LoadedDll& operator=(const LoadedDll&) = delete;
    ~LoadedDll() { reset(); }
    void reset() noexcept {
        if (module) FreeLibrary(module);
        module = nullptr;
    }
    bool release() noexcept {
        const bool result = module && FreeLibrary(module) != FALSE;
        module = nullptr;
        return result;
    }
    HMODULE module{};
};

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::string ascii_label(const wchar_t* text) {
    std::string result;
    for (; *text; ++text) result.push_back(
        *text <= 0x7f ? static_cast<char>(*text) : L'?');
    return result;
}

bool spawn(
    const char* test, HANDLE job, const std::vector<std::wstring>& args,
    Child& child, const char* stage) {
    wchar_t executable[8192]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) {
        const DWORD error = GetLastError();
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    std::wstring command = quote(executable);
    for (const std::wstring& arg : args) command += L" " + quote(arg);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable, buffer.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) {
        const DWORD error = GetLastError();
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }

    bool started = !job || AssignProcessToJobObject(job, process.hProcess);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (started && ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        started = false;
        error = GetLastError();
    }
    CloseHandle(process.hThread);
    if (!started) {
        TerminateProcess(process.hProcess, kForcedExit);
        WaitForSingleObject(process.hProcess, kWait);
        CloseHandle(process.hProcess);
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    child.process = process.hProcess;
    return true;
}

bool wait_child(
    const char* test, const Child& child, const char* stage,
    DWORD* exit = nullptr) {
    const DWORD wait = WaitForSingleObject(child.process, kWait);
    if (wait != WAIT_OBJECT_0) {
        const DWORD error = wait == WAIT_FAILED ? GetLastError() : wait;
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(child.process, &code)) {
        const DWORD error = GetLastError();
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    if (exit) *exit = code;
    return true;
}

bool child_ok(const char* test, const Child& child, const char* stage) {
    DWORD exit = 0;
    if (!wait_child(test, child, stage, &exit)) return false;
    if (exit == 0) return true;
    fail(test, stage, ERROR_PROCESS_ABORTED, E_FAIL);
    return false;
}

bool create_event(
    const char* test, const std::wstring& name, Handle& event,
    const char* stage) {
    SetLastError(ERROR_SUCCESS);
    event.reset(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
    const DWORD error = GetLastError();
    if (event.value && error != ERROR_ALREADY_EXISTS) return true;
    const DWORD reason = event.value ? ERROR_ALREADY_EXISTS : error;
    event.reset();
    fail(test, stage, reason, HRESULT_FROM_WIN32(reason));
    return false;
}

bool open_event(
    const char* test, const std::wstring& name, DWORD access,
    Handle& event, const char* stage) {
    event.reset(OpenEventW(access, FALSE, name.c_str()));
    if (event.value) return true;
    const DWORD error = GetLastError();
    fail(test, stage, error, HRESULT_FROM_WIN32(error));
    return false;
}

bool signal_event(const char* test, HANDLE event, const char* stage) {
    if (SetEvent(event)) return true;
    const DWORD error = GetLastError();
    fail(test, stage, error, HRESULT_FROM_WIN32(error));
    return false;
}

bool wait_event(const char* test, HANDLE event, const char* stage) {
    const DWORD wait = WaitForSingleObject(event, kWait);
    if (wait == WAIT_OBJECT_0) return true;
    const DWORD error = wait == WAIT_FAILED ? GetLastError() : wait;
    fail(test, stage, error, HRESULT_FROM_WIN32(error));
    return false;
}

bool wait_event_or_child(
    const char* test, HANDLE event, const Child& child, const char* stage) {
    HANDLE handles[] = {event, child.process};
    const DWORD wait = WaitForMultipleObjects(
        static_cast<DWORD>(std::size(handles)), handles, FALSE, kWait);
    if (wait == WAIT_OBJECT_0) return true;
    if (wait == WAIT_OBJECT_0 + 1) {
        DWORD exit = 0;
        const bool have_exit = GetExitCodeProcess(child.process, &exit);
        fail(
            test, stage, have_exit ? exit : GetLastError(),
            E_UNEXPECTED);
        return false;
    }
    const DWORD error = wait == WAIT_FAILED ? GetLastError() : wait;
    fail(test, stage, error, HRESULT_FROM_WIN32(error));
    return false;
}

std::wstring base_name() {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) return {};
    return L"Local\\LiuKai-BAFunctions-Test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(counter.QuadPart);
}

class StubEngine final : public IBootstrapperEngine {
public:
    StubEngine() = default;
    StubEngine(const StubEngine&) = delete;
    StubEngine& operator=(const StubEngine&) = delete;

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown)) {
            *object = static_cast<IUnknown*>(this);
        // WiX declares this IID in IBootstrapperEngine.h; MinGW does not
        // provide a __mingw_uuidof specialization for that SDK interface.
        } else if (IsEqualIID(riid, (GUID{0x6480d616, 0x27a0, 0x44d7,
                {0x90, 0x5b, 0x81, 0x51, 0x2c, 0x29, 0xc2, 0xfb}}))) {
            *object = static_cast<IBootstrapperEngine*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG result =
            static_cast<ULONG>(InterlockedDecrement(&references_));
        if (!result) delete this;
        return result;
    }

    STDMETHODIMP GetPackageCount(DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP GetVariableNumeric(LPCWSTR, LONGLONG*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetVariableString(
        LPCWSTR, LPWSTR, SIZE_T*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetVariableVersion(
        LPCWSTR, LPWSTR, SIZE_T*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP FormatString(LPCWSTR, LPWSTR, SIZE_T*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP EscapeString(LPCWSTR, LPWSTR, SIZE_T*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP EvaluateCondition(LPCWSTR, BOOL*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Log(
        BOOTSTRAPPER_LOG_LEVEL, LPCWSTR message) override {
        logs_.emplace_back(message ? message : L"");
        return S_OK;
    }
    STDMETHODIMP SendEmbeddedError(
        DWORD, LPCWSTR, DWORD, int*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SendEmbeddedProgress(
        DWORD, DWORD, int*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetUpdate(
        LPCWSTR, LPCWSTR, DWORD64, BOOTSTRAPPER_UPDATE_HASH_TYPE,
        LPCWSTR, LPCWSTR) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetLocalSource(
        LPCWSTR, LPCWSTR, LPCWSTR) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetDownloadSource(
        LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetVariableNumeric(LPCWSTR, LONGLONG) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetVariableString(
        LPCWSTR, LPCWSTR, BOOL) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetVariableVersion(LPCWSTR, LPCWSTR) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP CloseSplashScreen() override { return E_NOTIMPL; }
    STDMETHODIMP Detect(HWND) override { return E_NOTIMPL; }
    STDMETHODIMP Plan(BOOTSTRAPPER_ACTION) override { return E_NOTIMPL; }
    STDMETHODIMP Elevate(HWND) override { return E_NOTIMPL; }
    STDMETHODIMP Apply(HWND) override { return E_NOTIMPL; }
    STDMETHODIMP Quit(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP LaunchApprovedExe(
        HWND, LPCWSTR, LPCWSTR, DWORD) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetUpdateSource(LPCWSTR, LPCWSTR) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP CompareVersions(
        LPCWSTR, LPCWSTR, int*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetRelatedBundleVariable(
        LPCWSTR, LPCWSTR, LPWSTR, SIZE_T*) override {
        return E_NOTIMPL;
    }

    bool has_log(const wchar_t* marker) const {
        for (const std::wstring& log : logs_) {
            if (log.find(marker) != std::wstring::npos) return true;
        }
        return false;
    }

private:
    LONG references_{1};
    std::vector<std::wstring> logs_;
};

struct EngineHandle {
    explicit EngineHandle(StubEngine* initial = nullptr) noexcept
        : value(initial) {}
    EngineHandle(const EngineHandle&) = delete;
    EngineHandle& operator=(const EngineHandle&) = delete;
    ~EngineHandle() {
        if (value) value->Release();
    }
    StubEngine* value;
};

bool load_exports(
    const char* test, const wchar_t* dll_path, LoadedDll& dll,
    PFN_BA_FUNCTIONS_CREATE* create, PFN_BA_FUNCTIONS_DESTROY* destroy) {
    dll.module = LoadLibraryW(dll_path);
    if (!dll.module) {
        const DWORD error = GetLastError();
        return fail(test, "LoadLibraryW", error, HRESULT_FROM_WIN32(error)) == 0;
    }
    *create = reinterpret_cast<PFN_BA_FUNCTIONS_CREATE>(
        GetProcAddress(dll.module, "BAFunctionsCreate"));
    if (!*create) {
        const DWORD error = GetLastError();
        return fail(
            test, "BAFunctionsCreate export", error,
            HRESULT_FROM_WIN32(error)) == 0;
    }
    *destroy = reinterpret_cast<PFN_BA_FUNCTIONS_DESTROY>(
        GetProcAddress(dll.module, "BAFunctionsDestroy"));
    if (!*destroy) {
        const DWORD error = GetLastError();
        return fail(
            test, "BAFunctionsDestroy export", error,
            HRESULT_FROM_WIN32(error)) == 0;
    }
    return true;
}

HRESULT WINAPI sentinel_callback(
    BA_FUNCTIONS_MESSAGE, const LPVOID, LPVOID, LPVOID) noexcept {
    return E_UNEXPECTED;
}

bool callback_unpublished(
    const char* test, const BA_FUNCTIONS_CREATE_RESULTS& results,
    const void* context) {
    if (results.pfnBAFunctionsProc == sentinel_callback &&
        results.pvBAFunctionsProcContext == context) {
        return true;
    }
    fail(test, "failure published callback", ERROR_INVALID_STATE, E_UNEXPECTED);
    return false;
}

bool callback_succeeded(
    const char* test, const BA_FUNCTIONS_CREATE_RESULTS& results) {
    if (!results.pfnBAFunctionsProc ||
        results.pfnBAFunctionsProc == sentinel_callback) {
        fail(
            test, "success did not publish callback",
            ERROR_INVALID_STATE, E_HANDLE);
        return false;
    }
    const HRESULT callback_hr = results.pfnBAFunctionsProc(
        BA_FUNCTIONS_MESSAGE_ONCREATE, nullptr, nullptr,
        results.pvBAFunctionsProcContext);
    if (callback_hr == S_OK) return true;
    fail(test, "published callback", win32(callback_hr), callback_hr);
    return false;
}

void destroy_once(PFN_BA_FUNCTIONS_DESTROY destroy) {
    if (!destroy) return;
    BA_FUNCTIONS_DESTROY_ARGS args{sizeof(BA_FUNCTIONS_DESTROY_ARGS), FALSE};
    BA_FUNCTIONS_DESTROY_RESULTS results{
        sizeof(BA_FUNCTIONS_DESTROY_RESULTS), FALSE};
    destroy(&args, &results);
}

struct CallPlan {
    DWORD args_size{sizeof(BA_FUNCTIONS_CREATE_ARGS)};
    DWORD results_size{sizeof(BA_FUNCTIONS_CREATE_RESULTS)};
    DWORD64 api_version{kBaFunctionsApiVersion};
    DWORD command_size{sizeof(BOOTSTRAPPER_COMMAND)};
    bool null_args{};
    bool null_results{};
    bool null_engine{};
    bool null_command{};
    bool success{};
    BOOTSTRAPPER_ACTION action{BOOTSTRAPPER_ACTION_INSTALL};
    BOOTSTRAPPER_DISPLAY display{BOOTSTRAPPER_DISPLAY_PASSIVE};
    BOOTSTRAPPER_RELATION_TYPE relation{BOOTSTRAPPER_RELATION_NONE};
    HRESULT expected{E_INVALIDARG};
    const wchar_t* marker{};
};

bool plan_for(const wchar_t* name, CallPlan& plan) {
    plan = CallPlan{};
    if (!std::wcscmp(name, L"malformed-null-args")) {
        plan.null_args = true;
        plan.expected = E_POINTER;
    } else if (!std::wcscmp(name, L"malformed-null-results")) {
        plan.null_results = true;
        plan.expected = E_POINTER;
    } else if (!std::wcscmp(name, L"malformed-args-size")) {
        plan.args_size = static_cast<DWORD>(sizeof(BA_FUNCTIONS_CREATE_ARGS) - 1);
        plan.expected = E_INVALIDARG;
    } else if (!std::wcscmp(name, L"malformed-results-size")) {
        plan.results_size =
            static_cast<DWORD>(sizeof(BA_FUNCTIONS_CREATE_RESULTS) - 1);
        plan.expected = E_INVALIDARG;
    } else if (!std::wcscmp(name, L"malformed-null-command")) {
        plan.null_command = true;
        plan.marker = L"LIUKAI_GUARD_ERROR";
        plan.expected = E_INVALIDARG;
    } else if (!std::wcscmp(name, L"malformed-command-size")) {
        plan.command_size =
            static_cast<DWORD>(sizeof(BOOTSTRAPPER_COMMAND) - 1);
        plan.marker = L"LIUKAI_GUARD_ERROR";
        plan.expected = E_INVALIDARG;
    } else if (!std::wcscmp(name, L"malformed-api-version")) {
        plan.api_version ^= 1;
        plan.marker = L"LIUKAI_GUARD_ERROR";
        plan.expected = kRevisionMismatch;
    } else if (!std::wcscmp(name, L"malformed-null-engine")) {
        plan.null_engine = true;
        plan.expected = E_INVALIDARG;
    } else if (!std::wcscmp(name, L"embedded-upgrade-uninstall")) {
        plan.success = true;
        plan.action = BOOTSTRAPPER_ACTION_UNINSTALL;
        plan.display = BOOTSTRAPPER_DISPLAY_EMBEDDED;
        plan.relation = BOOTSTRAPPER_RELATION_UPGRADE;
        plan.expected = S_OK;
        plan.marker = L"LIUKAI_GUARD_EMBEDDED_UPGRADE";
    } else if (!std::wcscmp(name, L"embedded-upgrade-install")) {
        plan.success = true;
        plan.action = BOOTSTRAPPER_ACTION_INSTALL;
        plan.display = BOOTSTRAPPER_DISPLAY_EMBEDDED;
        plan.relation = BOOTSTRAPPER_RELATION_UPGRADE;
        plan.expected = S_OK;
        plan.marker = L"LIUKAI_GUARD_EMBEDDED_UPGRADE";
    } else {
        plan.expected = kBusy;
        plan.marker = L"LIUKAI_GUARD_BUSY";
        if (!std::wcscmp(name, L"busy-relation-none")) {
            plan.display = BOOTSTRAPPER_DISPLAY_EMBEDDED;
            plan.relation = BOOTSTRAPPER_RELATION_NONE;
        } else if (!std::wcscmp(name, L"busy-relation-patch")) {
            plan.display = BOOTSTRAPPER_DISPLAY_EMBEDDED;
            plan.relation = BOOTSTRAPPER_RELATION_PATCH;
        } else if (!std::wcscmp(name, L"busy-relation-addon")) {
            plan.display = BOOTSTRAPPER_DISPLAY_EMBEDDED;
            plan.relation = BOOTSTRAPPER_RELATION_ADDON;
        } else if (!std::wcscmp(name, L"busy-action-repair")) {
            plan.display = BOOTSTRAPPER_DISPLAY_EMBEDDED;
            plan.relation = BOOTSTRAPPER_RELATION_UPGRADE;
            plan.action = BOOTSTRAPPER_ACTION_REPAIR;
        } else if (!std::wcscmp(name, L"busy-display-none")) {
            plan.display = BOOTSTRAPPER_DISPLAY_NONE;
            plan.relation = BOOTSTRAPPER_RELATION_UPGRADE;
        } else if (!std::wcscmp(name, L"busy-display-passive")) {
            plan.display = BOOTSTRAPPER_DISPLAY_PASSIVE;
            plan.relation = BOOTSTRAPPER_RELATION_UPGRADE;
        } else if (!std::wcscmp(name, L"busy-after-destroy") ||
            !std::wcscmp(name, L"busy-after-terminate") ||
            !std::wcscmp(name, L"busy-after-fresh")) {
            plan.display = BOOTSTRAPPER_DISPLAY_PASSIVE;
            plan.relation = BOOTSTRAPPER_RELATION_NONE;
        } else {
            return false;
        }
    }
    return true;
}

int run_call_child(const wchar_t* dll_path, const wchar_t* case_name) {
    const std::string label = ascii_label(case_name);
    const char* test = label.c_str();
    CallPlan plan;
    if (!plan_for(case_name, plan))
        return fail(test, "unknown case", ERROR_BAD_ARGUMENTS, E_INVALIDARG);

    LoadedDll dll;
    PFN_BA_FUNCTIONS_CREATE create = nullptr;
    PFN_BA_FUNCTIONS_DESTROY destroy = nullptr;
    if (!load_exports(test, dll_path, dll, &create, &destroy)) return 1;
    StubEngine* engine = new (std::nothrow) StubEngine();
    if (!engine)
        return fail(test, "engine allocation", ERROR_OUTOFMEMORY, E_OUTOFMEMORY);
    EngineHandle engine_handle(engine);

    BOOTSTRAPPER_COMMAND command{};
    command.cbSize = plan.command_size;
    command.action = plan.action;
    command.display = plan.display;
    command.relationType = plan.relation;
    BA_FUNCTIONS_CREATE_ARGS args{
        plan.args_size, plan.api_version,
        plan.null_engine ? nullptr : engine, plan.null_command ? nullptr : &command};
    int sentinel_context = 0;
    BA_FUNCTIONS_CREATE_RESULTS results{
        plan.results_size, sentinel_callback, &sentinel_context};

    const HRESULT hr = create(
        plan.null_args ? nullptr : &args,
        plan.null_results ? nullptr : &results);
    bool ok = hr == plan.expected;
    if (!ok) fail(test, "actual HRESULT", win32(hr), hr);
    if (!plan.null_results) {
        if (plan.success) {
            if (!callback_succeeded(test, results)) ok = false;
        } else if (!callback_unpublished(test, results, &sentinel_context)) {
            ok = false;
        }
    }
    if (plan.marker && !engine->has_log(plan.marker)) {
        fail(test, "expected Log marker", ERROR_INVALID_DATA, E_UNEXPECTED);
        ok = false;
    }

    destroy_once(destroy);
    if (!dll.release()) {
        fail(test, "FreeLibrary", ERROR_INVALID_HANDLE, E_HANDLE);
        ok = false;
    }
    return ok ? 0 : 1;
}

int run_call_parent(
    const wchar_t* dll_path, const wchar_t* case_name) {
    Child child;
    if (!spawn(
            "parent", nullptr,
            {L"--call-child", dll_path, case_name},
            child, "spawn call child")) {
        return 1;
    }
    return child_ok("parent", child, "call child") ? 0 : 1;
}

int run_session_ba(
    const wchar_t* dll_path, const wchar_t* ready_name,
    const wchar_t* destroy_name) {
    constexpr const char kTest[] = "session-ba";
    Handle ready, destroy_event;
    if (!open_event(
            kTest, ready_name, EVENT_MODIFY_STATE, ready, "open ready") ||
        !open_event(
            kTest, destroy_name, SYNCHRONIZE,
            destroy_event, "open destroy")) {
        return 1;
    }

    LoadedDll dll;
    PFN_BA_FUNCTIONS_CREATE create = nullptr;
    PFN_BA_FUNCTIONS_DESTROY destroy = nullptr;
    if (!load_exports(kTest, dll_path, dll, &create, &destroy)) return 1;
    StubEngine* engine = new (std::nothrow) StubEngine();
    if (!engine) return fail(kTest, "engine allocation", ERROR_OUTOFMEMORY, E_OUTOFMEMORY);
    EngineHandle engine_handle(engine);

    BOOTSTRAPPER_COMMAND command{};
    command.cbSize = sizeof(command);
    command.action = BOOTSTRAPPER_ACTION_INSTALL;
    command.display = BOOTSTRAPPER_DISPLAY_PASSIVE;
    command.relationType = BOOTSTRAPPER_RELATION_NONE;
    BA_FUNCTIONS_CREATE_ARGS args{
        sizeof(args), kBaFunctionsApiVersion, engine, &command};
    int first_context = 0;
    BA_FUNCTIONS_CREATE_RESULTS results{
        sizeof(results), sentinel_callback, &first_context};
    const HRESULT first_hr = create(&args, &results);
    bool ok = first_hr == S_OK;
    if (!ok) fail(kTest, "normal create", win32(first_hr), first_hr);
    if (ok && !callback_succeeded(kTest, results)) ok = false;
    if (!engine->has_log(L"LIUKAI_GUARD_ACQUIRED")) {
        fail(kTest, "acquired Log marker", ERROR_INVALID_DATA, E_UNEXPECTED);
        ok = false;
    }

    int reentry_context = 0;
    BA_FUNCTIONS_CREATE_RESULTS reentry{
        sizeof(reentry), sentinel_callback, &reentry_context};
    const HRESULT reentry_hr = create(&args, &reentry);
    if (reentry_hr != kLocalState) {
        fail(kTest, "same-process reentry", win32(reentry_hr), reentry_hr);
        ok = false;
    }
    if (!callback_unpublished(kTest, reentry, &reentry_context)) ok = false;
    if (!engine->has_log(L"LIUKAI_GUARD_ERROR")) {
        fail(kTest, "reentry Log marker", ERROR_INVALID_DATA, E_UNEXPECTED);
        ok = false;
    }

    if (ok && !signal_event(kTest, ready.value, "signal ready")) ok = false;
    if (ok && !wait_event(kTest, destroy_event.value, "wait destroy")) ok = false;

    destroy_once(destroy);
    destroy_once(destroy);
    if (!dll.release()) {
        fail(kTest, "FreeLibrary", ERROR_INVALID_HANDLE, E_HANDLE);
        ok = false;
    }
    return ok ? 0 : 1;
}

int run_keeper(
    const wchar_t* mode, const wchar_t* dll_path,
    const wchar_t* ready_name, const wchar_t* destroy_name,
    const wchar_t* held_name, const wchar_t* end_name,
    const wchar_t* finished_name, const wchar_t* release_name) {
    constexpr const char kTest[] = "keeper";
    const bool normal_destroy = !std::wcscmp(mode, L"destroy");
    const bool force_terminate = !std::wcscmp(mode, L"terminate");
    if (normal_destroy == force_terminate)
        return fail(kTest, "unknown end mode", ERROR_BAD_ARGUMENTS, E_INVALIDARG);

    Handle ready, destroy_event, held, end_event, finished, release;
    if (!open_event(
            kTest, ready_name, SYNCHRONIZE, ready, "open BA ready") ||
        !open_event(
            kTest, destroy_name, EVENT_MODIFY_STATE,
            destroy_event, "open BA destroy") ||
        !open_event(
            kTest, held_name, EVENT_MODIFY_STATE, held, "open held") ||
        !open_event(
            kTest, end_name, SYNCHRONIZE, end_event, "open BA end") ||
        !open_event(
            kTest, finished_name, EVENT_MODIFY_STATE,
            finished, "open BA finished") ||
        !open_event(
            kTest, release_name, SYNCHRONIZE,
            release, "open keeper release")) {
        return 1;
    }

    Child ba;
    if (!spawn(
            kTest, nullptr,
            {L"--session-ba", dll_path, ready_name, destroy_name},
            ba, "spawn BA child") ||
        !wait_event_or_child(kTest, ready.value, ba, "BA did not acquire") ||
        !signal_event(kTest, held.value, "signal keeper held") ||
        !wait_event(kTest, end_event.value, "wait BA end request")) {
        return 1;
    }

    bool ended = false;
    if (normal_destroy) {
        if (signal_event(
                kTest, destroy_event.value, "request normal BA Destroy")) {
            ended = child_ok(kTest, ba, "normal BA exit");
        }
    } else {
        if (!TerminateProcess(ba.process, kForcedExit)) {
            const DWORD error = GetLastError();
            fail(kTest, "terminate exact BA", error, HRESULT_FROM_WIN32(error));
        } else {
            DWORD exit = 0;
            ended = wait_child(kTest, ba, "terminated BA exit", &exit) &&
                exit == kForcedExit;
            if (!ended)
                fail(kTest, "forced BA exit code", ERROR_INVALID_STATE, E_UNEXPECTED);
        }
    }
    if (!ended) return 1;
    if (!signal_event(kTest, finished.value, "signal BA finished") ||
        !wait_event(kTest, release.value, "wait keeper release")) {
        return 1;
    }
    return 0;
}

bool mutex_absent(const char* stage) {
    SetLastError(ERROR_SUCCESS);
    Handle mutex(OpenMutexW(
        SYNCHRONIZE, FALSE,
        liukai::installer::kInstallerSessionGuardName));
    const DWORD error = GetLastError();
    if (mutex.value) {
        mutex.reset();
        fail(
            stage, "product mutex exists",
            ERROR_ALREADY_EXISTS,
            HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING));
        return false;
    }
    if (error == ERROR_FILE_NOT_FOUND) return true;
    fail(
        stage, "mutex probe", error,
        HRESULT_FROM_WIN32(error));
    return false;
}

bool run_call_case(
    Job& job, const std::wstring& dll, const std::wstring& case_name) {
    Child parent;
    return spawn(
        "controller", job.value,
        {L"--call-parent", dll, case_name},
        parent, "spawn call parent") &&
        child_ok("controller", parent, "call case");
}

bool keeper_cycle(
    Job& job, const std::wstring& dll, const std::wstring& base,
    const wchar_t* mode, const wchar_t* suffix,
    const wchar_t* after_case, bool embedded_matrix) {
    const std::wstring prefix = base + L"-" + suffix;
    const std::wstring ready = prefix + L"-ba-ready";
    const std::wstring destroy = prefix + L"-ba-destroy";
    const std::wstring held = prefix + L"-keeper-held";
    const std::wstring end = prefix + L"-ba-end";
    const std::wstring finished = prefix + L"-ba-finished";
    const std::wstring release = prefix + L"-keeper-release";
    Handle ready_handle, destroy_handle, held_handle;
    Handle end_handle, finished_handle, release_handle;
    const char* test = !std::wcscmp(mode, L"destroy")
        ? "keeper-destroy" : "keeper-terminate";
    if (!create_event(test, ready, ready_handle, "create BA ready") ||
        !create_event(test, destroy, destroy_handle, "create BA destroy") ||
        !create_event(test, held, held_handle, "create keeper held") ||
        !create_event(test, end, end_handle, "create BA end") ||
        !create_event(test, finished, finished_handle, "create BA finished") ||
        !create_event(test, release, release_handle, "create keeper release")) {
        return false;
    }

    Child keeper;
    if (!spawn(
            test, job.value,
            {L"--keeper", mode, dll, ready, destroy, held, end, finished, release},
            keeper, "spawn keeper") ||
        !wait_event_or_child(
            test, held_handle.value, keeper, "keeper did not hold")) {
        return false;
    }

    if (embedded_matrix &&
        (!run_call_case(job, dll, L"embedded-upgrade-uninstall") ||
         !run_call_case(job, dll, L"embedded-upgrade-install") ||
         !run_call_case(job, dll, L"busy-relation-none") ||
         !run_call_case(job, dll, L"busy-relation-patch") ||
         !run_call_case(job, dll, L"busy-relation-addon") ||
         !run_call_case(job, dll, L"busy-action-repair") ||
         !run_call_case(job, dll, L"busy-display-none") ||
         !run_call_case(job, dll, L"busy-display-passive"))) {
        return false;
    }

    if (!signal_event(test, end_handle.value, "end BA") ||
        !wait_event_or_child(
            test, finished_handle.value, keeper, "BA did not end")) {
        return false;
    }
    if (WaitForSingleObject(keeper.process, 0) != WAIT_TIMEOUT) {
        fail(test, "keeper exited before release", ERROR_INVALID_STATE, E_UNEXPECTED);
        return false;
    }
    if (!run_call_case(job, dll, after_case) ||
        !signal_event(test, release_handle.value, "release keeper") ||
        !child_ok(test, keeper, "keeper exit")) {
        return false;
    }
    return mutex_absent("keeper-cleanup");
}

int controller(const wchar_t* dll_path) {
    constexpr const char kTest[] = "controller";
    if (!mutex_absent("environment-busy")) {
        std::fputs("environment_busy=1\n", stdout);
        return 1;
    }
    Job job;
    if (!job.create()) {
        const DWORD error = GetLastError();
        return fail(kTest, "cleanup job", error, HRESULT_FROM_WIN32(error));
    }
    std::wstring base = base_name();
    if (base.empty()) {
        const DWORD error = GetLastError();
        return fail(kTest, "unique events", error, HRESULT_FROM_WIN32(error));
    }
    const std::wstring dll(dll_path);

    const wchar_t* malformed[] = {
        L"malformed-null-args",
        L"malformed-null-results",
        L"malformed-args-size",
        L"malformed-results-size",
        L"malformed-null-engine",
        L"malformed-null-command",
        L"malformed-command-size",
        L"malformed-api-version",
    };
    for (const wchar_t* test : malformed)
        if (!run_call_case(job, dll, test)) return 1;

    if (!keeper_cycle(
            job, dll, base, L"destroy", L"normal",
            L"busy-after-destroy", true) ||
        !keeper_cycle(
            job, dll, base, L"terminate", L"forced",
            L"busy-after-terminate", false) ||
        !keeper_cycle(
            job, dll, base, L"destroy", L"fresh",
            L"busy-after-fresh", false) ||
        !mutex_absent("final-cleanup")) {
        return 1;
    }

    std::puts(
        "component-observed=true; "
        "official_entries=BAFunctionsCreate+BAFunctionsDestroy; "
        "keeper_cycles=3; call_cases=19; "
        "installer_runs=0; msi_actions=0; "
        "real_msi_burn_interruption_recovery=not_tested");
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !std::wcscmp(argv[1], L"--controller"))
        return controller(argv[2]);
    if (argc == 4 && !std::wcscmp(argv[1], L"--call-parent"))
        return run_call_parent(argv[2], argv[3]);
    if (argc == 4 && !std::wcscmp(argv[1], L"--call-child"))
        return run_call_child(argv[2], argv[3]);
    if (argc == 5 && !std::wcscmp(argv[1], L"--session-ba"))
        return run_session_ba(argv[2], argv[3], argv[4]);
    if (argc == 10 && !std::wcscmp(argv[1], L"--keeper"))
        return run_keeper(
            argv[2], argv[3], argv[4], argv[5], argv[6],
            argv[7], argv[8], argv[9]);
    return fail("fixture", "arguments", ERROR_BAD_ARGUMENTS, E_INVALIDARG);
}
