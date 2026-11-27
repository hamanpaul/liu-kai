#include "../tsf/language_bar.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "doctor_menu_fixture.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

int main() {
    const CLSID service = {0x01234567, 0x89ab, 0xcdef, {0x81,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}};
    std::vector<boshiamy::MenuCommand> selected;
    UINT popup_selection = 0;
    unsigned popup_count = 0;
    auto* item = new boshiamy::LanguageBarItem(service, [&](boshiamy::MenuCommand command) {
        selected.push_back(command); return S_OK;
    }, boshiamy::IconSource::from_path(L""), {},
        [&](HMENU menu, POINT point, const RECT*) -> UINT {
            ++popup_count;
            if (point.x != 34 || point.y != 56 || GetMenuItemCount(menu) != 14)
                return 0;
            MENUITEMINFOW version{sizeof(version)};
            version.fMask = MIIM_STATE;
            if (!GetMenuItemInfoW(menu, 0x5123, FALSE, &version) ||
                !(version.fState & MFS_DISABLED)) return 0;
            return popup_selection;
        });
    const std::wstring directory = L"C:\\Users\\Test\\AppData\\Local\\LiuKai\\doctor";
    item->set_doctor_metadata(L"0.1.1", directory);
    bool collecting = false;
    bool status_failed = false;
    item->set_doctor_status_reader([&]() {
        if (status_failed) throw std::runtime_error("test status failure");
        return collecting;
    });
    DoctorMenuCapture first;
    CHECK(item->InitMenu(&first) == S_OK);
    CHECK(first.entries.size() == 14);
    CHECK(first.contains(L"Doctor 設定（未收集）"));
    CHECK(first.contains(L"0.1.1"));
    CHECK(std::any_of(first.entries.begin(), first.entries.end(), [](const auto& e) {
        return e.text.find(L"0.1.1") != std::wstring::npos && (e.flags & 0x10) != 0;
    }));
    CHECK(first.contains(directory));
    collecting = true;
    DoctorMenuCapture active;
    CHECK(item->InitMenu(&active) == S_OK);
    CHECK(active.contains(L"Doctor 設定（收集中）"));
    BSTR tooltip = nullptr;
    CHECK(item->GetTooltipString(&tooltip) == S_OK);
    CHECK(std::wstring(tooltip, SysStringLen(tooltip)).find(L"Doctor：收集中") != std::wstring::npos);
    SysFreeString(tooltip);
    status_failed = true;
    DoctorMenuCapture unavailable;
    CHECK(item->InitMenu(&unavailable) == S_OK);
    CHECK(unavailable.contains(L"Doctor 設定（狀態未知）"));
    status_failed = false;
    CHECK(std::any_of(first.entries.begin(), first.entries.end(), [&](const auto& e) {
        return e.id == static_cast<UINT>(boshiamy::MenuCommand::DoctorFolder) &&
            e.text.find(directory) != std::wstring::npos;
    }));
    CHECK(std::any_of(first.entries.begin(), first.entries.end(), [](const auto& e) {
        return e.id == static_cast<UINT>(boshiamy::MenuCommand::DoctorSettings);
    }));
    CHECK(item->OnClick(TF_LBI_CLK_RIGHT, POINT{34, 56}, nullptr) == S_OK);
    CHECK(popup_count == 1);
    CHECK(selected.empty());
    popup_selection = static_cast<UINT>(boshiamy::MenuCommand::DoctorSettings);
    CHECK(item->OnClick(TF_LBI_CLK_RIGHT, POINT{34, 56}, nullptr) == S_OK);
    CHECK(selected == std::vector<boshiamy::MenuCommand>{boshiamy::MenuCommand::DoctorSettings});
    selected.clear();
    CHECK(item->OnMenuSelect(static_cast<UINT>(boshiamy::MenuCommand::DoctorSettings)) == S_OK);
    CHECK(item->OnMenuSelect(static_cast<UINT>(boshiamy::MenuCommand::DoctorFolder)) == S_OK);
    CHECK(selected == std::vector<boshiamy::MenuCommand>({
        boshiamy::MenuCommand::DoctorSettings, boshiamy::MenuCommand::DoctorFolder}));
    item->set_doctor_metadata(L"0.1.2", directory);
    DoctorMenuCapture updated;
    CHECK(item->InitMenu(&updated) == S_OK);
    CHECK(updated.contains(L"0.1.2"));
    CHECK(!updated.contains(L"0.1.1"));
    item->Release();
    std::cout << "Doctor language menu metadata and actions passed\n";
}
