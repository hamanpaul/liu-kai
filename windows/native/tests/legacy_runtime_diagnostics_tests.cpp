#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "../tsf/runtime_diagnostics.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

using namespace boshiamy::runtime_diagnostics;

int main() {
    wchar_t temp[MAX_PATH]{};
    CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) > 0);
    const auto directory = std::filesystem::path(temp) /
        (L"liu-kai-legacy-runtime-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto now = detail::utc_now_seconds();
    const auto marker = directory / kMarkerName;
    {
        std::ofstream output(marker, std::ios::binary);
        output << "LIUKAI_TSF_DIAGNOSTICS_V1\n" << (now + 300) << '\n';
        CHECK(output.good());
    }

    Journal journal(directory.wstring(), now);
    CHECK(journal.enabled());
    Record record;
    record.event = Event::ActivateEnd;
    record.has_focus = 1;
    CHECK(journal.write(record) == WriteResult::Written);
    CHECK(journal.write(record) == WriteResult::Written);

    bool event_file_found = false;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L"tsf-diagnostics-", 0) != 0) continue;
        event_file_found = true;
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(input)), {});
        CHECK(data.find("\"event\":3") != std::string::npos);
        CHECK(data.find("\"hasFocus\":1") != std::string::npos);
    }
    CHECK(event_file_found);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "Legacy runtime marker journal remains explicitly callable outside TSF callbacks\n";
    return 0;
}
