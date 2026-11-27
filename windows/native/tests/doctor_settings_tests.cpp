#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include "../doctor_host.hpp"
#include "../tools/settings_window.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#define CHECK(value) \
    do { \
        if (!(value)) { \
            std::cerr << #value << " line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

using namespace boshiamy;
using namespace boshiamy::doctor;
namespace fs = std::filesystem;

namespace {

constexpr DWORD kPumpTimeoutMs = 3'000;
constexpr DWORD kBlockedRequestTimeoutMs = 4'000;
constexpr ULONGLONG kCloseMustNotWaitMs = 1'000;
constexpr UINT kUiProbeMessage = WM_APP + 0x61;
constexpr std::int64_t kMarkActionTimeMs = 1'766'041'234'567LL;
constexpr std::int64_t kEpoch = 0x12345678LL;
constexpr std::uint64_t kSnapshotHandle = 0x1020304050607080ULL;

class OwnedTempDirectory {
public:
    OwnedTempDirectory() {
        std::error_code error;
        const auto base = fs::weakly_canonical(fs::temp_directory_path(error), error);
        if (error || !base.is_absolute())
            throw std::runtime_error("Cannot resolve the temp directory");
        base_ = base;
        for (unsigned attempt = 0; attempt != 64; ++attempt) {
            const auto candidate =
                base / (L"liu-kai-doctor-settings-tests-" +
                    std::to_wstring(
                        static_cast<unsigned long>(GetCurrentProcessId())) +
                    L"-" + std::to_wstring(GetTickCount64()) +
                    L"-" + std::to_wstring(attempt));
            if (fs::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            error.clear();
        }
        throw std::runtime_error("Cannot create a test temp directory");
    }

    OwnedTempDirectory(const OwnedTempDirectory &) = delete;
    OwnedTempDirectory &operator=(const OwnedTempDirectory &) = delete;

    ~OwnedTempDirectory() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(path_, error);
        if (error || !resolved.is_absolute() || resolved.parent_path() != base_ ||
            resolved.filename().wstring().rfind(L"liu-kai-doctor-settings-tests-", 0) != 0) return;
        fs::remove_all(resolved, error);
    }

    const fs::path &path() const noexcept { return path_; }

private:
    fs::path base_;
    fs::path path_;
};

struct ReplyStep {
    using Responder =
        std::function<DoctorHostClientResult(const DoctorHostRequest &)>;

    ReplyStep(
        DoctorHostCommand command_value,
        Responder responder_value,
        bool block_value)
        : command(command_value),
          respond(std::move(responder_value)),
          block(block_value),
          entered(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          release(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          completed(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!entered || !release || !completed)
            throw std::runtime_error("Cannot create a controlled request event");
    }

    ~ReplyStep() {
        if (entered) CloseHandle(entered);
        if (release) CloseHandle(release);
        if (completed) CloseHandle(completed);
    }

    DoctorHostCommand command;
    Responder respond;
    bool block;
    HANDLE entered;
    HANDLE release;
    HANDLE completed;
    std::atomic<DWORD> request_thread{0};
    std::atomic<bool> timed_out{false};
};

class FakeDoctorState {
public:
    ~FakeDoctorState() {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto &step : steps_) {
            if (step->release) SetEvent(step->release);
        }
    }

    std::shared_ptr<ReplyStep> queue(
        DoctorHostCommand command,
        DoctorHostClientResult result,
        bool block = false) {
        return queue(
            command,
            [result](const DoctorHostRequest &) { return result; },
            block);
    }

    std::shared_ptr<ReplyStep> queue(
        DoctorHostCommand command,
        ReplyStep::Responder responder,
        bool block = false) {
        auto step = std::make_shared<ReplyStep>(
            command, std::move(responder), block);
        std::lock_guard<std::mutex> guard(mutex_);
        steps_.push_back(step);
        return step;
    }

    void queue_confirmation(
        DoctorSettingsConfirmation expected,
        bool accepted) {
        std::lock_guard<std::mutex> guard(mutex_);
        confirmations_.push_back({expected, accepted});
    }

    void queue_picker(std::optional<fs::path> selected) {
        std::lock_guard<std::mutex> guard(mutex_);
        pickers_.push_back(std::move(selected));
    }

    void expect_owner(HWND owner) {
        std::lock_guard<std::mutex> guard(mutex_);
        expected_owner_ = owner;
    }

    DoctorHostClientResult request(const DoctorHostRequest &request) {
        std::shared_ptr<ReplyStep> step;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const std::size_t index = requests_.size();
            requests_.push_back(request);
            if (index >= steps_.size()) {
                unexpected_ = true;
            } else {
                step = steps_[index];
                if (request.command != step->command) unexpected_ = true;
            }
            if (step) {
                if (active_requests_ != 0) unexpected_ = true;
                ++active_requests_;
            }
        }
        if (!step) {
            DoctorHostClientResult rejected;
            rejected.failure = DoctorHostClientFailure::InvalidRequest;
            return rejected;
        }

        step->request_thread.store(GetCurrentThreadId());
        SetEvent(step->entered);
        if (step->block &&
            WaitForSingleObject(
                step->release, kBlockedRequestTimeoutMs) != WAIT_OBJECT_0) {
            step->timed_out.store(true);
        }
        const DoctorHostClientResult result = step->respond(request);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            --active_requests_;
        }
        SetEvent(step->completed);
        return result;
    }

    bool confirm(HWND owner, DoctorSettingsConfirmation kind) {
        std::lock_guard<std::mutex> guard(mutex_);
        callback_threads_.push_back(GetCurrentThreadId());
        confirmation_kinds_.push_back(kind);
        if (!owner || owner != expected_owner_ || confirmations_.empty()) {
            unexpected_ = true;
            return false;
        }
        const auto reply = confirmations_[0];
        confirmations_.erase(confirmations_.begin());
        if (reply.first != kind) unexpected_ = true;
        return reply.second;
    }

    std::optional<fs::path> pick_save_path(HWND owner) {
        std::lock_guard<std::mutex> guard(mutex_);
        callback_threads_.push_back(GetCurrentThreadId());
        ++picker_calls_;
        if (!owner || owner != expected_owner_ || pickers_.empty()) {
            unexpected_ = true;
            return std::nullopt;
        }
        auto selected = std::move(pickers_.front());
        pickers_.erase(pickers_.begin());
        return selected;
    }

    std::vector<DoctorHostRequest> requests() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_;
    }

    std::vector<DoctorSettingsConfirmation> confirmation_kinds() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return confirmation_kinds_;
    }

    std::size_t request_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_.size();
    }

    std::size_t picker_calls() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return picker_calls_;
    }

    bool callbacks_ran_on(DWORD thread_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return std::all_of(
            callback_threads_.begin(),
            callback_threads_.end(),
            [thread_id](DWORD value) { return value == thread_id; });
    }

    bool valid() const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (unexpected_ || active_requests_ != 0 ||
            requests_.size() != steps_.size() ||
            !confirmations_.empty() || !pickers_.empty()) {
            return false;
        }
        for (const auto &step : steps_)
            if (step->timed_out.load()) return false;
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ReplyStep>> steps_;
    std::vector<DoctorHostRequest> requests_;
    std::vector<std::pair<DoctorSettingsConfirmation, bool>>
        confirmations_;
    std::vector<std::optional<fs::path>> pickers_;
    std::vector<DoctorSettingsConfirmation> confirmation_kinds_;
    std::vector<DWORD> callback_threads_;
    std::size_t picker_calls_ = 0;
    std::size_t active_requests_ = 0;
    HWND expected_owner_ = nullptr;
    bool unexpected_ = false;
};

class FakeDoctor {
public:
    FakeDoctor() : state_(std::make_shared<FakeDoctorState>()) {}

    DoctorSettingsUiHooks hooks() const {
        const auto state = state_;
        DoctorSettingsUiHooks hooks;
        hooks.request =
            [state](const DoctorHostRequest &request) {
                return state->request(request);
            };
        hooks.confirm =
            [state](HWND owner, DoctorSettingsConfirmation kind) {
                return state->confirm(owner, kind);
            };
        hooks.pick_save_path =
            [state](HWND owner) {
                return state->pick_save_path(owner);
            };
        return hooks;
    }

    std::shared_ptr<ReplyStep> queue(
        DoctorHostCommand command,
        DoctorHostClientResult result,
        bool block = false) {
        return state_->queue(command, std::move(result), block);
    }

    std::shared_ptr<ReplyStep> queue(
        DoctorHostCommand command,
        ReplyStep::Responder responder,
        bool block = false) {
        return state_->queue(command, std::move(responder), block);
    }

    void queue_confirmation(
        DoctorSettingsConfirmation expected,
        bool accepted) {
        state_->queue_confirmation(expected, accepted);
    }

    void queue_picker(std::optional<fs::path> selected) {
        state_->queue_picker(std::move(selected));
    }

    void expect_owner(HWND owner) {
        state_->expect_owner(owner);
    }

    std::vector<DoctorHostRequest> requests() const {
        return state_->requests();
    }

    std::vector<DoctorSettingsConfirmation> confirmation_kinds() const {
        return state_->confirmation_kinds();
    }

    std::size_t request_count() const {
        return state_->request_count();
    }

    std::size_t picker_calls() const {
        return state_->picker_calls();
    }

    bool callbacks_ran_on(DWORD thread_id) const {
        return state_->callbacks_ran_on(thread_id);
    }

    bool valid() const {
        return state_->valid();
    }

private:
    std::shared_ptr<FakeDoctorState> state_;
};

template<class Predicate, class Observer>
static bool pump_messages_until(
    HWND dialog,
    Predicate predicate,
    Observer observer,
    DWORD timeout_ms = kPumpTimeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (predicate()) return true;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            observer(message);
            if (message.message == WM_QUIT) return predicate();
            if (!IsWindow(dialog) ||
                !IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (predicate()) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            0,
            nullptr,
            remaining,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED || wait == WAIT_TIMEOUT)
            return predicate();
    }
}

template<class Predicate>
static bool pump_messages_until(
    HWND dialog,
    Predicate predicate,
    DWORD timeout_ms = kPumpTimeoutMs) {
    return pump_messages_until(
        dialog, predicate, [](const MSG &) {}, timeout_ms);
}

static bool pump_probe(HWND dialog) {
    bool observed = false;
    if (!PostMessageW(dialog, kUiProbeMessage, 0, 0)) return false;
    return pump_messages_until(
        dialog,
        [&] { return observed; },
        [&](const MSG &message) {
            if (message.hwnd == dialog &&
                message.message == kUiProbeMessage) {
                observed = true;
            }
        });
}

static bool click(HWND control) {
    return control && PostMessageW(control, BM_CLICK, 0, 0) != FALSE;
}

static std::wstring control_text(HWND control) {
    if (!control) return {};
    std::wstring text(1024, L'\0');
    const int copied =
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(std::max(0, copied));
    return text;
}

static bool contains(const std::wstring &text, const std::wstring &part) {
    return text.find(part) != std::wstring::npos;
}

static bool local_epoch_ms(
    const SYSTEMTIME &local,
    std::int64_t *result) {
    if (!result) return false;
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc))
        return false;
    FILETIME file_time{};
    if (!SystemTimeToFileTime(&utc, &file_time)) return false;
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (value.QuadPart < kUnixEpochTicks) return false;
    *result = static_cast<std::int64_t>(
        (value.QuadPart - kUnixEpochTicks) / 10000ULL);
    return true;
}

static std::int64_t system_utc_ms() {
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks =
        116444736000000000ULL;
    return value.QuadPart >= kUnixEpochTicks
        ? static_cast<std::int64_t>(
            (value.QuadPart - kUnixEpochTicks) / 10000ULL)
        : 0;
}

static bool wait_for_clock_after(
    std::int64_t milliseconds,
    DWORD timeout_ms = 1'000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    do {
        if (system_utc_ms() > milliseconds) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}

static bool same_mark(
    const DoctorHostMark &left,
    const DoctorHostMark &right) {
    return left.epoch == right.epoch &&
        left.action_time_ms == right.action_time_ms &&
        left.selected_issue_time_ms ==
            right.selected_issue_time_ms &&
        left.before_ms == right.before_ms &&
        left.after_ms == right.after_ms;
}

static DoctorHostClientResult successful_mark(
    const DoctorHostRequest &request,
    std::int64_t epoch = kEpoch,
    std::int64_t action_time_ms = kMarkActionTimeMs) {
    DoctorHostClientResult result;
    result.reply.mark = request.mark;
    result.reply.mark.epoch = epoch;
    result.reply.mark.action_time_ms = action_time_ms;
    return result;
}

static bool choose_specific_issue(
    const SettingsWindow &window,
    const SYSTEMTIME &selected,
    const wchar_t *before,
    const wchar_t *after) {
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_RECENT),
        BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_SPECIFIC),
        BM_SETCHECK, BST_CHECKED, 0);
    return SendMessageW(
            window.control(SettingsWindow::ID_DOCTOR_ISSUE_DATE),
            DTM_SETSYSTEMTIME,
            GDT_VALID,
            reinterpret_cast<LPARAM>(
                const_cast<SYSTEMTIME *>(&selected))) != FALSE &&
        SendMessageW(
            window.control(SettingsWindow::ID_DOCTOR_ISSUE_TIME),
            DTM_SETSYSTEMTIME,
            GDT_VALID,
            reinterpret_cast<LPARAM>(
                const_cast<SYSTEMTIME *>(&selected))) != FALSE &&
        SetWindowTextW(
            window.control(SettingsWindow::ID_DOCTOR_BEFORE_SECONDS),
            before) != FALSE &&
        SetWindowTextW(
            window.control(SettingsWindow::ID_DOCTOR_AFTER_SECONDS),
            after) != FALSE;
}

static DoctorStoreState doctor_state(
    bool enabled,
    bool collecting,
    std::int64_t epoch = kEpoch) {
    DoctorStoreState state;
    state.enabled = enabled;
    state.collecting = collecting;
    state.epoch = epoch;
    return state;
}

static DoctorHostClientResult status_reply(
    const DoctorStoreState &state) {
    DoctorHostClientResult result;
    result.reply.status = state;
    return result;
}

static SettingsWindowOptions doctor_options(FakeDoctor &doctor) {
    SettingsWindowOptions options;
    options.initial_page = SettingsWindowPage::Doctor;
    options.doctor = doctor.hooks();
    // Native confirmation/save dialogs run a nested message loop. A queued
    // status timer must not take the single request slot during that loop.
    const auto confirm = options.doctor.confirm;
    options.doctor.confirm = [confirm](HWND owner, DoctorSettingsConfirmation kind) {
        SendMessageW(owner, WM_TIMER, 3100, 0);
        return confirm(owner, kind);
    };
    const auto picker = options.doctor.pick_save_path;
    options.doctor.pick_save_path = [picker](HWND owner) {
        SendMessageW(owner, WM_TIMER, 3100, 0);
        return picker(owner);
    };
    return options;
}

static bool doctor_ready(
    const SettingsWindow &window,
    const FakeDoctor &doctor) {
    // These tests drive exact timer deliveries, including nested modal loops.
    // Do not let elapsed wall time add uncontrolled fake-host requests.
    KillTimer(window.hwnd(), SettingsWindow::ID_DOCTOR_STATUS_TIMER);
    return doctor.request_count() >= 1 &&
        IsWindowEnabled(window.control(SettingsWindow::ID_DOCTOR_TOGGLE));
}

static void queue_export_through_preview(
    FakeDoctor &doctor,
    std::atomic<std::int64_t> *selected_issue_time = nullptr) {
    doctor.queue(
        DoctorHostCommand::Mark,
        [selected_issue_time](const DoctorHostRequest &request) {
            if (selected_issue_time) {
                selected_issue_time->store(
                    request.mark.selected_issue_time_ms);
            }
            DoctorHostClientResult result;
            result.reply.mark = request.mark;
            result.reply.mark.epoch = kEpoch;
            result.reply.mark.action_time_ms = kMarkActionTimeMs;
            return result;
        });

    DoctorHostClientResult prepared;
    prepared.reply.snapshot = {
        kEpoch, kSnapshotHandle};
    doctor.queue(DoctorHostCommand::Prepare, prepared);

    DoctorHostClientResult previewed;
    previewed.reply.preview = DoctorExportPreview{
        kEpoch, 12, 3, 17'000, 29'000};
    doctor.queue(DoctorHostCommand::Preview, previewed);
}

static void queue_cancelled_export(
    FakeDoctor &doctor,
    const DoctorStoreState &state,
    std::uint64_t snapshot_handle) {
    DoctorHostClientResult prepared;
    prepared.reply.snapshot = {
        state.epoch, snapshot_handle};
    doctor.queue(
        DoctorHostCommand::Prepare, prepared);
    DoctorHostClientResult previewed;
    previewed.reply.preview = DoctorExportPreview{
        state.epoch, 1, 0, 0, 0};
    doctor.queue(
        DoctorHostCommand::Preview, previewed);
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::SaveExport, false);
    doctor.queue(
        DoctorHostCommand::Cancel,
        status_reply(state));
}

static int test_toggle_consent_async_busy_and_status() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    auto state = doctor_state(false, false);
    state.sequence_gap = 17;
    state.ignored_lines = 5;
    doctor.queue(DoctorHostCommand::Status, status_reply(state));
    doctor.queue_confirmation(DoctorSettingsConfirmation::Enable, false);

    bool closed = false;
    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        [&] { closed = true; },
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(hwnd && window.control(SettingsWindow::ID_DOCTOR_STATUS));
    CHECK(TabCtrl_GetCurSel(window.control(SettingsWindow::ID_TAB)) ==
        static_cast<int>(SettingsWindowPage::Doctor));
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));

    const HWND toggle = window.control(SettingsWindow::ID_DOCTOR_TOGGLE);
    CHECK(SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)),
        L"17"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)),
        L"5"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)),
        L"關閉"));

    CHECK(click(toggle));
    CHECK(pump_probe(hwnd));
    CHECK(doctor.request_count() == 1);
    CHECK(SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED);

    doctor.queue_confirmation(DoctorSettingsConfirmation::Enable, true);
    auto blocked_enable = doctor.queue(
        DoctorHostCommand::Enable,
        status_reply(doctor_state(true, true)),
        true);
    CHECK(click(toggle));
    CHECK(pump_messages_until(hwnd, [&] {
        return WaitForSingleObject(
            blocked_enable->entered, 0) == WAIT_OBJECT_0;
    }));
    CHECK(blocked_enable->request_thread.load() != GetCurrentThreadId());
    CHECK(!IsWindowEnabled(toggle));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK)));

    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_probe(hwnd));
    CHECK(doctor.request_count() == 2);
    CHECK(doctor.confirmation_kinds().size() == 2);
    SetEvent(blocked_enable->release);
    CHECK(WaitForSingleObject(
        blocked_enable->completed, kPumpTimeoutMs) == WAIT_OBJECT_0);
    CHECK(pump_messages_until(hwnd, [&] {
        return SendMessageW(toggle, BM_GETCHECK, 0, 0) ==
                BST_CHECKED &&
            IsWindowEnabled(toggle);
    }));

    doctor.queue(
        DoctorHostCommand::Disable,
        status_reply(doctor_state(false, false)));
    CHECK(click(toggle));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 3 &&
            SendMessageW(toggle, BM_GETCHECK, 0, 0) ==
                BST_UNCHECKED &&
            IsWindowEnabled(toggle);
    }));
    const auto confirmations = doctor.confirmation_kinds();
    CHECK(confirmations.size() == 2);
    CHECK(confirmations[0] == DoctorSettingsConfirmation::Enable);
    CHECK(confirmations[1] == DoctorSettingsConfirmation::Enable);
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());

    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    CHECK(closed && !IsWindow(hwnd));
    return 0;
}

static int test_status_refresh_observes_collector_failure() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(DoctorHostCommand::Status, status_reply(doctor_state(true, true)));
    auto failed = doctor_state(true, false);
    failed.failure = DoctorStoreFailure::PermissionDenied;
    const auto refresh = doctor.queue(DoctorHostCommand::Status, status_reply(failed), true);
    SettingsWindow window(GetModuleHandleW(nullptr), temp.path() / L"settings.conf",
        {}, doctor_options(doctor));
    CHECK(window.create());
    const auto hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(hwnd, [&] { return doctor_ready(window, doctor); }));
    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(pump_messages_until(hwnd, [&] {
        return WaitForSingleObject(refresh->entered, 0) == WAIT_OBJECT_0;
    }));
    CHECK(refresh->request_thread.load() != GetCurrentThreadId());
    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(doctor.request_count() == 2);
    SetEvent(refresh->release);
    CHECK(pump_messages_until(hwnd, [&] {
        return IsWindowEnabled(window.control(SettingsWindow::ID_DOCTOR_TOGGLE)) &&
            contains(control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)), L"中斷");
    }));
    DoctorHostClientResult disconnected;
    disconnected.failure = DoctorHostClientFailure::EndpointUnavailable;
    doctor.queue(DoctorHostCommand::Status, disconnected);
    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(pump_messages_until(hwnd, [&] {
        return contains(control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)), L"無法確認");
    }));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    TabCtrl_SetCurSel(window.control(SettingsWindow::ID_TAB), 0);
    NMHDR notification{window.control(SettingsWindow::ID_TAB), SettingsWindow::ID_TAB, TCN_SELCHANGE};
    SendMessageW(hwnd, WM_NOTIFY, SettingsWindow::ID_TAB, reinterpret_cast<LPARAM>(&notification));
    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(pump_probe(hwnd));
    CHECK(doctor.request_count() == 3);
    CHECK(doctor.valid());
    return 0;
}

static int test_background_status_preserves_issue_editing() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));
    const auto refresh = doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)),
        true);

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    window.show(SW_SHOW);

    const HWND recent =
        window.control(SettingsWindow::ID_DOCTOR_MARK_RECENT);
    const HWND specific =
        window.control(SettingsWindow::ID_DOCTOR_MARK_SPECIFIC);
    const HWND issue_date =
        window.control(SettingsWindow::ID_DOCTOR_ISSUE_DATE);
    const HWND issue_time =
        window.control(SettingsWindow::ID_DOCTOR_ISSUE_TIME);
    const HWND before =
        window.control(SettingsWindow::ID_DOCTOR_BEFORE_SECONDS);
    const HWND after =
        window.control(SettingsWindow::ID_DOCTOR_AFTER_SECONDS);
    const HWND editable_controls[] = {
        recent,
        specific,
        issue_date,
        issue_time,
        before,
        after,
    };
    CHECK(std::all_of(
        std::begin(editable_controls),
        std::end(editable_controls),
        [](HWND control) { return IsWindowEnabled(control); }));

    SendMessageW(
        recent, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(
        specific, BM_SETCHECK, BST_CHECKED, 0);
    SYSTEMTIME selected_date{};
    selected_date.wYear = 2026;
    selected_date.wMonth = 8;
    selected_date.wDay = 12;
    SYSTEMTIME selected_time = selected_date;
    selected_time.wHour = 14;
    selected_time.wMinute = 37;
    selected_time.wSecond = 0;
    CHECK(SendMessageW(
        issue_date,
        DTM_SETSYSTEMTIME,
        GDT_VALID,
        reinterpret_cast<LPARAM>(&selected_date)) != FALSE);
    CHECK(SendMessageW(
        issue_time,
        DTM_SETSYSTEMTIME,
        GDT_VALID,
        reinterpret_cast<LPARAM>(&selected_time)) != FALSE);
    CHECK(SetWindowTextW(before, L"137"));
    CHECK(SetWindowTextW(after, L"41"));
    SetFocus(before);
    CHECK(GetFocus() == before);

    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(pump_messages_until(hwnd, [&] {
        return WaitForSingleObject(
            refresh->entered, 0) == WAIT_OBJECT_0;
    }));
    CHECK(refresh->request_thread.load() != GetCurrentThreadId());
    CHECK(pump_probe(hwnd));
    CHECK(std::all_of(
        std::begin(editable_controls),
        std::end(editable_controls),
        [](HWND control) { return IsWindowEnabled(control); }));
    CHECK(GetFocus() == before);
    CHECK(control_text(before) == L"137");
    CHECK(control_text(after) == L"41");
    CHECK(SendMessageW(
        recent, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    CHECK(SendMessageW(
        specific, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SYSTEMTIME date_during{};
    SYSTEMTIME time_during{};
    CHECK(SendMessageW(
        issue_date,
        DTM_GETSYSTEMTIME,
        0,
        reinterpret_cast<LPARAM>(&date_during)) == GDT_VALID);
    CHECK(SendMessageW(
        issue_time,
        DTM_GETSYSTEMTIME,
        0,
        reinterpret_cast<LPARAM>(&time_during)) == GDT_VALID);
    CHECK(date_during.wYear == selected_date.wYear);
    CHECK(date_during.wMonth == selected_date.wMonth);
    CHECK(date_during.wDay == selected_date.wDay);
    CHECK(time_during.wHour == selected_time.wHour);
    CHECK(time_during.wMinute == selected_time.wMinute);
    CHECK(time_during.wSecond == selected_time.wSecond);

    SetEvent(refresh->release);
    CHECK(WaitForSingleObject(
        refresh->completed,
        kPumpTimeoutMs) == WAIT_OBJECT_0);
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 2 &&
            IsWindowEnabled(window.control(SettingsWindow::ID_DOCTOR_TOGGLE)) &&
            std::all_of(
                std::begin(editable_controls),
                std::end(editable_controls),
                [](HWND control) {
                    return IsWindowEnabled(control);
                });
    }));
    CHECK(GetFocus() == before);
    CHECK(control_text(before) == L"137");
    CHECK(control_text(after) == L"41");

    const auto mark = doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            DoctorHostClientResult result;
            result.reply.mark = request.mark;
            result.reply.mark.epoch = kEpoch;
            result.reply.mark.action_time_ms =
                kMarkActionTimeMs;
            return result;
        },
        true);
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return WaitForSingleObject(
            mark->entered, 0) == WAIT_OBJECT_0;
    }));
    CHECK(std::all_of(
        std::begin(editable_controls),
        std::end(editable_controls),
        [](HWND control) {
            return !IsWindowEnabled(control);
        }));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_TOGGLE)));
    SetEvent(mark->release);
    CHECK(WaitForSingleObject(
        mark->completed,
        kPumpTimeoutMs) == WAIT_OBJECT_0);
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 3 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_mark_recent_and_specific_local_time() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)),
        L"收集"));

    CHECK(control_text(
        window.control(SettingsWindow::ID_DOCTOR_BEFORE_SECONDS)) ==
        L"300");
    CHECK(control_text(
        window.control(SettingsWindow::ID_DOCTOR_AFTER_SECONDS)) ==
        L"30");
    CHECK(SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_RECENT),
        BM_GETCHECK, 0, 0) == BST_CHECKED);

    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            DoctorHostClientResult result;
            result.reply.mark = request.mark;
            result.reply.mark.epoch = kEpoch;
            result.reply.mark.action_time_ms =
                request.mark.selected_issue_time_ms + 123;
            return result;
        });
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 2 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));
    auto requests = doctor.requests();
    CHECK(requests.size() == 2);
    CHECK(requests[1].command == DoctorHostCommand::Mark);
    CHECK(requests[1].mark.selected_issue_time_ms > 0);
    CHECK(requests[1].mark.before_ms == 300'000);
    CHECK(requests[1].mark.after_ms == 30'000);

    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_RECENT),
        BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_SPECIFIC),
        BM_SETCHECK, BST_CHECKED, 0);
    SYSTEMTIME specific{};
    specific.wYear = 2026;
    specific.wMonth = 2;
    specific.wDay = 14;
    specific.wHour = 13;
    specific.wMinute = 7;
    specific.wSecond = 42;
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_ISSUE_DATE),
        DTM_SETSYSTEMTIME, GDT_VALID,
        reinterpret_cast<LPARAM>(&specific));
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_ISSUE_TIME),
        DTM_SETSYSTEMTIME, GDT_VALID,
        reinterpret_cast<LPARAM>(&specific));
    SetWindowTextW(
        window.control(SettingsWindow::ID_DOCTOR_BEFORE_SECONDS),
        L"90");
    SetWindowTextW(
        window.control(SettingsWindow::ID_DOCTOR_AFTER_SECONDS),
        L"12");

    std::int64_t expected_issue_time = 0;
    CHECK(local_epoch_ms(specific, &expected_issue_time));
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            DoctorHostClientResult result;
            result.reply.mark = request.mark;
            result.reply.mark.epoch = kEpoch + 1;
            result.reply.mark.action_time_ms = kMarkActionTimeMs;
            return result;
        });
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 3 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));
    requests = doctor.requests();
    CHECK(requests.size() == 3);
    CHECK(requests[2].mark.selected_issue_time_ms ==
        expected_issue_time);
    CHECK(requests[2].mark.before_ms == 90'000);
    CHECK(requests[2].mark.after_ms == 12'000);
    CHECK(!control_text(
        window.control(SettingsWindow::ID_DOCTOR_RESULT)).empty());
    CHECK(doctor.valid());
    return 0;
}

static int test_off_history_supports_mark_export_and_clear() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    auto off = doctor_state(false, false);
    off.has_retained_range = true;
    off.oldest_retained_ms = 1'700'000'000'000LL;
    off.newest_retained_ms = 1'700'000'060'000LL;
    off.raw_bytes = 4096;
    doctor.queue(DoctorHostCommand::Status, status_reply(off));
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            return successful_mark(
                request, kEpoch, kMarkActionTimeMs);
        });
    queue_cancelled_export(
        doctor, off, kSnapshotHandle);
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            return successful_mark(
                request, kEpoch, kMarkActionTimeMs + 1);
        });
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::Clear, true);
    auto cleared = off;
    cleared.epoch += 1;
    cleared.raw_bytes = 0;
    cleared.has_retained_range = false;
    cleared.oldest_retained_ms = 0;
    cleared.newest_retained_ms = 0;
    doctor.queue(
        DoctorHostCommand::Clear,
        status_reply(cleared));

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));

    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK_RECENT)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_BEFORE_SECONDS)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_AFTER_SECONDS)));

    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 5 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));
    auto requests = doctor.requests();
    CHECK(requests[1].command == DoctorHostCommand::Mark);
    auto marked = requests[1].mark;
    marked.epoch = kEpoch;
    marked.action_time_ms = kMarkActionTimeMs;
    CHECK(requests[2].command ==
        DoctorHostCommand::Prepare);
    CHECK(same_mark(requests[2].mark, marked));
    CHECK(requests[3].command ==
        DoctorHostCommand::Preview);
    CHECK(requests[4].command ==
        DoctorHostCommand::Cancel);

    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 6 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 7 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_CLEAR));
    }));
    requests = doctor.requests();
    CHECK(requests[5].command == DoctorHostCommand::Mark);
    CHECK(requests[6].command == DoctorHostCommand::Clear);
    CHECK(std::none_of(
        requests.begin(),
        requests.end(),
        [](const DoctorHostRequest &request) {
            return request.command ==
                DoctorHostCommand::Enable;
        }));
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_export_reuses_unedited_mark_and_tracks_edits() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    const auto on = doctor_state(true, true);
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(on));
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            return successful_mark(
                request, kEpoch, kMarkActionTimeMs);
        });
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(on));
    queue_cancelled_export(
        doctor, on, kSnapshotHandle);
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            return successful_mark(
                request, kEpoch, kMarkActionTimeMs + 1);
        });
    queue_cancelled_export(
        doctor, on, kSnapshotHandle + 1);
    queue_cancelled_export(
        doctor, on, kSnapshotHandle + 2);
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::Clear, true);
    auto next_epoch = on;
    next_epoch.epoch += 1;
    doctor.queue(
        DoctorHostCommand::Clear,
        status_reply(next_epoch));
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            return successful_mark(
                request,
                kEpoch + 1,
                kMarkActionTimeMs + 2);
        });
    queue_cancelled_export(
        doctor, next_epoch, kSnapshotHandle + 3);

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));

    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 2 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));
    auto first_request = doctor.requests()[1];
    CHECK(first_request.command == DoctorHostCommand::Mark);
    auto first_mark = first_request.mark;
    first_mark.epoch = kEpoch;
    first_mark.action_time_ms = kMarkActionTimeMs;
    CHECK(wait_for_clock_after(
        first_mark.selected_issue_time_ms));

    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 3 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));

    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 6 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));
    auto requests = doctor.requests();
    CHECK(requests[2].command ==
        DoctorHostCommand::Status);
    CHECK(requests[3].command ==
        DoctorHostCommand::Prepare);
    CHECK(same_mark(requests[3].mark, first_mark));
    CHECK(requests[4].command ==
        DoctorHostCommand::Preview);
    CHECK(requests[5].command ==
        DoctorHostCommand::Cancel);

    SYSTEMTIME selected{};
    selected.wYear = 2026;
    selected.wMonth = 6;
    selected.wDay = 15;
    selected.wHour = 9;
    selected.wMinute = 8;
    selected.wSecond = 7;
    CHECK(choose_specific_issue(
        window, selected, L"90", L"12"));
    std::int64_t selected_ms = 0;
    CHECK(local_epoch_ms(selected, &selected_ms));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 10 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));
    requests = doctor.requests();
    CHECK(requests[6].command == DoctorHostCommand::Mark);
    CHECK(requests[6].mark.selected_issue_time_ms ==
        selected_ms);
    CHECK(requests[6].mark.before_ms == 90'000);
    CHECK(requests[6].mark.after_ms == 12'000);
    auto edited_mark = requests[6].mark;
    edited_mark.epoch = kEpoch;
    edited_mark.action_time_ms =
        kMarkActionTimeMs + 1;
    CHECK(requests[7].command ==
        DoctorHostCommand::Prepare);
    CHECK(same_mark(requests[7].mark, edited_mark));
    CHECK(requests[8].command ==
        DoctorHostCommand::Preview);
    CHECK(requests[9].command ==
        DoctorHostCommand::Cancel);

    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 13 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));
    requests = doctor.requests();
    CHECK(requests[10].command ==
        DoctorHostCommand::Prepare);
    CHECK(same_mark(requests[10].mark, edited_mark));
    CHECK(requests[11].command ==
        DoctorHostCommand::Preview);
    CHECK(requests[12].command ==
        DoctorHostCommand::Cancel);

    CHECK(click(window.control(
        SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 14 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_CLEAR));
    }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 18 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));
    requests = doctor.requests();
    CHECK(requests[13].command ==
        DoctorHostCommand::Clear);
    CHECK(requests[14].command ==
        DoctorHostCommand::Mark);
    CHECK(requests[15].command ==
        DoctorHostCommand::Prepare);
    auto after_clear_mark = requests[14].mark;
    after_clear_mark.epoch = kEpoch + 1;
    after_clear_mark.action_time_ms =
        kMarkActionTimeMs + 2;
    CHECK(same_mark(
        requests[15].mark, after_clear_mark));
    CHECK(requests[15].mark.epoch != first_mark.epoch);
    CHECK(std::none_of(
        requests.begin(),
        requests.end(),
        [](const DoctorHostRequest &request) {
            return request.command ==
                DoctorHostCommand::Enable;
        }));
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_export_prepare_preview_save() {
    OwnedTempDirectory temp;
    const fs::path destination = temp.path() / L"doctor-export.zip";
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));
    std::atomic<std::int64_t> selected_issue_time{0};
    queue_export_through_preview(doctor, &selected_issue_time);
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::SaveExport, true);
    doctor.queue_picker(destination);
    const auto save_step = doctor.queue(
        DoctorHostCommand::Save,
        status_reply(doctor_state(true, true)), true);

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    SYSTEMTIME selected{};
    selected.wYear = 2024;
    selected.wMonth = 7;
    selected.wDay = 8;
    selected.wHour = 9;
    selected.wMinute = 10;
    selected.wSecond = 11;
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_RECENT),
        BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_MARK_SPECIFIC),
        BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_ISSUE_DATE),
        DTM_SETSYSTEMTIME, GDT_VALID,
        reinterpret_cast<LPARAM>(&selected));
    SendMessageW(
        window.control(SettingsWindow::ID_DOCTOR_ISSUE_TIME),
        DTM_SETSYSTEMTIME, GDT_VALID,
        reinterpret_cast<LPARAM>(&selected));
    std::int64_t expected_selected = 0;
    CHECK(local_epoch_ms(selected, &expected_selected));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 5 &&
            WaitForSingleObject(save_step->entered, 0) == WAIT_OBJECT_0;
    }));

    const auto requests = doctor.requests();
    CHECK(requests.size() == 5);
    CHECK(requests[1].command == DoctorHostCommand::Mark);
    CHECK(requests[2].command == DoctorHostCommand::Prepare);
    CHECK(requests[3].command == DoctorHostCommand::Preview);
    CHECK(requests[4].command == DoctorHostCommand::Save);
    CHECK(selected_issue_time.load() == expected_selected);
    CHECK(requests[2].mark.epoch == kEpoch);
    CHECK(requests[2].mark.action_time_ms == kMarkActionTimeMs);
    CHECK(requests[2].mark.selected_issue_time_ms ==
        selected_issue_time.load());
    CHECK(requests[2].mark.before_ms == 300'000);
    CHECK(requests[2].mark.after_ms == 30'000);
    CHECK(requests[3].snapshot.epoch == kEpoch);
    CHECK(requests[3].snapshot.handle == kSnapshotHandle);
    CHECK(requests[4].snapshot.epoch == kEpoch);
    CHECK(requests[4].snapshot.handle == kSnapshotHandle);
    CHECK(requests[4].destination == destination.wstring());
    const auto confirmations = doctor.confirmation_kinds();
    CHECK(confirmations.size() == 1);
    CHECK(confirmations[0] ==
        DoctorSettingsConfirmation::SaveExport);
    CHECK(doctor.picker_calls() == 1);
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_PREVIEW)),
        L"12"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_PREVIEW)),
        L"3"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_PREVIEW)),
        L"17"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_PREVIEW)),
        L"29"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_PREVIEW)),
        L"2024"));
    CHECK(SetEvent(save_step->release));
    CHECK(pump_messages_until(hwnd, [&] {
        return IsWindowEnabled(window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT)),
        L"已儲存"));
    doctor.queue(DoctorHostCommand::Status, status_reply(doctor_state(true, true)));
    SendMessageW(hwnd, WM_TIMER, 3100, 0);
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 6 &&
            IsWindowEnabled(window.control(SettingsWindow::ID_DOCTOR_TOGGLE));
    }));
    CHECK(contains(control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT)), L"已儲存"));
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int run_export_cancel_scenario(bool confirm_preview) {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));
    queue_export_through_preview(doctor);
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::SaveExport,
        confirm_preview);
    if (confirm_preview) doctor.queue_picker(std::nullopt);
    doctor.queue(
        DoctorHostCommand::Cancel,
        status_reply(doctor_state(true, true)));

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    const bool cancelled = pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 5 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    });
    if (!cancelled) {
        std::cerr << "cancel request sequence:";
        for (const auto &request : doctor.requests())
            std::cerr << ' ' << static_cast<unsigned>(request.command);
        std::cerr << "; valid=" << doctor.valid() << '\n';
    }
    CHECK(cancelled);

    const auto requests = doctor.requests();
    CHECK(requests[4].command == DoctorHostCommand::Cancel);
    CHECK(requests[4].snapshot.epoch == kEpoch);
    CHECK(requests[4].snapshot.handle == kSnapshotHandle);
    CHECK(doctor.picker_calls() == (confirm_preview ? 1U : 0U));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT)),
        L"取消"));
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_export_preview_and_picker_cancel() {
    CHECK(run_export_cancel_scenario(false) == 0);
    CHECK(run_export_cancel_scenario(true) == 0);
    return 0;
}

static int test_save_stale_epoch_discards_preview_and_never_reports_success() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));
    queue_export_through_preview(doctor);
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::SaveExport, true);
    doctor.queue_picker(temp.path() / L"stale-doctor-export.zip");

    DoctorHostClientResult stale;
    stale.reply.failure = DoctorStoreFailure::EpochMismatch;
    stale.reply.status = doctor_state(true, true, kEpoch + 1);
    doctor.queue(DoctorHostCommand::Save, stale);
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            DoctorHostClientResult result;
            result.reply.mark = request.mark;
            result.reply.mark.epoch = kEpoch + 1;
            result.reply.mark.action_time_ms = kMarkActionTimeMs + 1;
            return result;
        });

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 5 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT));
    }));

    const auto result_after_save =
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT));
    CHECK(!result_after_save.empty());
    CHECK(!contains(result_after_save, L"已儲存"));
    CHECK(control_text(
        window.control(SettingsWindow::ID_DOCTOR_PREVIEW)) ==
        L"尚未建立匯出預覽。");
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_EXPORT_CANCEL)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 6 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK));
    }));
    const auto requests = doctor.requests();
    CHECK(requests[5].command == DoctorHostCommand::Mark);
    CHECK(requests[5].mark.epoch == 0);
    CHECK(!contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT)),
        L"已儲存"));
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int run_authoritative_export_failure_recovery(
    DoctorHostCommand failed_command) {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));
    if (failed_command == DoctorHostCommand::Preview) {
        doctor.queue(
            DoctorHostCommand::Mark,
            [](const DoctorHostRequest &request) {
                DoctorHostClientResult result;
                result.reply.mark = request.mark;
                result.reply.mark.epoch = kEpoch;
                result.reply.mark.action_time_ms = kMarkActionTimeMs;
                return result;
            });
        DoctorHostClientResult prepared;
        prepared.reply.snapshot = {
            kEpoch, kSnapshotHandle};
        doctor.queue(DoctorHostCommand::Prepare, prepared);
        DoctorHostClientResult expired;
        expired.reply.failure =
            DoctorStoreFailure::ExportExpired;
        expired.reply.status =
            doctor_state(true, true, kEpoch + 1);
        doctor.queue(DoctorHostCommand::Preview, expired);
    } else {
        queue_export_through_preview(doctor);
        doctor.queue_confirmation(
            DoctorSettingsConfirmation::SaveExport, false);
        DoctorHostClientResult expired;
        expired.reply.failure =
            DoctorStoreFailure::ExportExpired;
        expired.reply.status =
            doctor_state(true, true, kEpoch + 1);
        doctor.queue(failed_command, expired);
    }
    doctor.queue(
        DoctorHostCommand::Mark,
        [](const DoctorHostRequest &request) {
            DoctorHostClientResult result;
            result.reply.mark = request.mark;
            result.reply.mark.epoch = kEpoch + 1;
            result.reply.mark.action_time_ms =
                kMarkActionTimeMs + 1;
            return result;
        });

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    const std::size_t failed_request_count =
        failed_command == DoctorHostCommand::Preview ? 4 : 5;
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() ==
                failed_request_count &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK)) &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_CLEAR));
    }));
    CHECK(control_text(
        window.control(SettingsWindow::ID_DOCTOR_PREVIEW)) ==
        L"尚未建立匯出預覽。");
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_EXPORT_CANCEL)));
    CHECK(!contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT)),
        L"已儲存"));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() ==
            failed_request_count + 1;
    }));
    CHECK(doctor.requests().back().command ==
        DoctorHostCommand::Mark);
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_authoritative_preview_and_cancel_failures_release_preview() {
    CHECK(run_authoritative_export_failure_recovery(
        DoctorHostCommand::Preview) == 0);
    CHECK(run_authoritative_export_failure_recovery(
        DoctorHostCommand::Cancel) == 0);
    return 0;
}

static int run_transport_cancel_failure_keeps_preview(
    DoctorHostClientFailure failure) {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(true, true)));
    queue_export_through_preview(doctor);
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::SaveExport, false);
    DoctorHostClientResult cancelled;
    cancelled.failure = failure;
    doctor.queue(DoctorHostCommand::Cancel, cancelled);

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_EXPORT)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 5 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_EXPORT_CANCEL));
    }));
    CHECK(!control_text(
        window.control(SettingsWindow::ID_DOCTOR_PREVIEW)).empty());
    CHECK(control_text(window.control(SettingsWindow::ID_DOCTOR_PREVIEW)) !=
        L"尚未建立匯出預覽。");
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_MARK)));
    CHECK(!IsWindowEnabled(
        window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(!contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT)),
        L"已取消"));
    doctor.queue(
        DoctorHostCommand::Cancel,
        status_reply(doctor_state(true, true)));
    CHECK(click(
        window.control(SettingsWindow::ID_DOCTOR_EXPORT_CANCEL)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 6 &&
            control_text(window.control(
                SettingsWindow::ID_DOCTOR_PREVIEW)) == L"尚未建立匯出預覽。" &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_MARK)) &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_CLEAR));
    }));
    CHECK(doctor.valid());
    return 0;
}

static int test_transport_cancel_failures_keep_preview() {
    CHECK(run_transport_cancel_failure_keeps_preview(
        DoctorHostClientFailure::Timeout) == 0);
    CHECK(run_transport_cancel_failure_keeps_preview(
        DoctorHostClientFailure::Disconnect) == 0);
    CHECK(run_transport_cancel_failure_keeps_preview(
        DoctorHostClientFailure::EndpointUntrusted) == 0);
    return 0;
}

static int test_clear_cancel_and_failure_are_not_reported_as_success() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    auto state = doctor_state(false, false);
    state.raw_bytes = 2048;
    doctor.queue(DoctorHostCommand::Status, status_reply(state));
    doctor.queue_confirmation(
        DoctorSettingsConfirmation::Clear, false);

    SettingsWindow window(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        {},
        doctor_options(doctor));
    CHECK(window.create());
    const HWND hwnd = window.hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(window, doctor); }));
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(pump_probe(hwnd));
    CHECK(doctor.request_count() == 1);

    doctor.queue_confirmation(
        DoctorSettingsConfirmation::Clear, true);
    DoctorHostClientResult failed;
    failed.reply.failure = DoctorStoreFailure::ClearIncomplete;
    failed.reply.status = state;
    failed.reply.status.pending_clear = true;
    failed.reply.status.failure =
        DoctorStoreFailure::ClearIncomplete;
    doctor.queue(DoctorHostCommand::Clear, failed);
    CHECK(click(window.control(SettingsWindow::ID_DOCTOR_CLEAR)));
    CHECK(pump_messages_until(hwnd, [&] {
        return doctor.request_count() == 2 &&
            IsWindowEnabled(
                window.control(SettingsWindow::ID_DOCTOR_CLEAR));
    }));
    const auto result =
        control_text(window.control(SettingsWindow::ID_DOCTOR_RESULT));
    CHECK(contains(result, L"失敗"));
    CHECK(!contains(result, L"已清除"));
    CHECK(contains(
        control_text(window.control(SettingsWindow::ID_DOCTOR_STATUS)),
        L"未完成"));
    CHECK(doctor.confirmation_kinds().size() == 2);
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_close_does_not_wait_for_inflight_host_request() {
    OwnedTempDirectory temp;
    FakeDoctor doctor;
    doctor.queue(
        DoctorHostCommand::Status,
        status_reply(doctor_state(false, false)));
    doctor.queue_confirmation(DoctorSettingsConfirmation::Enable, true);
    auto blocked_enable = doctor.queue(
        DoctorHostCommand::Enable,
        status_reply(doctor_state(true, true)),
        true);

    bool closed = false;
    auto window = std::make_unique<SettingsWindow>(
        GetModuleHandleW(nullptr),
        temp.path() / L"settings.conf",
        SettingsWindow::CloseCallback([&] { closed = true; }),
        doctor_options(doctor));
    CHECK(window->create());
    const HWND hwnd = window->hwnd();
    doctor.expect_owner(hwnd);
    CHECK(pump_messages_until(
        hwnd, [&] { return doctor_ready(*window, doctor); }));
    CHECK(click(window->control(SettingsWindow::ID_DOCTOR_TOGGLE)));
    CHECK(pump_messages_until(hwnd, [&] {
        return WaitForSingleObject(
            blocked_enable->entered, 0) == WAIT_OBJECT_0;
    }));
    CHECK(blocked_enable->request_thread.load() != GetCurrentThreadId());
    CHECK(pump_probe(hwnd));

    CHECK(SendMessageW(hwnd, WM_CLOSE, 0, 0) == 0);
    CHECK(closed);
    CHECK(!IsWindow(hwnd));
    const ULONGLONG close_started = GetTickCount64();
    window.reset();
    const ULONGLONG close_elapsed = GetTickCount64() - close_started;
    CHECK(close_elapsed < kCloseMustNotWaitMs);
    CHECK(WaitForSingleObject(
        blocked_enable->completed, 0) == WAIT_TIMEOUT);

    SetEvent(blocked_enable->release);
    CHECK(WaitForSingleObject(
        blocked_enable->completed, kPumpTimeoutMs) == WAIT_OBJECT_0);
    CHECK(doctor.callbacks_ran_on(GetCurrentThreadId()));
    CHECK(doctor.valid());
    return 0;
}

static int test_existing_settings_save_and_cancel_still_work() {
    OwnedTempDirectory temp;
    const fs::path settings_path = temp.path() / L"settings.conf";
    Settings initial;
    initial.shift_toggle = false;
    initial.default_english = false;
    initial.save(settings_path);

    bool saved_closed = false;
    {
        SettingsWindow window(
            GetModuleHandleW(nullptr),
            settings_path,
            [&] { saved_closed = true; });
        CHECK(window.create());
        CHECK(TabCtrl_GetCurSel(window.control(SettingsWindow::ID_TAB)) ==
            static_cast<int>(SettingsWindowPage::General));
        SendMessageW(
            window.control(SettingsWindow::ID_SHIFT),
            BM_SETCHECK, BST_CHECKED, 0);
        CHECK(click(window.control(SettingsWindow::ID_OK)));
        CHECK(pump_messages_until(
            window.hwnd(), [&] { return !IsWindow(window.hwnd()); }));
    }
    CHECK(saved_closed);
    const Settings saved = Settings::load(settings_path);
    CHECK(saved.shift_toggle);
    CHECK(!saved.default_english);

    bool cancelled_closed = false;
    {
        SettingsWindow window(
            GetModuleHandleW(nullptr),
            settings_path,
            [&] { cancelled_closed = true; });
        CHECK(window.create());
        SendMessageW(
            window.control(SettingsWindow::ID_SHIFT),
            BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(
            window.control(SettingsWindow::ID_DEFAULT_ENGLISH),
            CB_SETCURSEL, 1, 0);
        CHECK(click(window.control(SettingsWindow::ID_CANCEL)));
        CHECK(pump_messages_until(
            window.hwnd(), [&] { return !IsWindow(window.hwnd()); }));
    }
    CHECK(cancelled_closed);
    CHECK(Settings::load(settings_path) == saved);
    return 0;
}

struct TestCase {
    const char *name;
    int (*run)();
};

constexpr TestCase kTests[] = {
    {"status-refresh-observes-collector-failure", test_status_refresh_observes_collector_failure},
    {
        "background-status-preserves-issue-editing",
        test_background_status_preserves_issue_editing,
    },
    {
        "toggle-consent-async-busy-and-status",
        test_toggle_consent_async_busy_and_status,
    },
    {
        "mark-recent-and-specific-local-time",
        test_mark_recent_and_specific_local_time,
    },
    {
        "off-history-mark-export-clear",
        test_off_history_supports_mark_export_and_clear,
    },
    {
        "export-reuses-unchanged-mark-and-tracks-edits",
        test_export_reuses_unedited_mark_and_tracks_edits,
    },
    {
        "export-prepare-preview-save",
        test_export_prepare_preview_save,
    },
    {
        "export-preview-and-picker-cancel",
        test_export_preview_and_picker_cancel,
    },
    {
        "save-stale-epoch",
        test_save_stale_epoch_discards_preview_and_never_reports_success,
    },
    {
        "authoritative-preview-and-cancel-failures",
        test_authoritative_preview_and_cancel_failures_release_preview,
    },
    {
        "transport-cancel-failures-keep-preview",
        test_transport_cancel_failures_keep_preview,
    },
    {
        "clear-cancel-and-failure",
        test_clear_cancel_and_failure_are_not_reported_as_success,
    },
    {
        "close-with-inflight-request",
        test_close_does_not_wait_for_inflight_host_request,
    },
    {
        "existing-settings-save-and-cancel",
        test_existing_settings_save_and_cancel_still_work,
    },
};

} // namespace

int main(int argc, char **argv) {
    std::cout << std::unitbuf;
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
        std::cerr << "Unknown doctor settings test case: "
                  << selected << '\n';
        return 2;
    }
    return 0;
}
