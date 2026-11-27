#include <windows.h>
#include "../wix/installer-session-guard.hpp"
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

namespace {
constexpr DWORD kWait = 10'000;
constexpr HRESULT kBusy = HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING);

int fail(int test, const char* stage, DWORD win32 = 0, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "GUARD%03d %s Win32=%lu HRESULT=0x%08lx\n",
        test, stage, static_cast<unsigned long>(win32),
        static_cast<unsigned long>(static_cast<DWORD>(hr)));
    return 1;
}

DWORD win32(HRESULT hr) {
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : 0;
}

struct Handle {
    explicit Handle(HANDLE value = nullptr) : value(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) {
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
            TerminateProcess(process, 0xfeedfaceUL);
            WaitForSingleObject(process, kWait);
        }
        CloseHandle(process);
    }
    HANDLE process{};
    DWORD pid{};
};

struct Job {
    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() { if (value) CloseHandle(value); }
    bool create() {
        value = CreateJobObjectW(nullptr, nullptr);
        if (!value) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits))) return true;
        CloseHandle(value);
        value = nullptr;
        return false;
    }
    HANDLE value{};
};

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

bool spawn(int test, HANDLE job, const std::vector<std::wstring>& args,
        Child& child, const char* stage) {
    wchar_t executable[8192]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable))
        return fail(test, stage, GetLastError(), E_HANDLE) == 0;
    std::wstring command = quote(executable);
    for (const auto& arg : args) command += L" " + quote(arg);
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
    if (!CreateProcessW(executable, buffer.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &process)) {
        const DWORD error = GetLastError();
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    bool started = !job || AssignProcessToJobObject(job, process.hProcess);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (started && ResumeThread(process.hThread) ==
            static_cast<DWORD>(-1)) {
        started = false;
        error = GetLastError();
    }
    CloseHandle(process.hThread);
    if (!started) {
        TerminateProcess(process.hProcess, 0xfeed0001UL);
        WaitForSingleObject(process.hProcess, kWait);
        CloseHandle(process.hProcess);
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    child.process = process.hProcess;
    child.pid = process.dwProcessId;
    return true;
}

bool wait_child(int test, const Child& child, const char* stage,
        DWORD* exit = nullptr) {
    const DWORD wait = WaitForSingleObject(child.process, kWait);
    if (wait != WAIT_OBJECT_0) {
        const DWORD error = wait == WAIT_FAILED ? GetLastError() : wait;
        return fail(test, stage, error, HRESULT_FROM_WIN32(error)) == 0;
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

bool child_ok(int test, const Child& child, const char* stage) {
    DWORD exit = 0;
    if (!wait_child(test, child, stage, &exit)) return false;
    if (exit == 0) return true;
    fail(test, stage, ERROR_PROCESS_ABORTED, E_FAIL);
    return false;
}

bool create_event(int test, const std::wstring& name, Handle& event,
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

bool signal_event(int test, const std::wstring& name, const char* stage) {
    Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
    if (!event.value) {
        const DWORD error = GetLastError();
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    if (SetEvent(event.value)) return true;
    const DWORD error = GetLastError();
    fail(test, stage, error, HRESULT_FROM_WIN32(error));
    return false;
}

bool wait_event(int test, const std::wstring& name, const char* stage) {
    Handle event(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
    if (!event.value) {
        const DWORD error = GetLastError();
        fail(test, stage, error, HRESULT_FROM_WIN32(error));
        return false;
    }
    const DWORD wait = WaitForSingleObject(event.value, kWait);
    if (wait == WAIT_OBJECT_0) return true;
    const DWORD error = wait == WAIT_FAILED ? GetLastError() : wait;
    fail(test, stage, error, HRESULT_FROM_WIN32(error));
    return false;
}

bool parent(int test, DWORD expected, Handle& result) {
    HANDLE raw = nullptr;
    const HRESULT hr = liukai::installer::OpenInstallerGuardParent(&raw);
    result.reset(raw);
    if (hr != S_OK || !result.value) {
        if (hr == S_OK) fail(test, "OpenInstallerGuardParent returned null",
            ERROR_BAD_ARGUMENTS, E_HANDLE);
        else fail(test, "OpenInstallerGuardParent", win32(hr), hr);
        return false;
    }
    const DWORD actual = GetProcessId(result.value);
    const DWORD process_error = actual ? ERROR_SUCCESS : GetLastError();
    if (actual != expected) {
        fail(test, "OpenInstallerGuardParent PID",
            process_error ? process_error : ERROR_INVALID_PARAMETER, E_HANDLE);
        return false;
    }
    const DWORD wait = WaitForSingleObject(result.value, 0);
    if (wait == WAIT_TIMEOUT) return true;
    const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_NOT_FOUND;
    fail(test, "OpenInstallerGuardParent lifetime", error,
        HRESULT_FROM_WIN32(error));
    return false;
}

int test_id(int argc, wchar_t** argv) {
    return argc > 2 ? static_cast<int>(std::wcstol(argv[2], nullptr, 10)) : 0;
}

int run_ba(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 7) return fail(test, "BA arguments", ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Handle parent_handle;
    if (!parent(test, static_cast<DWORD>(std::wcstoul(argv[4], nullptr, 10)),
            parent_handle)) return 1;
    HANDLE raw = nullptr;
    const HRESULT hr = liukai::installer::AcquireInstallerSessionGuard(
        argv[3], parent_handle.value, &raw);
    Handle local(raw);
    if (hr != S_OK || !local.value)
        return fail(test, "AcquireInstallerSessionGuard from BA",
            hr == S_OK ? ERROR_BAD_ARGUMENTS : win32(hr), hr == S_OK ? E_HANDLE : hr);
    if (std::wcscmp(argv[5], L"-") &&
        !signal_event(test, argv[5], "BA ready")) return 1;
    if (std::wcscmp(argv[6], L"-") &&
        !wait_event(test, argv[6], "BA hold")) return 1;
    return 0;
}

int run_contender(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 6) return fail(test, "contender arguments",
        ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Handle parent_handle;
    if (!parent(test, static_cast<DWORD>(std::wcstoul(argv[4], nullptr, 10)),
            parent_handle)) return 1;
    HANDLE raw = nullptr;
    const HRESULT hr = liukai::installer::AcquireInstallerSessionGuard(
        argv[3], parent_handle.value, &raw);
    Handle local(raw);
    const bool expect_busy = std::wcscmp(argv[5], L"busy") == 0;
    if (expect_busy ? hr == kBusy : hr == S_OK && local.value) return 0;
    return fail(test, expect_busy ? "expected 1618" : "expected success",
        hr == S_OK ? ERROR_BAD_ARGUMENTS : win32(hr), hr == S_OK ? E_HANDLE : hr);
}

int run_race_ba(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 10) return fail(test, "race BA arguments",
        ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Handle parent_handle;
    if (!parent(test, static_cast<DWORD>(std::wcstoul(argv[4], nullptr, 10)),
            parent_handle) ||
        !signal_event(test, argv[5], "race BA ready") ||
        !wait_event(test, argv[6], "race BA start")) return 1;
    HANDLE raw = nullptr;
    const HRESULT hr = liukai::installer::AcquireInstallerSessionGuard(
        argv[3], parent_handle.value, &raw);
    Handle local(raw);
    if (hr == S_OK && !local.value)
        return fail(test, "race winner returned null", ERROR_BAD_ARGUMENTS, E_HANDLE);
    int result = 1;
    if (hr == S_OK) {
        if (!signal_event(test, argv[7], "race winner")) return 1;
        result = 0;
    } else if (hr == kBusy) {
        if (!signal_event(test, argv[8], "race loser")) return 1;
        result = 2;
    } else {
        fail(test, "race acquisition", win32(hr), hr);
    }
    local.reset();
    parent_handle.reset();
    return signal_event(test, argv[9], "race done") ? result : 1;
}

int run_keeper_hold(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 6) return fail(test, "keeper arguments",
        ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Child ba;
    if (!spawn(test, nullptr, {L"--ba", argv[2], argv[3],
            std::to_wstring(GetCurrentProcessId()), L"-", L"-"},
            ba, "spawn held BA") || !child_ok(test, ba, "held BA exit")) return 1;
    return signal_event(test, argv[4], "keeper held") &&
        wait_event(test, argv[5], "keeper stop") ? 0 : 1;
}

int run_keeper_race(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 10) return fail(test, "race keeper arguments",
        ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Child ba;
    if (!spawn(test, nullptr, {L"--race-ba", argv[2], argv[3],
            std::to_wstring(GetCurrentProcessId()), argv[4], argv[5],
            argv[6], argv[7], argv[8]}, ba, "spawn race BA")) return 1;
    DWORD exit = 0;
    if (!wait_child(test, ba, "race BA exit", &exit) ||
        exit != 0 && exit != 2)
        return exit == 0 || exit == 2 ? 1 :
            fail(test, "race BA unexpected exit", ERROR_PROCESS_ABORTED, E_FAIL);
    return wait_event(test, argv[9], "race keeper stop") ? 0 : 1;
}

int run_parent_check(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 4) return fail(test, "parent-check arguments",
        ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Handle parent_handle;
    if (!parent(test, static_cast<DWORD>(std::wcstoul(argv[3], nullptr, 10)),
            parent_handle)) return 1;
    parent_handle.reset();
    return 0;
}

int run_parent_keeper(int argc, wchar_t** argv) {
    const int test = test_id(argc, argv);
    if (argc != 3) return fail(test, "parent keeper arguments",
        ERROR_BAD_ARGUMENTS, E_INVALIDARG);
    Child child;
    return spawn(test, nullptr, {L"--parent-check", argv[2],
            std::to_wstring(GetCurrentProcessId())}, child,
            "spawn parent checker") && child_ok(test, child, "keeper parent check")
        ? 0 : 1;
}

std::wstring base_name() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return L"Global\\LiuKai-Guard-Test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(counter.QuadPart) + L"-";
}

std::wstring tag(int test) {
    wchar_t text[8]{};
    std::swprintf(text, std::size(text), L"%03d", test);
    return text;
}

std::wstring object(const std::wstring& base, int test) { return base + tag(test); }
std::wstring event(const std::wstring& guard, const std::wstring& suffix) {
    return guard + L"-" + suffix;
}

bool case1(Job& job, const std::wstring& base) {
    const int test = 1;
    const auto guard = object(base, test), ready = event(guard, L"ready");
    Handle ready_handle;
    Child ba, contender;
    return create_event(test, ready, ready_handle, "case1 ready") &&
        spawn(test, job.value, {L"--ba", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), ready, L"-"},
            ba, "case1 BA") &&
        wait_event(test, ready, "case1 BA ready") &&
        child_ok(test, ba, "case1 BA exit") &&
        spawn(test, job.value, {L"--contender", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), L"busy"},
            contender, "case1 contender") &&
        child_ok(test, contender, "case1 1618");
}

bool case2(Job& job, const std::wstring& base) {
    const int test = 2;
    const auto guard = object(base, test);
    const auto ready = event(guard, L"ready"), hold = event(guard, L"hold");
    Handle ready_handle, hold_handle;
    Child ba, contender;
    if (!create_event(test, ready, ready_handle, "case2 ready") ||
        !create_event(test, hold, hold_handle, "case2 hold") ||
        !spawn(test, job.value, {L"--ba", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), ready, hold},
            ba, "case2 BA") ||
        !wait_event(test, ready, "case2 BA ready")) return false;
    if (WaitForSingleObject(ba.process, 0) != WAIT_TIMEOUT) {
        fail(test, "synthetic BA exited before termination",
            ERROR_INVALID_STATE, E_UNEXPECTED);
        return false;
    }
    if (!TerminateProcess(ba.process, 0xfeedfaceUL)) {
        const DWORD error = GetLastError();
        fail(test, "terminate exact synthetic BA", error,
            HRESULT_FROM_WIN32(error));
        return false;
    }
    return wait_child(test, ba, "case2 BA exit") &&
        spawn(test, job.value, {L"--contender", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), L"busy"},
            contender, "case2 contender") &&
        child_ok(test, contender, "case2 1618");
}

bool case3(Job& job, const std::wstring& base) {
    const int test = 3;
    const auto guard = object(base, test);
    const auto held = event(guard, L"held"), stop = event(guard, L"stop");
    Handle held_handle, stop_handle;
    Child keeper, busy, fresh;
    if (!create_event(test, held, held_handle, "case3 held") ||
        !create_event(test, stop, stop_handle, "case3 stop") ||
        !spawn(test, job.value, {L"--keeper-hold", tag(test), guard, held, stop},
            keeper, "case3 keeper") ||
        !wait_event(test, held, "case3 keeper held")) return false;
    if (!spawn(test, job.value, {L"--contender", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), L"busy"},
            busy, "case3 busy") || !child_ok(test, busy, "case3 1618")) return false;
    if (!SetEvent(stop_handle.value)) {
        const DWORD error = GetLastError();
        fail(test, "case3 stop keeper", error, HRESULT_FROM_WIN32(error));
        return false;
    }
    return child_ok(test, keeper, "case3 keeper exit") &&
        spawn(test, job.value, {L"--contender", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), L"success"},
            fresh, "case3 fresh") &&
        child_ok(test, fresh, "case3 no stale lock");
}

bool case4(Job& job, const std::wstring& base) {
    const int test = 4;
    const auto guard = object(base, test);
    const auto start = event(guard, L"start"), stop = event(guard, L"stop");
    Handle start_handle, stop_handle, ready[2], won[2], lost[2], done[2];
    std::wstring ready_name[2], won_name[2], lost_name[2], done_name[2];
    if (!create_event(test, start, start_handle, "case4 start") ||
        !create_event(test, stop, stop_handle, "case4 stop")) return false;
    for (int i = 0; i != 2; ++i) {
        const auto n = std::to_wstring(i);
        ready_name[i] = event(guard, L"ready-" + n);
        won_name[i] = event(guard, L"won-" + n);
        lost_name[i] = event(guard, L"lost-" + n);
        done_name[i] = event(guard, L"done-" + n);
        if (!create_event(test, ready_name[i], ready[i], "case4 ready") ||
            !create_event(test, won_name[i], won[i], "case4 won") ||
            !create_event(test, lost_name[i], lost[i], "case4 lost") ||
            !create_event(test, done_name[i], done[i], "case4 done")) return false;
    }
    Child keeper[2];
    for (int i = 0; i != 2; ++i)
        if (!spawn(test, job.value, {L"--keeper-race", tag(test), guard,
                ready_name[i], start, won_name[i], lost_name[i], done_name[i], stop},
                keeper[i], "case4 keeper")) return false;
    if (!wait_event(test, ready_name[0], "case4 ready zero") ||
        !wait_event(test, ready_name[1], "case4 ready one")) return false;
    if (!SetEvent(start_handle.value)) {
        const DWORD error = GetLastError();
        fail(test, "case4 release barrier", error, HRESULT_FROM_WIN32(error));
        return false;
    }
    if (!wait_event(test, done_name[0], "case4 done zero") ||
        !wait_event(test, done_name[1], "case4 done one")) return false;
    int winners = 0, losers = 0;
    for (int i = 0; i != 2; ++i) {
        const DWORD w = WaitForSingleObject(won[i].value, 0);
        const DWORD l = WaitForSingleObject(lost[i].value, 0);
        if (w == WAIT_FAILED || l == WAIT_FAILED || (w == WAIT_OBJECT_0) == (l == WAIT_OBJECT_0)) {
            fail(test, "case4 result shape", ERROR_INVALID_STATE, E_UNEXPECTED);
            return false;
        }
        winners += w == WAIT_OBJECT_0;
        losers += l == WAIT_OBJECT_0;
        if (WaitForSingleObject(keeper[i].process, 0) != WAIT_TIMEOUT) {
            fail(test, "case4 keeper exited", ERROR_INVALID_STATE, E_UNEXPECTED);
            return false;
        }
    }
    if (winners != 1 || losers != 1) {
        fail(test, "case4 one winner", ERROR_INVALID_STATE, E_UNEXPECTED);
        return false;
    }
    Child contender;
    if (!spawn(test, job.value, {L"--contender", tag(test), guard,
            std::to_wstring(GetCurrentProcessId()), L"busy"},
            contender, "case4 retained contender") ||
        !child_ok(test, contender, "case4 keeper retained 1618")) return false;
    if (!SetEvent(stop_handle.value)) {
        const DWORD error = GetLastError();
        fail(test, "case4 stop keepers", error, HRESULT_FROM_WIN32(error));
        return false;
    }
    return child_ok(test, keeper[0], "case4 keeper zero") &&
        child_ok(test, keeper[1], "case4 keeper one");
}

bool case5(const std::wstring& base) {
    const int test = 5;
    const auto guard = object(base, test);
    // A real, live process handle without PROCESS_DUP_HANDLE reaches the
    // duplication failure path; a same-name/type error would not prove it.
    Handle wrong_keeper(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, GetCurrentProcessId()));
    if (!wrong_keeper.value) {
        const DWORD error = GetLastError();
        fail(test, "open keeper without duplicate permission", error,
            HRESULT_FROM_WIN32(error));
        return false;
    }
    HANDLE raw = nullptr;
    const HRESULT hr = liukai::installer::AcquireInstallerSessionGuard(
        guard.c_str(), wrong_keeper.value, &raw);
    Handle returned(raw);
    if (hr != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        fail(test, "keeper without duplicate permission", win32(hr), hr);
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    Handle probe(OpenMutexW(SYNCHRONIZE, FALSE, guard.c_str()));
    const DWORD error = GetLastError();
    if (probe.value) {
        fail(test, "duplicate failure leaked mutex", ERROR_ALREADY_EXISTS, E_FAIL);
        return false;
    }
    if (error != ERROR_FILE_NOT_FOUND) {
        fail(test, "OpenMutexW after duplicate failure", error,
            HRESULT_FROM_WIN32(error));
        return false;
    }
    return true;
}

bool case6(const std::wstring& base) {
    const int test = 6;
    const auto guard = object(base, test);
    SetLastError(ERROR_SUCCESS);
    Handle existing(CreateEventW(nullptr, TRUE, FALSE, guard.c_str()));
    const DWORD create_error = GetLastError();
    if (!existing.value || create_error == ERROR_ALREADY_EXISTS) {
        fail(test, "create same-name event",
            existing.value ? ERROR_ALREADY_EXISTS : create_error, E_FAIL);
        return false;
    }
    HANDLE raw = nullptr;
    HRESULT hr = liukai::installer::AcquireInstallerSessionGuard(
        guard.c_str(), GetCurrentProcess(), &raw);
    Handle returned(raw);
    if (!FAILED(hr) || hr == kBusy) {
        fail(test, "event conflict became busy", win32(hr), hr);
        return false;
    }
    Handle probe(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, guard.c_str()));
    if (!probe.value || !SetEvent(existing.value)) {
        const DWORD error = GetLastError();
        fail(test, "existing event unusable", error, HRESULT_FROM_WIN32(error));
        return false;
    }
    const DWORD event_wait = WaitForSingleObject(probe.value, 0);
    if (event_wait != WAIT_OBJECT_0) {
        const DWORD error = event_wait == WAIT_FAILED ? GetLastError() : event_wait;
        fail(test, "existing event unusable", error, HRESULT_FROM_WIN32(error));
        return false;
    }
    returned.reset();
    probe.reset();
    existing.reset();
    raw = nullptr;
    hr = liukai::installer::AcquireInstallerSessionGuard(
        guard.c_str(), GetCurrentProcess(), &raw);
    Handle acquired(raw);
    if (hr != S_OK || !acquired.value) {
        fail(test, "name did not recover after event close",
            hr == S_OK ? ERROR_BAD_ARGUMENTS : win32(hr),
            hr == S_OK ? E_HANDLE : hr);
        return false;
    }
    return true;
}

bool case7(Job& job) {
    const int test = 7;
    Child direct, keeper;
    return spawn(test, job.value, {L"--parent-check", tag(test),
            std::to_wstring(GetCurrentProcessId())},
            direct, "controller parent check") &&
        child_ok(test, direct, "controller parent PID") &&
        spawn(test, job.value, {L"--parent-keeper", tag(test)},
            keeper, "keeper parent check") &&
        child_ok(test, keeper, "keeper parent PID");
}

int controller() {
    if (std::wcscmp(liukai::installer::kInstallerSessionGuardName,
            L"Global\\LiuKai-Burn-94C1F608-8C0F-424C-A572-E0F4BCB10327-v1"))
        return fail(0, "product name", ERROR_INVALID_NAME, E_FAIL);
    Job job;
    if (!job.create()) {
        const DWORD error = GetLastError();
        return fail(0, "cleanup job", error, HRESULT_FROM_WIN32(error));
    }
    const auto base = base_name();
    if (!case1(job, base) || !case2(job, base) || !case3(job, base) ||
        !case4(job, base) || !case5(base) || !case6(base) || !case7(job))
        return 1;
    std::puts("wix_installer_guard_tests=passed; cases=7");
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 1 || std::wcscmp(argv[1], L"--controller") == 0) return controller();
    if (std::wcscmp(argv[1], L"--ba") == 0) return run_ba(argc, argv);
    if (std::wcscmp(argv[1], L"--contender") == 0) return run_contender(argc, argv);
    if (std::wcscmp(argv[1], L"--race-ba") == 0) return run_race_ba(argc, argv);
    if (std::wcscmp(argv[1], L"--keeper-hold") == 0) return run_keeper_hold(argc, argv);
    if (std::wcscmp(argv[1], L"--keeper-race") == 0) return run_keeper_race(argc, argv);
    if (std::wcscmp(argv[1], L"--parent-check") == 0) return run_parent_check(argc, argv);
    if (std::wcscmp(argv[1], L"--parent-keeper") == 0) return run_parent_keeper(argc, argv);
    return fail(0, "unknown role", ERROR_BAD_ARGUMENTS, E_INVALIDARG);
}
