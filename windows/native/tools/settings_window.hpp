#ifndef BOSHIAMY_SETTINGS_WINDOW_HPP
#define BOSHIAMY_SETTINGS_WINDOW_HPP

#include "../settings.hpp"
#include "doctor_settings_ui.hpp"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace boshiamy {
namespace settings_window_detail {

inline bool valid_snapshot(const doctor::DoctorHostSnapshot &snapshot) {
    return snapshot.handle != 0;
}

inline bool host_result_ok(const doctor::DoctorHostClientResult &result) {
    return result.failure == doctor::DoctorHostClientFailure::None &&
        result.reply.protocol_failure ==
            doctor::DoctorHostProtocolFailure::None &&
        result.reply.failure == doctor::DoctorStoreFailure::None;
}

inline bool authoritative_stale_snapshot(
    const doctor::DoctorHostClientResult &result) {
    return result.failure == doctor::DoctorHostClientFailure::None &&
        result.reply.protocol_failure ==
            doctor::DoctorHostProtocolFailure::None &&
        (result.reply.failure ==
                doctor::DoctorStoreFailure::ExportExpired ||
            result.reply.failure ==
                doctor::DoctorStoreFailure::EpochMismatch);
}

class DoctorAsyncState {
public:
    ~DoctorAsyncState() = default;

    struct Completion {
        doctor::DoctorHostRequest request;
        doctor::DoctorHostClientResult result;
    };

    static constexpr UINT kCompletionMessage = WM_APP + 0x51;

    void attach(HWND hwnd, std::uint32_t token) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closing_) return;
        hwnd_ = hwnd;
        token_ = token;
        accepting_ = hwnd != nullptr;
    }

    bool begin(
        const std::shared_ptr<DoctorAsyncState> &self,
        DoctorSettingsRequest callback,
        const doctor::DoctorHostRequest &request,
        std::uint32_t *completion_id) {
        if (!callback || !completion_id) return false;
        std::uint32_t id = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closing_ || busy_) return false;
            busy_ = true;
            id = ++next_completion_id_;
            *completion_id = id;
        }
        try {
            std::thread(
                [self, callback = std::move(callback), request, id] {
                    self->run(callback, request, id);
                })
                .detach();
            return true;
        } catch (const std::system_error &) {
            std::lock_guard<std::mutex> guard(mutex_);
            busy_ = false;
            return false;
        }
    }

    bool take(
        std::uint32_t id,
        Completion *completion) {
        if (!completion) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = completions_.find(id);
        if (found == completions_.end()) return false;
        *completion = std::move(found->second);
        completions_.erase(found);
        return true;
    }

    void close(
        const std::shared_ptr<DoctorAsyncState> &self,
        const doctor::DoctorHostSnapshot &snapshot,
        const DoctorSettingsRequest &callback) {
        std::optional<doctor::DoctorHostRequest> cleanup;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
            accepting_ = false;
            hwnd_ = nullptr;
            if (valid_snapshot(snapshot))
                cleanup_snapshot_ = snapshot;
            if (!busy_ && cleanup_snapshot_ && callback) {
                cleanup = cancel_request(*cleanup_snapshot_);
                cleanup_snapshot_.reset();
                busy_ = true;
            }
        }
        if (!cleanup) return;
        try {
            std::thread(
                [self](
                    const DoctorSettingsRequest &request,
                    const doctor::DoctorHostRequest &cancel) {
                    self->run_cleanup(request, cancel);
                },
                callback,
                *cleanup)
                .detach();
        } catch (const std::system_error &) {
            std::lock_guard<std::mutex> guard(mutex_);
            busy_ = false;
        }
    }

private:
    DoctorAsyncState() = default;

    static doctor::DoctorHostClientResult invoke(
        const DoctorSettingsRequest &callback,
        const doctor::DoctorHostRequest &request) {
        try {
            return callback(request);
        } catch (...) {
            doctor::DoctorHostClientResult result;
            result.failure =
                doctor::DoctorHostClientFailure::Disconnect;
            result.platform_result_code =
                static_cast<std::int32_t>(E_UNEXPECTED);
            return result;
        }
    }

    static doctor::DoctorHostRequest cancel_request(
        const doctor::DoctorHostSnapshot &snapshot) {
        doctor::DoctorHostRequest request;
        request.command = doctor::DoctorHostCommand::Cancel;
        request.snapshot = snapshot;
        return request;
    }

    void run(
        const DoctorSettingsRequest &callback,
        const doctor::DoctorHostRequest &request,
        std::uint32_t id) {
        auto result = invoke(callback, request);
        std::optional<doctor::DoctorHostRequest> cleanup;
        bool post = false;
        HWND hwnd = nullptr;
        std::uint32_t token = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (request.command == doctor::DoctorHostCommand::Prepare &&
                host_result_ok(result) &&
                valid_snapshot(result.reply.snapshot)) {
                cleanup_snapshot_ = result.reply.snapshot;
            } else if (request.command == doctor::DoctorHostCommand::Save &&
                (host_result_ok(result) ||
                    authoritative_stale_snapshot(result))) {
                cleanup_snapshot_.reset();
            } else if (request.command ==
                    doctor::DoctorHostCommand::Preview &&
                authoritative_stale_snapshot(result)) {
                cleanup_snapshot_.reset();
            } else if (request.command ==
                    doctor::DoctorHostCommand::Cancel &&
                (host_result_ok(result) ||
                    authoritative_stale_snapshot(result))) {
                cleanup_snapshot_.reset();
            }

            if (closing_ && cleanup_snapshot_ &&
                request.command != doctor::DoctorHostCommand::Cancel) {
                cleanup = cancel_request(*cleanup_snapshot_);
                cleanup_snapshot_.reset();
            } else {
                busy_ = false;
            }

            if (accepting_ && !closing_ && hwnd_) {
                completions_.emplace(
                    id,
                    Completion{request, std::move(result)});
                hwnd = hwnd_;
                token = token_;
                post = true;
            }
        }

        if (post && !PostMessageW(
                hwnd,
                kCompletionMessage,
                static_cast<WPARAM>(token),
                static_cast<LPARAM>(id))) {
            std::lock_guard<std::mutex> guard(mutex_);
            completions_.erase(id);
        }
        if (cleanup) run_cleanup(callback, *cleanup);
    }

    void run_cleanup(
        const DoctorSettingsRequest &callback,
        const doctor::DoctorHostRequest &request) {
        invoke(callback, request);
        std::lock_guard<std::mutex> guard(mutex_);
        busy_ = false;
    }

    std::mutex mutex_;
    std::map<std::uint32_t, Completion> completions_;
    bool accepting_ = false;
    bool closing_ = false;
    bool busy_ = false;
    HWND hwnd_ = nullptr;
    std::uint32_t token_ = 0;
    std::uint32_t next_completion_id_ = 0;
    std::optional<doctor::DoctorHostSnapshot> cleanup_snapshot_;

    friend class DoctorAsyncStateFactory;
};

struct DoctorAsyncStateFactory {
    static std::shared_ptr<DoctorAsyncState> create() {
        return std::shared_ptr<DoctorAsyncState>(
            new DoctorAsyncState());
    }
};

} // namespace settings_window_detail

class SettingsWindow {
public:
    enum {
        ID_TAB = 1001,
        ID_DEFAULT_ENGLISH = 1005,
        ID_LANGUAGE = 1007,
        ID_SHIFT = 1008,
        ID_INHERIT = 1009,
        ID_RECOMMENDED = 1014,
        ID_SHORTEST = 1015,
        ID_ASSOCIATED = 1016,
        ID_ALL = 1017,
        ID_HINT_OUTPUT = 1018,
        ID_HINT_ONLY = 1019,
        ID_EXTENSIONS = 1021,
        ID_ENTER_SPACE = 1022,
        ID_UPPER = 1023,
        ID_LOWER_SYMBOLS = 1024,
        ID_RELATED_SHIFT = 1025,
        ID_OK = 1,
        ID_CANCEL = 2,
        ID_STATUS = 1100,
        ID_GENERAL_DESCRIPTION = 2001,
        ID_REVERSE_LABEL = 2101,
        ID_DOCTOR_TOGGLE = 3001,
        ID_DOCTOR_STATUS = 3002,
        ID_DOCTOR_MARK_RECENT = 3003,
        ID_DOCTOR_MARK_SPECIFIC = 3004,
        ID_DOCTOR_ISSUE_DATE = 3005,
        ID_DOCTOR_ISSUE_TIME = 3006,
        ID_DOCTOR_BEFORE_SECONDS = 3007,
        ID_DOCTOR_AFTER_SECONDS = 3008,
        ID_DOCTOR_MARK = 3009,
        ID_DOCTOR_EXPORT = 3010,
        ID_DOCTOR_EXPORT_CANCEL = 3011,
        ID_DOCTOR_CLEAR = 3012,
        ID_DOCTOR_PREVIEW = 3013,
        ID_DOCTOR_RESULT = 3014,
        ID_DOCTOR_STATUS_TIMER = 3100,
    };

    using CloseCallback = std::function<void()>;

    SettingsWindow(
        HINSTANCE instance,
        std::filesystem::path path,
        CloseCallback closed = {})
        : SettingsWindow(
            instance,
            std::move(path),
            std::move(closed),
            SettingsWindowOptions{}) {}

    SettingsWindow(
        HINSTANCE instance,
        std::filesystem::path path,
        CloseCallback closed,
        SettingsWindowOptions options)
        : instance_(instance),
          path_(std::move(path)),
          closed_(std::move(closed)),
          options_(std::move(options)),
          doctor_hooks_(
              doctor_hooks_with_defaults(options_.doctor)),
          doctor_token_(
              next_doctor_token().fetch_add(
                  1,
                  std::memory_order_relaxed) + 1) {}

    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    ~SettingsWindow() {
        close_doctor_async();
        if (hwnd_) DestroyWindow(hwnd_);
        release_class();
        if (font_) DeleteObject(font_);
    }

    HWND hwnd() const { return hwnd_; }
    HWND control(int id) const { return GetDlgItem(hwnd_, id); }

    bool create() {
        if (hwnd_) return true;
        valid_ = true;
        creation_failed_ = false;
        page_ = 0;
        adding_page_ = -1;
        edit_ = Settings{};
        general_controls_.clear();
        aux_controls_.clear();
        doctor_controls_.clear();
        doctor_async_ =
            settings_window_detail::DoctorAsyncStateFactory::create();
        doctor_status_known_ = false;
        doctor_busy_ = false;
        doctor_modal_ = false;
        doctor_status_requested_ = false;
        doctor_has_preview_ = false;
        doctor_preview_ready_ = false;
        doctor_has_mark_ = false;
        doctor_mark_ = {};
        doctor_recent_selection_ = true;
        doctor_selection_dirty_ = false;
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        INITCOMMONCONTROLSEX common{
            sizeof(common), ICC_TAB_CLASSES | ICC_DATE_CLASSES};
        if (!InitCommonControlsEx(&common) || !acquire_class())
            return false;
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(
                SPI_GETNONCLIENTMETRICS,
                sizeof(metrics),
                &metrics,
                0)) {
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            class_name_,
            L"嘸蝦米輸入法 設定",
            WS_OVERLAPPED |
                WS_CAPTION |
                WS_SYSMENU |
                WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            640,
            620,
            nullptr,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            if (font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            return false;
        }
        return true;
    }

    void show(int command = SW_SHOWNORMAL) {
        if (create()) {
            ShowWindow(hwnd_, command);
            UpdateWindow(hwnd_);
        }
    }

private:
    enum class DoctorAction {
        None,
        Status,
        Enable,
        Disable,
        Mark,
        ExportMark,
        ExportPrepare,
        ExportPreview,
        ExportSave,
        ExportCancel,
        Clear,
    };

    static constexpr wchar_t class_name_[] = L"LiuKaiSettingsWindow";
    static inline unsigned class_users_ = 0;
    static inline bool class_owned_ = false;
    static inline HINSTANCE class_instance_ = nullptr;
    static inline HICON class_icon_ = nullptr;

    static std::atomic<std::uint32_t> &next_doctor_token() {
        static std::atomic<std::uint32_t> value{0};
        return value;
    }

    static DoctorSettingsUiHooks doctor_hooks_with_defaults(
        DoctorSettingsUiHooks hooks) {
        const auto defaults = DoctorSettingsUiHooks::product();
        if (!hooks.request) hooks.request = defaults.request;
        if (!hooks.confirm) hooks.confirm = defaults.confirm;
        if (!hooks.pick_save_path)
            hooks.pick_save_path = defaults.pick_save_path;
        return hooks;
    }

    HINSTANCE instance_;
    std::filesystem::path path_;
    CloseCallback closed_;
    SettingsWindowOptions options_;
    DoctorSettingsUiHooks doctor_hooks_;
    HWND hwnd_ = nullptr;
    HWND tab_ = nullptr;
    HFONT font_ = nullptr;
    Settings edit_{};
    bool valid_ = true;
    bool class_ref_ = false;
    bool creation_failed_ = false;
    int page_ = 0;
    int adding_page_ = -1;
    std::vector<HWND> general_controls_;
    std::vector<HWND> aux_controls_;
    std::vector<HWND> doctor_controls_;
    std::shared_ptr<settings_window_detail::DoctorAsyncState>
        doctor_async_;
    std::uint32_t doctor_token_ = 0;
    doctor::DoctorStoreState doctor_status_{};
    bool doctor_status_known_ = false;
    bool doctor_status_requested_ = false;
    bool doctor_busy_ = false;
    bool doctor_modal_ = false;
    DoctorAction doctor_action_ = DoctorAction::None;
    doctor::DoctorHostMark doctor_mark_{};
    doctor::DoctorHostSnapshot doctor_snapshot_{};
    doctor::DoctorExportPreview doctor_preview_{};
    bool doctor_has_preview_ = false;
    bool doctor_preview_ready_ = false;
    bool doctor_has_mark_ = false;
    bool doctor_recent_selection_ = true;
    bool doctor_selection_dirty_ = false;

    bool doctor_history_available() const {
        return doctor_status_.failure ==
                doctor::DoctorStoreFailure::None &&
            !doctor_status_.pending_clear &&
            (doctor_status_.collecting ||
                doctor_status_.has_retained_range ||
                doctor_status_.raw_bytes != 0);
    }

    bool acquire_class() {
        if (class_ref_) return true;
        if (class_users_) {
            if (instance_ != class_instance_) return false;
            ++class_users_;
            class_ref_ = true;
            return true;
        }

        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = proc;
        window_class.hInstance = instance_;
        window_class.hCursor =
            LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        window_class.lpszClassName = class_name_;
        class_icon_ = reinterpret_cast<HICON>(LoadImageW(
            instance_,
            MAKEINTRESOURCEW(101),
            IMAGE_ICON,
            0,
            0,
            LR_DEFAULTSIZE));
        window_class.hIcon = class_icon_;
        window_class.hIconSm = class_icon_;
        const ATOM atom = RegisterClassExW(&window_class);
        if (atom) {
            class_owned_ = true;
            class_instance_ = instance_;
        } else if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            WNDCLASSEXW existing{sizeof(existing)};
            if (!GetClassInfoExW(
                    instance_,
                    class_name_,
                    &existing) ||
                existing.lpfnWndProc != proc ||
                existing.hInstance != instance_) {
                if (class_icon_) DestroyIcon(class_icon_);
                class_icon_ = nullptr;
                return false;
            }
            if (class_icon_) DestroyIcon(class_icon_);
            class_icon_ = nullptr;
            class_owned_ = false;
            class_instance_ = instance_;
        } else {
            if (class_icon_) DestroyIcon(class_icon_);
            class_icon_ = nullptr;
            return false;
        }
        class_users_ = 1;
        class_ref_ = true;
        return true;
    }

    void release_class() {
        if (!class_ref_) return;
        class_ref_ = false;
        if (--class_users_ != 0) return;
        if (class_owned_)
            UnregisterClassW(class_name_, class_instance_);
        if (class_icon_) DestroyIcon(class_icon_);
        class_icon_ = nullptr;
        class_owned_ = false;
        class_instance_ = nullptr;
    }

    static LRESULT CALLBACK proc(
        HWND hwnd,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) {
        auto *window = reinterpret_cast<SettingsWindow *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto *create =
                reinterpret_cast<CREATESTRUCTW *>(lparam);
            window = static_cast<SettingsWindow *>(
                create->lpCreateParams);
            SetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(window));
            window->hwnd_ = hwnd;
        }
        return window
            ? window->message(message, wparam, lparam)
            : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND add(
        const wchar_t *window_class,
        const wchar_t *text,
        DWORD style,
        int x,
        int y,
        int width,
        int height,
        int id) {
        HWND control = CreateWindowExW(
            0,
            window_class,
            text,
            WS_CHILD | WS_VISIBLE | style,
            x,
            y,
            width,
            height,
            hwnd_,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(id)),
            instance_,
            nullptr);
        if (font_)
            SendMessageW(
                control,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font_),
                TRUE);
        if (!control) creation_failed_ = true;
        if (adding_page_ == 0) {
            general_controls_.push_back(control);
        } else if (adding_page_ == 1) {
            aux_controls_.push_back(control);
        } else if (adding_page_ == 2) {
            doctor_controls_.push_back(control);
        }
        return control;
    }

    void label(
        const wchar_t *text,
        int x,
        int y,
        int id = 0,
        int width = 440,
        int height = 24) {
        add(
            L"STATIC",
            text,
            SS_LEFT,
            x,
            y,
            width,
            height,
            id);
    }

    void checkbox(int id, const wchar_t *text, int y) {
        add(
            L"BUTTON",
            text,
            BS_AUTOCHECKBOX | WS_TABSTOP,
            30,
            y,
            550,
            24,
            id);
    }

    void radio(
        int id,
        const wchar_t *text,
        int x,
        int y,
        bool first = false) {
        add(
            L"BUTTON",
            text,
            BS_AUTORADIOBUTTON |
                WS_TABSTOP |
                (first ? WS_GROUP : 0),
            x,
            y,
            215,
            24,
            id);
    }

    void init() {
        try {
            edit_ = Settings::load(path_);
        } catch (...) {
            valid_ = false;
        }

        doctor_async_->attach(hwnd_, doctor_token_);
        tab_ = add(
            WC_TABCONTROLW,
            L"",
            WS_TABSTOP,
            12,
            10,
            600,
            500,
            ID_TAB);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t *>(L"一般");
        TabCtrl_InsertItem(tab_, 0, &item);
        item.pszText = const_cast<wchar_t *>(L"輔助/查詢");
        TabCtrl_InsertItem(tab_, 1, &item);
        item.pszText = const_cast<wchar_t *>(L"診斷");
        TabCtrl_InsertItem(tab_, 2, &item);

        init_general_page();
        init_auxiliary_page();
        init_doctor_page();

        adding_page_ = -1;
        add(
            L"STATIC",
            L"",
            SS_LEFT,
            16,
            520,
            380,
            38,
            ID_STATUS);
        add(
            L"BUTTON",
            L"確定",
            BS_DEFPUSHBUTTON | WS_TABSTOP,
            430,
            526,
            82,
            28,
            ID_OK);
        add(
            L"BUTTON",
            L"取消",
            BS_PUSHBUTTON | WS_TABSTOP,
            522,
            526,
            82,
            28,
            ID_CANCEL);

        populate_general_controls();
        const int initial_page =
            static_cast<int>(options_.initial_page);
        TabCtrl_SetCurSel(tab_, initial_page);
        page(initial_page);
        enable();
        update_doctor_controls();
        if (!valid_) {
            SetWindowTextW(
                control(ID_STATUS),
                L"無法讀取設定檔；請修復後重新開啟。");
        }
    }

    void init_general_page() {
        adding_page_ = 0;
        label(
            L"選擇應用程式開啟時，輸入法的預設輸入模式。",
            24,
            48,
            ID_GENERAL_DESCRIPTION,
            560);
        label(L"預設輸入模式(D):", 32, 78);
        HWND default_mode = add(
            L"COMBOBOX",
            L"",
            CBS_DROPDOWNLIST | WS_TABSTOP,
            180,
            76,
            250,
            120,
            ID_DEFAULT_ENGLISH);
        SendMessageW(
            default_mode, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"中文模式"));
        SendMessageW(
            default_mode, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"英數模式"));
        label(L"預設多國語言模式(L):", 32, 112);
        HWND language = add(
            L"COMBOBOX",
            L"",
            CBS_DROPDOWNLIST | WS_TABSTOP,
            180,
            108,
            250,
            150,
            ID_LANGUAGE);
        for (const auto *value : {
                 L"繁體中文模式",
                 L"簡體中文模式",
                 L"簡繁轉換模式",
                 L"日文模式"}) {
            SendMessageW(
                language,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(value));
        }
        checkbox(
            ID_INHERIT,
            L"繼承系統輸入法狀態（忽略上方預設狀態設定）",
            146);
        checkbox(
            ID_SHIFT,
            L"使用 Shift 切換「中文/英文」模式",
            176);
        checkbox(ID_EXTENSIONS, L"顯示罕見字", 206);
        checkbox(
            ID_UPPER,
            L"Shift + 字母鍵改為輸入大寫字母",
            236);
        checkbox(
            ID_LOWER_SYMBOLS,
            L"Shift + ,.'[] 改為輸入下排符號",
            266);
    }

    void init_auxiliary_page() {
        adding_page_ = 1;
        label(
            L"注音/同音字反查",
            32,
            48,
            ID_REVERSE_LABEL);
        radio(ID_RECOMMENDED, L"提示建議碼", 40, 78, true);
        radio(ID_SHORTEST, L"提示最簡碼", 250, 78);
        radio(ID_ALL, L"提示所有拆碼", 40, 108);
        label(L"快打模式", 32, 144);
        radio(
            ID_HINT_OUTPUT,
            L"提示最簡碼並輸出文字",
            40,
            174,
            true);
        radio(ID_HINT_ONLY, L"只提示拆碼", 250, 174);
    }

    void init_doctor_page() {
        adding_page_ = 2;
        add(
            L"BUTTON",
            L"啟用 Doctor 資料收集",
            BS_AUTOCHECKBOX | WS_TABSTOP,
            24,
            44,
            570,
            24,
            ID_DOCTOR_TOGGLE);
        add(
            L"STATIC",
            L"尚未讀取 Doctor 狀態。",
            SS_LEFT,
            24,
            76,
            570,
            58,
            ID_DOCTOR_STATUS);
        add(
            L"BUTTON",
            L"剛才",
            BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP,
            24,
            144,
            170,
            24,
            ID_DOCTOR_MARK_RECENT);
        add(
            L"BUTTON",
            L"指定本機日期時間",
            BS_AUTORADIOBUTTON | WS_TABSTOP,
            220,
            144,
            210,
            24,
            ID_DOCTOR_MARK_SPECIFIC);
        add(
            DATETIMEPICK_CLASSW,
            L"",
            DTS_SHORTDATEFORMAT | WS_TABSTOP,
            220,
            176,
            155,
            24,
            ID_DOCTOR_ISSUE_DATE);
        add(
            DATETIMEPICK_CLASSW,
            L"",
            DTS_TIMEFORMAT | WS_TABSTOP,
            385,
            176,
            125,
            24,
            ID_DOCTOR_ISSUE_TIME);
        label(L"前", 24, 178, 0, 24);
        add(
            L"EDIT",
            L"300",
            ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER | WS_TABSTOP,
            52,
            176,
            52,
            24,
            ID_DOCTOR_BEFORE_SECONDS);
        label(L"秒", 108, 178, 0, 24);
        label(L"後", 136, 178, 0, 24);
        add(
            L"EDIT",
            L"30",
            ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER | WS_TABSTOP,
            164,
            176,
            44,
            24,
            ID_DOCTOR_AFTER_SECONDS);
        label(L"秒", 210, 178, 0, 24);
        add(
            L"BUTTON",
            L"標記問題",
            BS_PUSHBUTTON | WS_TABSTOP,
            24,
            214,
            140,
            28,
            ID_DOCTOR_MARK);
        add(
            L"BUTTON",
            L"匯出",
            BS_PUSHBUTTON | WS_TABSTOP,
            24,
            256,
            110,
            28,
            ID_DOCTOR_EXPORT);
        add(
            L"BUTTON",
            L"取消預覽",
            BS_PUSHBUTTON | WS_TABSTOP,
            144,
            256,
            110,
            28,
            ID_DOCTOR_EXPORT_CANCEL);
        add(
            L"BUTTON",
            L"清除資料",
            BS_PUSHBUTTON | WS_TABSTOP,
            420,
            256,
            110,
            28,
            ID_DOCTOR_CLEAR);
        add(
            L"STATIC",
            L"尚未建立匯出預覽。",
            SS_LEFT,
            24,
            294,
            570,
            78,
            ID_DOCTOR_PREVIEW);
        add(
            L"STATIC",
            L"",
            SS_LEFT,
            24,
            380,
            570,
            58,
            ID_DOCTOR_RESULT);
        label(
            L"一般欄位保留輸入階段的匿名診斷；敏感或未知欄位僅保留健康與生命週期。"
            L"部分應用程式不提供欄位種類。",
            24,
            448,
            0,
            570,
            44);

        SYSTEMTIME local{};
        GetLocalTime(&local);
        SendMessageW(
            control(ID_DOCTOR_ISSUE_DATE),
            DTM_SETSYSTEMTIME,
            GDT_VALID,
            reinterpret_cast<LPARAM>(&local));
        SendMessageW(
            control(ID_DOCTOR_ISSUE_TIME),
            DTM_SETSYSTEMTIME,
            GDT_VALID,
            reinterpret_cast<LPARAM>(&local));
        SendMessageW(
            control(ID_DOCTOR_MARK_RECENT),
            BM_SETCHECK,
            BST_CHECKED,
            0);
        SendMessageW(
            control(ID_DOCTOR_BEFORE_SECONDS),
            WM_SETTEXT,
            0,
            reinterpret_cast<LPARAM>(L"300"));
        SendMessageW(
            control(ID_DOCTOR_AFTER_SECONDS),
            WM_SETTEXT,
            0,
            reinterpret_cast<LPARAM>(L"30"));
    }

    void populate_general_controls() {
        SendMessageW(
            control(ID_DEFAULT_ENGLISH),
            CB_SETCURSEL,
            edit_.default_english ? 1 : 0,
            0);
        SendMessageW(
            control(ID_LANGUAGE),
            CB_SETCURSEL,
            static_cast<WPARAM>(edit_.default_language),
            0);
        SendMessageW(
            control(ID_INHERIT),
            BM_SETCHECK,
            edit_.inherit_system,
            0);
        SendMessageW(
            control(ID_SHIFT),
            BM_SETCHECK,
            edit_.shift_toggle,
            0);
        SendMessageW(
            control(ID_EXTENSIONS),
            BM_SETCHECK,
            edit_.show_extensions,
            0);
        SendMessageW(
            control(ID_UPPER),
            BM_SETCHECK,
            edit_.shift_uppercase,
            0);
        SendMessageW(
            control(ID_LOWER_SYMBOLS),
            BM_SETCHECK,
            edit_.shift_lower_symbols,
            0);

        int reverse_id = ID_RECOMMENDED;
        switch (edit_.reverse_mode) {
        case ReverseMode::Recommended:
            reverse_id = ID_RECOMMENDED;
            break;
        case ReverseMode::Shortest:
            reverse_id = ID_SHORTEST;
            break;
        case ReverseMode::All:
            reverse_id = ID_ALL;
            break;
        case ReverseMode::Associated:
            reverse_id = ID_RECOMMENDED;
            break;
        }
        CheckRadioButton(
            hwnd_,
            ID_RECOMMENDED,
            ID_ALL,
            reverse_id);
        CheckRadioButton(
            hwnd_,
            ID_HINT_OUTPUT,
            ID_HINT_ONLY,
            edit_.speed_hint_only
                ? ID_HINT_ONLY
                : ID_HINT_OUTPUT);
    }

    void page(int value) {
        if (value < static_cast<int>(SettingsWindowPage::General) ||
            value > static_cast<int>(SettingsWindowPage::Doctor)) {
            value = static_cast<int>(SettingsWindowPage::General);
        }
        page_ = value;
        for (HWND child : general_controls_)
            ShowWindow(child, value == 0 ? SW_SHOW : SW_HIDE);
        for (HWND child : aux_controls_)
            ShowWindow(child, value == 1 ? SW_SHOW : SW_HIDE);
        for (HWND child : doctor_controls_)
            ShowWindow(child, value == 2 ? SW_SHOW : SW_HIDE);
        if (value == static_cast<int>(SettingsWindowPage::Doctor)) {
            SetTimer(hwnd_, ID_DOCTOR_STATUS_TIMER, 5'000, nullptr);
            ensure_doctor_status();
        } else {
            KillTimer(hwnd_, ID_DOCTOR_STATUS_TIMER);
        }
    }

    void enable() {
        const bool enabled =
            SendMessageW(
                control(ID_INHERIT),
                BM_GETCHECK,
                0,
                0) != BST_CHECKED;
        EnableWindow(control(ID_DEFAULT_ENGLISH), enabled);
        EnableWindow(control(ID_LANGUAGE), enabled);
        EnableWindow(control(ID_OK), valid_);
    }

    void save() {
        if (!valid_) return;
        const int english = static_cast<int>(SendMessageW(
            control(ID_DEFAULT_ENGLISH),
            CB_GETCURSEL,
            0,
            0));
        const int language = static_cast<int>(SendMessageW(
            control(ID_LANGUAGE),
            CB_GETCURSEL,
            0,
            0));
        if (english < 0 || english > 1 ||
            language < 0 || language > 3) {
            return;
        }
        edit_.default_english = english == 1;
        edit_.default_language = static_cast<Mode>(language);
        edit_.inherit_system =
            SendMessageW(
                control(ID_INHERIT),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        edit_.shift_toggle =
            SendMessageW(
                control(ID_SHIFT),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        edit_.show_extensions =
            SendMessageW(
                control(ID_EXTENSIONS),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        edit_.shift_uppercase =
            SendMessageW(
                control(ID_UPPER),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        edit_.shift_lower_symbols =
            SendMessageW(
                control(ID_LOWER_SYMBOLS),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        if (SendMessageW(
                control(ID_RECOMMENDED),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED) {
            edit_.reverse_mode = ReverseMode::Recommended;
        } else if (SendMessageW(
                       control(ID_SHORTEST),
                       BM_GETCHECK,
                       0,
                       0) == BST_CHECKED) {
            edit_.reverse_mode = ReverseMode::Shortest;
        } else if (SendMessageW(
                       control(ID_ALL),
                       BM_GETCHECK,
                       0,
                       0) == BST_CHECKED) {
            edit_.reverse_mode = ReverseMode::All;
        } else {
            edit_.reverse_mode = ReverseMode::Recommended;
        }
        edit_.speed_hint_only =
            SendMessageW(
                control(ID_HINT_ONLY),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        edit_.related_enter_space = false;
        try {
            edit_.save(path_);
            destroy();
        } catch (...) {
            SetWindowTextW(
                control(ID_STATUS),
                L"無法儲存設定檔，請檢查檔案或資料夾。");
        }
    }

    void destroy() {
        close_doctor_async();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void close_doctor_async() {
        if (hwnd_) KillTimer(hwnd_, ID_DOCTOR_STATUS_TIMER);
        if (!doctor_async_) return;
        const auto snapshot = doctor_has_preview_
            ? doctor_snapshot_
            : doctor::DoctorHostSnapshot{};
        doctor_async_->close(
            doctor_async_,
            snapshot,
            doctor_hooks_.request);
    }

    void clear_prepared_preview() {
        doctor_has_preview_ = false;
        doctor_preview_ready_ = false;
        doctor_snapshot_ = {};
        doctor_preview_ = {};
        SetWindowTextW(
            control(ID_DOCTOR_PREVIEW),
            L"尚未建立匯出預覽。");
    }

    void forget_doctor_mark() {
        doctor_has_mark_ = false;
        doctor_mark_ = {};
        doctor_recent_selection_ = true;
        doctor_selection_dirty_ = false;
    }

    bool doctor_recent_selection() const {
        return SendMessageW(
            control(ID_DOCTOR_MARK_RECENT),
            BM_GETCHECK,
            0,
            0) == BST_CHECKED;
    }

    void remember_doctor_mark(
        const doctor::DoctorHostMark &mark) {
        doctor_mark_ = mark;
        doctor_has_mark_ =
            mark.epoch == doctor_status_.epoch &&
            mark.action_time_ms != 0;
        doctor_recent_selection_ =
            doctor_recent_selection();
        doctor_selection_dirty_ = false;
    }

    bool start_doctor_request(
        DoctorAction action,
        const doctor::DoctorHostRequest &request) {
        if (doctor_busy_ || !doctor_async_) return false;
        doctor_busy_ = true;
        doctor_action_ = action;
        std::uint32_t completion_id = 0;
        if (!doctor_async_->begin(
                doctor_async_,
                doctor_hooks_.request,
                request,
                &completion_id)) {
            doctor_busy_ = false;
            doctor_action_ = DoctorAction::None;
            set_doctor_result(
                L"無法啟動背景 Doctor 作業，請稍後再試。");
            update_doctor_controls();
            return false;
        }
        update_doctor_controls();
        return true;
    }

    void ensure_doctor_status(bool refresh = false) {
        if ((!refresh && doctor_status_requested_) || doctor_busy_ || doctor_modal_) return;
        doctor_status_requested_ = true;
        doctor::DoctorHostRequest request;
        request.command = doctor::DoctorHostCommand::Status;
        if (!start_doctor_request(
                DoctorAction::Status,
                request)) {
            doctor_status_requested_ = false;
        }
    }

    struct DoctorModalGuard {
        bool &active;
        explicit DoctorModalGuard(bool &value) : active(value) { active = true; }
        ~DoctorModalGuard() { active = false; }
    };

    bool confirm_doctor(DoctorSettingsConfirmation kind) {
        DoctorModalGuard guard(doctor_modal_);
        return doctor_hooks_.confirm && doctor_hooks_.confirm(hwnd_, kind);
    }

    void doctor_toggle() {
        if (doctor_busy_ || !doctor_status_known_) return;
        const bool requested =
            SendMessageW(
                control(ID_DOCTOR_TOGGLE),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        if (requested == doctor_status_.enabled) return;
        if (requested && !confirm_doctor(DoctorSettingsConfirmation::Enable)) {
            SendMessageW(
                control(ID_DOCTOR_TOGGLE),
                BM_SETCHECK,
                doctor_status_.enabled ? BST_CHECKED : BST_UNCHECKED,
                0);
            set_doctor_result(L"未啟用 Doctor；尚未送出變更。");
            return;
        }

        doctor::DoctorHostRequest request;
        request.command = requested
            ? doctor::DoctorHostCommand::Enable
            : doctor::DoctorHostCommand::Disable;
        start_doctor_request(
            requested
                ? DoctorAction::Enable
                : DoctorAction::Disable,
            request);
    }

    bool doctor_now_ms(std::int64_t *value) const {
        if (!value) return false;
        FILETIME file_time{};
        GetSystemTimeAsFileTime(&file_time);
        ULARGE_INTEGER current{};
        current.LowPart = file_time.dwLowDateTime;
        current.HighPart = file_time.dwHighDateTime;
        constexpr std::uint64_t kUnixEpochTicks =
            116444736000000000ULL;
        if (current.QuadPart < kUnixEpochTicks) return false;
        *value = static_cast<std::int64_t>(
            (current.QuadPart - kUnixEpochTicks) /
            10000ULL);
        return true;
    }

    static bool local_system_time_ms(
        const SYSTEMTIME &local,
        std::int64_t *value) {
        if (!value) return false;
        SYSTEMTIME utc{};
        FILETIME file_time{};
        if (!TzSpecificLocalTimeToSystemTime(
                nullptr,
                &local,
                &utc) ||
            !SystemTimeToFileTime(&utc, &file_time)) {
            return false;
        }
        ULARGE_INTEGER current{};
        current.LowPart = file_time.dwLowDateTime;
        current.HighPart = file_time.dwHighDateTime;
        constexpr std::uint64_t kUnixEpochTicks =
            116444736000000000ULL;
        if (current.QuadPart < kUnixEpochTicks) return false;
        *value = static_cast<std::int64_t>(
            (current.QuadPart - kUnixEpochTicks) /
            10000ULL);
        return true;
    }

    bool doctor_selected_issue_ms(std::int64_t *value) const {
        if (!value) return false;
        const bool recent =
            SendMessageW(
                control(ID_DOCTOR_MARK_RECENT),
                BM_GETCHECK,
                0,
                0) == BST_CHECKED;
        if (recent) return doctor_now_ms(value);

        SYSTEMTIME date{};
        SYSTEMTIME time{};
        if (SendMessageW(
                control(ID_DOCTOR_ISSUE_DATE),
                DTM_GETSYSTEMTIME,
                0,
                reinterpret_cast<LPARAM>(&date)) != GDT_VALID ||
            SendMessageW(
                control(ID_DOCTOR_ISSUE_TIME),
                DTM_GETSYSTEMTIME,
                0,
                reinterpret_cast<LPARAM>(&time)) != GDT_VALID) {
            return false;
        }
        date.wHour = time.wHour;
        date.wMinute = time.wMinute;
        date.wSecond = time.wSecond;
        date.wMilliseconds = time.wMilliseconds;
        date.wDayOfWeek = 0;
        return local_system_time_ms(date, value);
    }

    bool parse_seconds(int id, std::int64_t *milliseconds) const {
        if (!milliseconds) return false;
        wchar_t buffer[64]{};
        GetWindowTextW(control(id), buffer, std::size(buffer));
        if (buffer[0] == L'\0') return false;
        wchar_t *end = nullptr;
        errno = 0;
        const long long seconds =
            std::wcstoll(buffer, &end, 10);
        if (errno == ERANGE || !end || *end != L'\0' ||
            seconds < 0 ||
            static_cast<unsigned long long>(seconds) >
                static_cast<unsigned long long>(
                    std::numeric_limits<std::int64_t>::max() /
                    1000)) {
            return false;
        }
        *milliseconds = static_cast<std::int64_t>(seconds) * 1000;
        return true;
    }

    bool make_mark_request(doctor::DoctorHostRequest *request) {
        if (!request) return false;
        doctor::DoctorHostMark mark;
        if (!doctor_selected_issue_ms(
                &mark.selected_issue_time_ms) ||
            !parse_seconds(
                ID_DOCTOR_BEFORE_SECONDS,
                &mark.before_ms) ||
            !parse_seconds(
                ID_DOCTOR_AFTER_SECONDS,
                &mark.after_ms)) {
            set_doctor_result(
                L"請輸入有效的本機時間與前後秒數。");
            return false;
        }
        request->command = doctor::DoctorHostCommand::Mark;
        request->mark = mark;
        return true;
    }

    bool make_export_request(
        doctor::DoctorHostRequest *request,
        bool *requires_mark) {
        if (!request || !requires_mark) return false;
        *requires_mark = true;
        if (!doctor_has_mark_ ||
            doctor_mark_.epoch != doctor_status_.epoch) {
            return make_mark_request(request);
        }

        const bool recent = doctor_recent_selection();
        std::int64_t selected = 0;
        std::int64_t before = 0;
        std::int64_t after = 0;
        if (recent &&
                recent == doctor_recent_selection_ &&
                !doctor_selection_dirty_) {
            selected = doctor_mark_.selected_issue_time_ms;
        } else if (!doctor_selected_issue_ms(&selected)) {
            set_doctor_result(
                L"請輸入有效的本機時間與前後秒數。");
            return false;
        }
        if (!parse_seconds(
                ID_DOCTOR_BEFORE_SECONDS, &before) ||
            !parse_seconds(
                ID_DOCTOR_AFTER_SECONDS, &after)) {
            set_doctor_result(
                L"請輸入有效的本機時間與前後秒數。");
            return false;
        }
        const bool changed =
            doctor_selection_dirty_ ||
            recent != doctor_recent_selection_ ||
            (!recent &&
                selected != doctor_mark_.selected_issue_time_ms) ||
            before != doctor_mark_.before_ms ||
            after != doctor_mark_.after_ms;
        if (!changed) {
            request->command =
                doctor::DoctorHostCommand::Prepare;
            request->mark = doctor_mark_;
            *requires_mark = false;
            return true;
        }
        request->command =
            doctor::DoctorHostCommand::Mark;
        request->mark.selected_issue_time_ms = selected;
        request->mark.before_ms = before;
        request->mark.after_ms = after;
        return true;
    }

    void doctor_mark() {
        if (doctor_busy_ || !doctor_status_known_ ||
            !doctor_history_available() ||
            doctor_has_preview_) {
            return;
        }
        doctor::DoctorHostRequest request;
        if (!make_mark_request(&request)) return;
        start_doctor_request(DoctorAction::Mark, request);
    }

    void doctor_export() {
        if (doctor_busy_) return;
        if (doctor_has_preview_) {
            if (doctor_preview_ready_) {
                request_export_save();
            } else {
                doctor::DoctorHostRequest preview;
                preview.command =
                    doctor::DoctorHostCommand::Preview;
                preview.snapshot = doctor_snapshot_;
                start_doctor_request(
                    DoctorAction::ExportPreview,
                    preview);
            }
            return;
        }
        if (!doctor_status_known_ ||
            !doctor_history_available()) {
            return;
        }
        doctor::DoctorHostRequest request;
        bool requires_mark = true;
        if (!make_export_request(
                &request, &requires_mark)) {
            return;
        }
        start_doctor_request(
            requires_mark
                ? DoctorAction::ExportMark
                : DoctorAction::ExportPrepare,
            request);
    }

    void request_export_save() {
        if (doctor_busy_ || !doctor_has_preview_ ||
            !doctor_preview_ready_) {
            return;
        }
        if (!confirm_doctor(DoctorSettingsConfirmation::SaveExport)) {
            doctor_cancel_export();
            return;
        }
        if (!doctor_hooks_.pick_save_path) {
            doctor_cancel_export();
            return;
        }
        std::optional<std::filesystem::path> destination;
        {
            DoctorModalGuard guard(doctor_modal_);
            destination = doctor_hooks_.pick_save_path(hwnd_);
        }
        if (!destination) {
            doctor_cancel_export();
            return;
        }
        doctor::DoctorHostRequest request;
        request.command = doctor::DoctorHostCommand::Save;
        request.snapshot = doctor_snapshot_;
        request.destination = destination->wstring();
        start_doctor_request(
            DoctorAction::ExportSave,
            request);
    }

    void doctor_cancel_export() {
        if (doctor_busy_ || !doctor_has_preview_) return;
        doctor::DoctorHostRequest request;
        request.command = doctor::DoctorHostCommand::Cancel;
        request.snapshot = doctor_snapshot_;
        start_doctor_request(
            DoctorAction::ExportCancel,
            request);
    }

    void doctor_clear() {
        if (doctor_busy_ || !doctor_status_known_ ||
            doctor_has_preview_) {
            return;
        }
        if (!confirm_doctor(DoctorSettingsConfirmation::Clear)) {
            set_doctor_result(L"已取消清除；Doctor 資料未變更。");
            return;
        }
        doctor::DoctorHostRequest request;
        request.command = doctor::DoctorHostCommand::Clear;
        start_doctor_request(DoctorAction::Clear, request);
    }

    void handle_doctor_completion(
        std::uint32_t completion_id) {
        settings_window_detail::DoctorAsyncState::Completion completion;
        if (!doctor_async_ ||
            !doctor_async_->take(
                completion_id,
                &completion)) {
            return;
        }

        const DoctorAction action = doctor_action_;
        doctor_busy_ = false;
        doctor_action_ = DoctorAction::None;
        const auto &result = completion.result;
        switch (action) {
        case DoctorAction::Status:
            if (settings_window_detail::host_result_ok(result)) {
                doctor_status_ = result.reply.status;
                doctor_status_known_ = true;
                render_doctor_status();
            } else {
                doctor_status_requested_ = false;
                doctor_status_known_ = false;
                const auto text = L"無法確認 Doctor 狀態：" + doctor_error_text(result);
                SetWindowTextW(control(ID_DOCTOR_STATUS), text.c_str());
            }
            break;
        case DoctorAction::Enable:
        case DoctorAction::Disable: {
            if (settings_window_detail::host_result_ok(result)) {
                doctor_status_ = result.reply.status;
                doctor_status_known_ = true;
                render_doctor_status();
                set_doctor_result(
                    action == DoctorAction::Enable
                        ? L"Doctor 已啟用。"
                        : L"Doctor 已停用。");
            } else {
                set_doctor_result(
                    (action == DoctorAction::Enable
                         ? L"啟用 Doctor 失敗："
                         : L"停用 Doctor 失敗：") +
                    doctor_error_text(result));
            }
            break;
        }
        case DoctorAction::Mark:
            if (settings_window_detail::host_result_ok(result)) {
                remember_doctor_mark(result.reply.mark);
                set_doctor_result(
                    L"已標記問題；實際操作時間：" +
                    format_epoch_ms(
                        doctor_mark_.action_time_ms) +
                    L"。");
            } else {
                set_doctor_result(
                    L"問題標記失敗：" +
                    doctor_error_text(result));
            }
            break;
        case DoctorAction::ExportMark:
            if (settings_window_detail::host_result_ok(result)) {
                remember_doctor_mark(result.reply.mark);
                doctor::DoctorHostRequest prepare;
                prepare.command =
                    doctor::DoctorHostCommand::Prepare;
                prepare.mark = doctor_mark_;
                start_doctor_request(
                    DoctorAction::ExportPrepare,
                    prepare);
                return;
            }
            set_doctor_result(
                L"建立匯出標記失敗：" +
                doctor_error_text(result));
            break;
        case DoctorAction::ExportPrepare:
            if (settings_window_detail::host_result_ok(result) &&
                settings_window_detail::valid_snapshot(
                    result.reply.snapshot) &&
                result.reply.snapshot.epoch ==
                    doctor_mark_.epoch) {
                doctor_snapshot_ = result.reply.snapshot;
                doctor_has_preview_ = true;
                doctor_preview_ready_ = false;
                doctor::DoctorHostRequest preview;
                preview.command =
                    doctor::DoctorHostCommand::Preview;
                preview.snapshot = doctor_snapshot_;
                start_doctor_request(
                    DoctorAction::ExportPreview,
                    preview);
                return;
            }
            set_doctor_result(
                L"建立固定匯出失敗：" +
                (settings_window_detail::host_result_ok(result)
                    ? std::wstring(L"預覽期次不一致")
                    : doctor_error_text(result)));
            break;
        case DoctorAction::ExportPreview:
            if (settings_window_detail::host_result_ok(result) &&
                result.reply.preview.epoch ==
                    doctor_snapshot_.epoch) {
                doctor_preview_ = result.reply.preview;
                doctor_preview_ready_ = true;
                render_doctor_preview();
                request_export_save();
                update_doctor_controls();
                return;
            }
            if (settings_window_detail::authoritative_stale_snapshot(
                    result)) {
                clear_prepared_preview();
            }
            set_doctor_result(
                L"讀取匯出預覽失敗：" +
                (settings_window_detail::host_result_ok(result)
                    ? std::wstring(L"預覽期次不一致")
                    : doctor_error_text(result)));
            break;
        case DoctorAction::ExportSave:
            if (settings_window_detail::host_result_ok(result)) {
                clear_prepared_preview();
                set_doctor_result(L"Doctor 匯出已儲存。");
            } else if (
                settings_window_detail::authoritative_stale_snapshot(
                    result)) {
                clear_prepared_preview();
                set_doctor_result(
                    L"Doctor 匯出儲存失敗：" +
                    doctor_error_text(result));
            } else {
                set_doctor_result(
                    L"Doctor 匯出儲存失敗：" +
                    doctor_error_text(result) +
                    L" 預覽保留，可重試或明確取消。");
            }
            break;
        case DoctorAction::ExportCancel:
            if (settings_window_detail::host_result_ok(result)) {
                clear_prepared_preview();
                set_doctor_result(L"已取消 Doctor 匯出預覽。");
            } else if (
                settings_window_detail::authoritative_stale_snapshot(
                    result)) {
                clear_prepared_preview();
                set_doctor_result(
                    L"取消 Doctor 匯出失敗：" +
                    doctor_error_text(result));
            } else {
                set_doctor_result(
                    L"取消 Doctor 匯出失敗：" +
                    doctor_error_text(result) +
                    L" 預覽仍未確認清除。");
            }
            break;
        case DoctorAction::Clear:
            if (settings_window_detail::host_result_ok(result)) {
                clear_prepared_preview();
                forget_doctor_mark();
                set_doctor_result(L"Doctor 資料已清除。");
            } else {
                set_doctor_result(
                    L"清除 Doctor 資料失敗：" +
                    doctor_error_text(result));
            }
            break;
        case DoctorAction::None:
            break;
        }
        apply_status_if_present(
            result,
            completion.request.command);
        update_doctor_controls();
    }

    void apply_status_if_present(
        const doctor::DoctorHostClientResult &result,
        doctor::DoctorHostCommand command) {
        if (result.failure !=
                doctor::DoctorHostClientFailure::None ||
            result.reply.protocol_failure !=
                doctor::DoctorHostProtocolFailure::None) {
            return;
        }
        const auto &state = result.reply.status;
        const bool authoritative =
            command == doctor::DoctorHostCommand::Status ||
            command == doctor::DoctorHostCommand::Enable ||
            command == doctor::DoctorHostCommand::Disable ||
            command == doctor::DoctorHostCommand::Save ||
            command == doctor::DoctorHostCommand::Cancel ||
            command == doctor::DoctorHostCommand::Clear ||
            state.epoch != 0;
        if (!authoritative) return;
        if (doctor_has_mark_ &&
            doctor_mark_.epoch != state.epoch) {
            forget_doctor_mark();
        }
        doctor_status_ = state;
        doctor_status_known_ = true;
        render_doctor_status();
    }

    static std::int64_t saturating_add(
        std::int64_t value,
        std::int64_t delta) {
        if (delta > 0 &&
            value >
                std::numeric_limits<std::int64_t>::max() -
                    delta) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (delta < 0 &&
            value <
                std::numeric_limits<std::int64_t>::min() -
                    delta) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return value + delta;
    }

    static std::wstring format_epoch_ms(std::int64_t value) {
        if (value < 0 ||
            static_cast<std::uint64_t>(value) >
                (std::numeric_limits<std::uint64_t>::max() -
                    116444736000000000ULL) /
                    10000ULL) {
            return L"無效時間";
        }
        const auto ticks =
            116444736000000000ULL +
            static_cast<std::uint64_t>(value) *
                10000ULL;
        FILETIME file_time{};
        file_time.dwLowDateTime =
            static_cast<DWORD>(ticks & 0xffffffffULL);
        file_time.dwHighDateTime =
            static_cast<DWORD>(ticks >> 32);
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (!FileTimeToSystemTime(&file_time, &utc) ||
            !SystemTimeToTzSpecificLocalTime(
                nullptr,
                &utc,
                &local)) {
            return L"無效時間";
        }
        wchar_t buffer[64]{};
        std::swprintf(
            buffer,
            std::size(buffer),
            L"%04u/%02u/%02u %02u:%02u:%02u",
            static_cast<unsigned>(local.wYear),
            static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wDay),
            static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute),
            static_cast<unsigned>(local.wSecond));
        return buffer;
    }

    static std::wstring retained_range(
        const doctor::DoctorStoreState &state) {
        if (!state.has_retained_range ||
            state.pending_clear ||
            state.failure != doctor::DoctorStoreFailure::None ||
            state.oldest_retained_ms < 0 ||
            state.newest_retained_ms < state.oldest_retained_ms) {
            return L"保留區間：目前沒有可讀資料。";
        }
        return L"保留區間：" +
            format_epoch_ms(state.oldest_retained_ms) +
            L" 至 " +
            format_epoch_ms(state.newest_retained_ms) +
            L"。";
    }

    static std::wstring client_failure_text(
        doctor::DoctorHostClientFailure failure) {
        switch (failure) {
        case doctor::DoctorHostClientFailure::None:
            return {};
        case doctor::DoctorHostClientFailure::EndpointUnavailable:
            return L"Doctor 背景端點未就緒";
        case doctor::DoctorHostClientFailure::AccessDenied:
            return L"無權存取 Doctor";
        case doctor::DoctorHostClientFailure::EndpointUntrusted:
            return L"Doctor 背景端點未受信任";
        case doctor::DoctorHostClientFailure::OwnerConflict:
            return L"Doctor 擁有者衝突";
        case doctor::DoctorHostClientFailure::Timeout:
            return L"Doctor 回應逾時";
        case doctor::DoctorHostClientFailure::Disconnect:
            return L"Doctor 背景連線中斷";
        case doctor::DoctorHostClientFailure::ProtocolRejected:
            return L"Doctor 通訊被拒絕";
        case doctor::DoctorHostClientFailure::InvalidRequest:
            return L"Doctor 要求無效";
        case doctor::DoctorHostClientFailure::ProcessStartFailed:
            return L"Doctor 背景程序無法啟動";
        }
        return L"Doctor 發生未知連線錯誤";
    }

    static std::wstring protocol_failure_text(
        doctor::DoctorHostProtocolFailure failure) {
        if (failure == doctor::DoctorHostProtocolFailure::None)
            return {};
        return L"Doctor 通訊格式無效";
    }

    static std::wstring store_failure_text(
        doctor::DoctorStoreFailure failure) {
        switch (failure) {
        case doctor::DoctorStoreFailure::None:
            return {};
        case doctor::DoctorStoreFailure::Disabled:
            return L"Doctor 未啟用";
        case doctor::DoctorStoreFailure::PrivacyRejected:
            return L"未符合隱私政策";
        case doctor::DoctorStoreFailure::InvalidSchema:
            return L"資料狀態無效";
        case doctor::DoctorStoreFailure::TornTail:
            return L"資料尾端不完整";
        case doctor::DoctorStoreFailure::EpochMismatch:
            return L"資料期次已變更，請重新建立預覽";
        case doctor::DoctorStoreFailure::EpochCommitFailed:
            return L"新資料期次無法完成";
        case doctor::DoctorStoreFailure::ClearIncomplete:
            return L"資料清除未完成";
        case doctor::DoctorStoreFailure::DiskFull:
            return L"磁碟空間不足";
        case doctor::DoctorStoreFailure::PermissionDenied:
            return L"沒有存取檔案的權限";
        case doctor::DoctorStoreFailure::IoError:
            return L"資料存取失敗";
        case doctor::DoctorStoreFailure::ExportExists:
            return L"已有未完成的匯出預覽";
        case doctor::DoctorStoreFailure::ExportExpired:
            return L"匯出預覽已過期";
        case doctor::DoctorStoreFailure::ExportLimit:
            return L"匯出內容超出限制";
        case doctor::DoctorStoreFailure::TotalLimit:
            return L"Doctor 資料總量超出限制";
        case doctor::DoctorStoreFailure::RecordTooLarge:
            return L"單筆事件超出限制";
        }
        return L"Doctor 儲存發生未知錯誤";
    }

    static std::wstring doctor_error_text(
        const doctor::DoctorHostClientResult &result) {
        std::wstring text = client_failure_text(result.failure);
        if (!text.empty()) return text;
        text = protocol_failure_text(
            result.reply.protocol_failure);
        if (!text.empty()) return text;
        return store_failure_text(result.reply.failure);
    }

    void render_doctor_status() {
        if (!control(ID_DOCTOR_STATUS)) return;
        std::wstring text =
            doctor_status_.enabled
                ? L"同意狀態：已開啟；"
                : L"同意狀態：已關閉；";
        text += doctor_status_.collecting
            ? L"實際收集：進行中。 "
            : L"實際收集：未進行。 ";
        if (doctor_status_.failure !=
            doctor::DoctorStoreFailure::None) {
            text += L"狀態中斷：" +
                store_failure_text(
                    doctor_status_.failure) +
                L"。 ";
        }
        if (doctor_status_.pending_clear) {
            text += L"清除未完成。 ";
        }
        if (doctor_status_.producer_limit_reached) {
            text += L"部分應用程式目前暫未收集。 ";
        }
        text += L"序列缺漏 " +
            std::to_wstring(doctor_status_.sequence_gap) +
            L"；忽略行數 " +
            std::to_wstring(doctor_status_.ignored_lines) +
            L"。 ";
        text += retained_range(doctor_status_);
        SetWindowTextW(
            control(ID_DOCTOR_STATUS),
            text.c_str());
    }

    void render_doctor_preview() {
        const std::int64_t begin = (std::max)(
            std::int64_t{0},
            saturating_add(
                doctor_mark_.selected_issue_time_ms,
                -doctor_mark_.before_ms));
        const std::int64_t end = saturating_add(
            doctor_mark_.selected_issue_time_ms,
            doctor_mark_.after_ms);
        std::wstring text =
            L"固定預覽：" +
            std::to_wstring(doctor_preview_.record_count) +
            L" 筆；區間：" +
            format_epoch_ms(begin) +
            L" 至 " +
            format_epoch_ms(end) +
            L"；前端缺少 " +
            std::to_wstring(
                doctor_preview_.missing_before_ms) +
            L" ms；後端缺少 " +
            std::to_wstring(
                doctor_preview_.missing_after_ms) +
            L" ms；序列缺漏 " +
            std::to_wstring(doctor_preview_.sequence_gap) +
            L"。";
        SetWindowTextW(
            control(ID_DOCTOR_PREVIEW),
            text.c_str());
    }

    void set_doctor_result(const std::wstring &text) {
        SetWindowTextW(
            control(ID_DOCTOR_RESULT),
            text.c_str());
    }

    static const wchar_t *action_button_text(
        DoctorAction action) {
        switch (action) {
        case DoctorAction::Status:
            return L"讀取 Doctor 狀態中…";
        case DoctorAction::Enable:
            return L"啟用 Doctor…";
        case DoctorAction::Disable:
            return L"停用 Doctor…";
        case DoctorAction::Mark:
        case DoctorAction::ExportMark:
            return L"標記問題中…";
        case DoctorAction::ExportPrepare:
        case DoctorAction::ExportPreview:
        case DoctorAction::ExportSave:
            return L"處理匯出中…";
        case DoctorAction::ExportCancel:
            return L"取消預覽中…";
        case DoctorAction::Clear:
            return L"清除資料中…";
        case DoctorAction::None:
            return L"";
        }
        return L"";
    }

    void update_doctor_controls() {
        if (!hwnd_) return;
        const bool ready =
            doctor_status_known_ && !doctor_busy_;
        const bool readable_state =
            doctor_status_known_ &&
            doctor_history_available();
        const bool readable =
            ready && readable_state;
        const bool editable =
            readable_state &&
            !doctor_has_preview_ &&
            (!doctor_busy_ ||
                doctor_action_ == DoctorAction::Status);
        EnableWindow(control(ID_DOCTOR_TOGGLE), ready);
        EnableWindow(
            control(ID_DOCTOR_MARK_RECENT),
            editable);
        EnableWindow(
            control(ID_DOCTOR_MARK_SPECIFIC),
            editable);
        EnableWindow(
            control(ID_DOCTOR_ISSUE_DATE),
            editable);
        EnableWindow(
            control(ID_DOCTOR_ISSUE_TIME),
            editable);
        EnableWindow(
            control(ID_DOCTOR_BEFORE_SECONDS),
            editable);
        EnableWindow(
            control(ID_DOCTOR_AFTER_SECONDS),
            editable);
        EnableWindow(
            control(ID_DOCTOR_MARK),
            readable && !doctor_has_preview_);
        EnableWindow(
            control(ID_DOCTOR_EXPORT),
            readable ||
                (ready && doctor_has_preview_));
        EnableWindow(
            control(ID_DOCTOR_EXPORT_CANCEL),
            ready && doctor_has_preview_);
        EnableWindow(
            control(ID_DOCTOR_CLEAR),
            ready && !doctor_has_preview_);

        if (!doctor_busy_) {
            SetWindowTextW(
                control(ID_DOCTOR_TOGGLE),
                L"啟用 Doctor 資料收集");
            SetWindowTextW(
                control(ID_DOCTOR_MARK),
                L"標記問題");
            SetWindowTextW(
                control(ID_DOCTOR_EXPORT),
                doctor_has_preview_
                    ? (doctor_preview_ready_
                        ? L"儲存預覽"
                        : L"重試預覽")
                    : L"匯出");
            SetWindowTextW(
                control(ID_DOCTOR_EXPORT_CANCEL),
                L"取消預覽");
            SetWindowTextW(
                control(ID_DOCTOR_CLEAR),
                L"清除資料");
            if (doctor_status_known_) {
                SendMessageW(
                    control(ID_DOCTOR_TOGGLE),
                    BM_SETCHECK,
                    doctor_status_.enabled
                        ? BST_CHECKED
                        : BST_UNCHECKED,
                    0);
            }
            return;
        }

        const wchar_t *processing =
            action_button_text(doctor_action_);
        switch (doctor_action_) {
        case DoctorAction::Enable:
        case DoctorAction::Disable:
        case DoctorAction::Status:
            SetWindowTextW(
                control(ID_DOCTOR_TOGGLE),
                processing);
            break;
        case DoctorAction::Mark:
        case DoctorAction::ExportMark:
            SetWindowTextW(
                control(ID_DOCTOR_MARK),
                processing);
            break;
        case DoctorAction::ExportPrepare:
        case DoctorAction::ExportPreview:
        case DoctorAction::ExportSave:
            SetWindowTextW(
                control(ID_DOCTOR_EXPORT),
                processing);
            break;
        case DoctorAction::ExportCancel:
            SetWindowTextW(
                control(ID_DOCTOR_EXPORT_CANCEL),
                processing);
            break;
        case DoctorAction::Clear:
            SetWindowTextW(
                control(ID_DOCTOR_CLEAR),
                processing);
            break;
        case DoctorAction::None:
            break;
        }
    }

    LRESULT message(
        UINT value,
        WPARAM wparam,
        LPARAM lparam) {
        switch (value) {
        case WM_CREATE:
            init();
            return creation_failed_ ? -1 : 0;
        case DM_GETDEFID:
            return MAKELONG(ID_OK, DC_HASDEFID);
        case WM_TIMER:
            if (wparam == ID_DOCTOR_STATUS_TIMER &&
                page_ == static_cast<int>(SettingsWindowPage::Doctor) &&
                !IsIconic(hwnd_)) {
                ensure_doctor_status(true);
            }
            return 0;
        case settings_window_detail::DoctorAsyncState::
            kCompletionMessage:
            if (static_cast<std::uint32_t>(wparam) ==
                doctor_token_) {
                handle_doctor_completion(
                    static_cast<std::uint32_t>(lparam));
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED &&
                (LOWORD(wparam) == ID_DOCTOR_MARK_RECENT ||
                    LOWORD(wparam) ==
                        ID_DOCTOR_MARK_SPECIFIC)) {
                doctor_selection_dirty_ = true;
            }
            switch (LOWORD(wparam)) {
            case ID_INHERIT:
                enable();
                break;
            case ID_OK:
                save();
                break;
            case ID_CANCEL:
                destroy();
                break;
            case ID_DOCTOR_TOGGLE:
                doctor_toggle();
                break;
            case ID_DOCTOR_MARK:
                doctor_mark();
                break;
            case ID_DOCTOR_EXPORT:
                doctor_export();
                break;
            case ID_DOCTOR_EXPORT_CANCEL:
                doctor_cancel_export();
                break;
            case ID_DOCTOR_CLEAR:
                doctor_clear();
                break;
            }
            return 0;
        case WM_NOTIFY: {
            const auto *header =
                reinterpret_cast<NMHDR *>(lparam);
            if (header->idFrom == ID_TAB &&
                header->code == TCN_SELCHANGE) {
                page(TabCtrl_GetCurSel(tab_));
            } else if (
                (header->idFrom ==
                        ID_DOCTOR_ISSUE_DATE ||
                    header->idFrom ==
                        ID_DOCTOR_ISSUE_TIME) &&
                header->code == DTN_DATETIMECHANGE) {
                doctor_selection_dirty_ = true;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (wparam == VK_RETURN) {
                save();
                return 0;
            }
            break;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_NCDESTROY: {
            close_doctor_async();
            HWND dying = hwnd_;
            const LRESULT result =
                DefWindowProcW(dying, value, wparam, lparam);
            SetWindowLongPtrW(
                dying,
                GWLP_USERDATA,
                0);
            hwnd_ = nullptr;
            if (closed_) {
                try {
                    closed_();
                } catch (...) {
                }
            }
            return result;
        }
        }
        return DefWindowProcW(
            hwnd_,
            value,
            wparam,
            lparam);
    }
};

} // namespace boshiamy

#endif
