#ifndef BOSHIAMY_WINDOWS_DOCTOR_SETTINGS_UI_HPP
#define BOSHIAMY_WINDOWS_DOCTOR_SETTINGS_UI_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

#include "../doctor_host.hpp"

#include <cwchar>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace boshiamy {

enum class DoctorSettingsConfirmation {
    Enable,
    SaveExport,
    Clear,
};

using DoctorSettingsRequest =
    std::function<doctor::DoctorHostClientResult(
        const doctor::DoctorHostRequest &)>;
using DoctorSettingsConfirm =
    std::function<bool(HWND, DoctorSettingsConfirmation)>;
using DoctorSettingsSavePicker =
    std::function<std::optional<std::filesystem::path>(HWND)>;

struct DoctorSettingsUiHooks {
    DoctorSettingsRequest request;
    DoctorSettingsConfirm confirm;
    DoctorSettingsSavePicker pick_save_path;

    static DoctorSettingsUiHooks product();
};

enum class SettingsWindowPage : int {
    General = 0,
    Auxiliary = 1,
    Doctor = 2,
};

struct SettingsWindowOptions {
    SettingsWindowPage initial_page = SettingsWindowPage::General;
    DoctorSettingsUiHooks doctor{};
};

inline DoctorSettingsUiHooks DoctorSettingsUiHooks::product() {
    DoctorSettingsUiHooks hooks;
    hooks.request = [](const doctor::DoctorHostRequest &request) {
        return doctor::doctor_host::request(request, 4'000, true);
    };
    hooks.confirm = [](HWND owner, DoctorSettingsConfirmation kind) {
        const wchar_t *message = L"";
        const wchar_t *title = L"嘸．改 Doctor";
        switch (kind) {
        case DoctorSettingsConfirmation::Enable:
            message =
                L"啟用後會在本機記錄一般欄位的輸入流程狀態與耗時，"
                L"不記錄輸入文字、按鍵內容或座標。\n"
                L"敏感或無法辨識的欄位只保留健康與生命週期事件。\n"
                L"資料最多保留 32 MiB 或 7 天；此選擇會保留，"
                L"下次登入時繼續收集。確定要啟用 Doctor 嗎？";
            break;
        case DoctorSettingsConfirmation::SaveExport:
            message =
                L"預覽已固定。要選擇位置並儲存這份 Doctor ZIP 嗎？";
            break;
        case DoctorSettingsConfirmation::Clear:
            message =
                L"確定要清除目前可用的 Doctor 資料嗎？\n"
                L"此操作無法復原。";
            title = L"清除 Doctor 資料";
            break;
        }
        return MessageBoxW(
            owner,
            message,
            title,
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    };
    hooks.pick_save_path = [](HWND owner)
        -> std::optional<std::filesystem::path> {
        std::wstring destination(4096, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter =
            L"ZIP 壓縮檔 (*.zip)\0*.zip\0所有檔案 (*.*)\0*.*\0\0";
        dialog.lpstrFile = destination.data();
        dialog.nMaxFile = static_cast<DWORD>(destination.size());
        dialog.lpstrDefExt = L"zip";
        dialog.lpstrTitle = L"儲存 Doctor 匯出";
        dialog.Flags =
            OFN_EXPLORER |
            OFN_NOCHANGEDIR |
            OFN_OVERWRITEPROMPT |
            OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog)) return std::nullopt;
        destination.resize(
            static_cast<std::size_t>(
                std::wcslen(destination.data())));
        if (destination.empty()) return std::nullopt;
        return std::filesystem::path(destination);
    };
    return hooks;
}

} // namespace boshiamy

#endif
