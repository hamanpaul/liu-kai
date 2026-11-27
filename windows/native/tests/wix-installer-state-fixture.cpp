#include "../doctor_paths.hpp"
#include "../doctor_store.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace boshiamy;
using namespace boshiamy::doctor;
namespace fs = std::filesystem;

namespace {

const DoctorBuildMetadata kBuild{
    "0.1.0-installer-scenario-fixture",
    "506dcded002b2b3fa9edbe6b387d3e6797aa2383",
};
constexpr const wchar_t* kMarkerName = L"installer-scenario-fixture-v1.json";
constexpr const char* kMarkerText =
    "{\"kind\":\"synthetic-installer-scenario-fixture-v1\","
    "\"settings\":\"synthetic-distinct\","
    "\"doctor_enabled\":false,"
    "\"doctor_records\":1}\n";

class Clock final : public DoctorClock {
public:
    std::int64_t wall_utc_ms() const override {
        FILETIME value{};
        GetSystemTimeAsFileTime(&value);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = value.dwLowDateTime;
        ticks.HighPart = value.dwHighDateTime;
        return static_cast<std::int64_t>(
            ticks.QuadPart / 10000ULL - 11644473600000ULL);
    }

    std::int64_t monotonic_ns() const override {
        const auto elapsed =
            std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            elapsed).count();
    }
};

bool write_marker(const fs::path& path) {
    HANDLE file=CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::string text(kMarkerText);
    DWORD written=0;
    const BOOL ok=WriteFile(
        file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok != FALSE;
}

bool marker_valid(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string text(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    return text == kMarkerText;
}

bool settings_valid(const fs::path& path) {
    try {
        const Settings value=Settings::load(path);
        return !value.default_english &&
            !value.shift_toggle &&
            value.shift_uppercase &&
            value.speed_hint_only &&
            value.related_enter_space;
    }
    catch (...) {
        return false;
    }
}

int read_state(const fs::path& root, const fs::path& settings, const fs::path& marker) {
    Clock clock;
    auto store=DoctorStore::open(
        root, kBuild, clock, DoctorStoreLimits::defaults());
    if (!store) {
        std::cerr << "Cannot open synthetic Doctor state\n";
        return 1;
    }
    const auto status=store->status();
    const auto records=store->read_records();
    if (status.failure != DoctorStoreFailure::None || !records.ok()) {
        std::cerr << "Cannot read synthetic Doctor state\n";
        return 1;
    }
    std::cout
        << "{\"synthetic_fixture\":"
        << (marker_valid(marker) ? "true" : "false")
        << ",\"settings_present\":"
        << (fs::exists(settings) ? "true" : "false")
        << ",\"settings_valid\":"
        << (settings_valid(settings) ? "true" : "false")
        << ",\"doctor_present\":"
        << (fs::exists(root) ? "true" : "false")
        << ",\"doctor_enabled\":"
        << (status.enabled ? "true" : "false")
        << ",\"doctor_record_count\":"
        << records.value.size()
        << "}\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 ||
        (std::wstring(argv[1]) != L"--seed" &&
         std::wstring(argv[1]) != L"--read")) {
        std::cerr << "Usage: wix-installer-state-fixture --seed|--read\n";
        return 2;
    }
    try {
        const fs::path settings=settings_file_path();
        const fs::path root=doctor_directory();
        const fs::path marker=settings.parent_path() / kMarkerName;
        if (std::wstring(argv[1]) == L"--seed") {
            if (fs::exists(settings) || fs::exists(root) || fs::exists(marker)) {
                std::cerr << "Synthetic fixture roots already exist; refusing to seed over them\n";
                return 3;
            }
            fs::create_directories(settings.parent_path());
            if (!write_marker(marker))
                throw std::runtime_error("Cannot write synthetic marker");
            Settings value;
            value.default_english=false;
            value.shift_toggle=false;
            value.shift_uppercase=true;
            value.speed_hint_only=true;
            value.related_enter_space=true;
            value.save(settings);
            Clock clock;
            auto store=DoctorStore::open(
                root, kBuild, clock, DoctorStoreLimits::defaults());
            if (!store)
                throw std::runtime_error("Cannot open Doctor store");
            if (!store->enable().ok()) throw std::runtime_error("Cannot enable Doctor store");
            DoctorEventRecord event{};
            event.producer_id=71;
            event.session_id=1;
            event.epoch=store->status().epoch;
            event.operation_id=901;
            event.sequence=1;
            event.monotonic_ns=clock.monotonic_ns();
            event.wall_utc_ms=clock.wall_utc_ms();
            event.event_code=DoctorEventCode::KeyDown;
            event.callback_source=DoctorCallbackSource::KeyAction;
            event.scope=DoctorInputScope::Ordinary;
            event.phase=DoctorEventPhase::Composition;
            event.result_code=DoctorResultCode::Ok;
            if (!store->append(event).ok()) throw std::runtime_error("Cannot append Doctor event");
            if (!store->disable().ok()) throw std::runtime_error("Cannot disable Doctor store");
        }
        else if (!marker_valid(marker)) {
            std::cerr << "Synthetic fixture marker is absent; refusing private-state readback\n";
            return 4;
        }
        return read_state(root, settings, marker);
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
