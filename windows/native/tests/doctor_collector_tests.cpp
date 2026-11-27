#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <iterator>
#include <iostream>
#include <string>
#include <thread>

#include "../doctor_poc.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

using namespace boshiamy::doctor;

static int test_shared_command_parser() {
    wchar_t collector_path[] = L"C:\\Temp\\doctor";
    wchar_t collector_program[] = L"LiuKaiSettings.exe";
    wchar_t collector_mode[] = L"--doctor-collector";
    wchar_t output_arg[] = L"--output-dir";
    wchar_t *collector[] = {
        collector_program, collector_mode, output_arg, collector_path,
    };
    const auto collect = poc::parse(
        static_cast<int>(std::size(collector)), collector);
    CHECK(collect.kind == poc::CommandKind::Collect);
    CHECK(collect.output == std::filesystem::path(collector_path));

    wchar_t once_mode[] = L"--doctor-once";
    wchar_t status_mode[] = L"status";
    wchar_t output_mode[] = L"--output";
    wchar_t *once[] = {
        collector_program, once_mode, status_mode, output_mode, collector_path,
    };
    const auto status = poc::parse(static_cast<int>(std::size(once)), once);
    CHECK(status.kind == poc::CommandKind::Snapshot);
    CHECK(status.output == std::filesystem::path(collector_path));

    wchar_t probe_mode[] = L"--doctor-probe";
    wchar_t access_mode[] = L"access-check";
    wchar_t pid_mode[] = L"--pid";
    wchar_t pid_value[] = L"1234";
    wchar_t creation_mode[] = L"--process-creation";
    wchar_t creation_value[] = L"0x12345678";
    wchar_t *access[] = {
        collector_program, probe_mode, access_mode, pid_mode, pid_value,
        creation_mode, creation_value, output_mode, collector_path,
    };
    const auto access_check = poc::parse(
        static_cast<int>(std::size(access)), access);
    CHECK(access_check.kind == poc::CommandKind::AccessCheck);
    CHECK(access_check.process_id == 1234);
    CHECK(access_check.process_creation == 0x12345678);
    return 0;
}

int main() {
    if (const int result = test_shared_command_parser()) return result;
    // A rejected relative path must not consult an unrelated Win32 last error.
    SetLastError(ERROR_ALREADY_EXISTS);
    CHECK(poc::Collector(L"relative-doctor-path").run() == 2);
    SetLastError(ERROR_ACCESS_DENIED);
    CHECK(poc::Collector(L"relative-doctor-path").run() == 2);
    wchar_t temp[MAX_PATH]{};
    CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) > 0);
    const auto directory = std::filesystem::path(temp) /
        (L"liu-kai-doctor-collector-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));

    ProcessProducer producer;
    CHECK(producer.ensure());
    ScopeObservation ordinary{};
    ordinary.context_available = true;
    ordinary.query_result = S_OK;
    ordinary.token_count = 1;
    ordinary.tokens[0] = ScopeToken::Default;
    producer.publish_scope(ordinary);

    int collector_result = -1;
    std::thread collector([&] {
        collector_result = poc::Collector(directory).run();
    });

    bool written = false;
    std::string status;
    for (std::size_t attempt = 0; attempt < 150 && !written; ++attempt) {
        EventRecord record{};
        record.operation_id = 41;
        record.monotonic_ns = monotonic_nanoseconds();
        record.event_code = static_cast<std::uint32_t>(Event::Focus);
        record.callback_source = static_cast<std::uint32_t>(Event::Focus);
        record.scope = static_cast<std::uint32_t>(ScopeClass::Ordinary);
        if (producer.try_write(record) == WriteResult::Written) written = true;
        Sleep(25);
    }

    bool observed = false;
    for (std::size_t attempt = 0; attempt < 150 && written; ++attempt) {
        if (poc::read_file(directory / L"status.json", &status) &&
            status.find("\"eventsRead\":1") != std::string::npos) {
            observed = true;
            break;
        }
        Sleep(25);
    }
    const int snapshot_result = observed ? poc::snapshot(directory) : 5;
    const int stop_result = poc::stop(directory);
    collector.join();

    CHECK(written);
    CHECK(observed);
    CHECK(snapshot_result == 0);
    CHECK(stop_result == 0);
    CHECK(collector_result == 0);
    CHECK(status.find("\"collectorAlive\":1") != std::string::npos);
    CHECK(status.find("\"hostKind\":") != std::string::npos);
    CHECK(status.find("\"integrityLevelRid\":") != std::string::npos);
    CHECK(status.find("\"scopeQueryHr\":0") != std::string::npos);
    CHECK(status.find("\"scopeAppPropertyHr\":") != std::string::npos);
    CHECK(status.find("\"scopeRequestOuterHr\":") != std::string::npos);
    CHECK(status.find("\"scopeRequestInnerHr\":") != std::string::npos);
    CHECK(status.find("\"scopePropertyValueHr\":") != std::string::npos);
    CHECK(status.find("\"scopeVariantType\":") != std::string::npos);
    CHECK(status.find("\"scopeClass\":1") != std::string::npos);

    const auto access_output = directory / L"access.json";
    std::uint64_t process_creation = 0;
    CHECK(boshiamy::doctor::detail::process_creation_stamp(&process_creation));
    CHECK(poc::access_check(GetCurrentProcessId(), process_creation,
        access_output) == 0);
    std::string access;
    CHECK(poc::read_file(access_output, &access));
    CHECK(access.find("\"openHr\":0") != std::string::npos);
    CHECK(access.find("\"targetValidationHr\":0") != std::string::npos);
    CHECK(access.find("\"mappingOpenHr\":0") != std::string::npos);
    CHECK(access.find("\"ownerValidationHr\":0") != std::string::npos);
    CHECK(access.find("\"opened\":1") != std::string::npos);

    std::string events;
    std::string snapshot;
    CHECK(poc::read_file(directory / L"events.jsonl", &events));
    CHECK(poc::read_file(directory / L"snapshot.json", &snapshot));
    CHECK(events.find("\"eventCode\":5") != std::string::npos);
    CHECK(events.find("\"keyCode\"") == std::string::npos);
    CHECK(events.find("\"coordinate\"") == std::string::npos);
    CHECK(snapshot.find("\"eventsRead\":1") != std::string::npos);
    CHECK(snapshot.find("\"scopeClass\":1") != std::string::npos);
    CHECK(std::filesystem::file_size(directory / L"events.jsonl") <=
        poc::kMaxEventFileBytes);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "Doctor collector, ownership, probe snapshot and bounded event output passed\n";
    return 0;
}
