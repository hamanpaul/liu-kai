#ifndef BOSHIAMY_WINDOWS_DOCTOR_STORE_HPP
#define BOSHIAMY_WINDOWS_DOCTOR_STORE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "doctor_event.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace boshiamy::doctor {

enum class DoctorStoreFailure : std::uint32_t {
    None = 0,
    Disabled = 1,
    PrivacyRejected = 2,
    InvalidSchema = 3,
    TornTail = 4,
    EpochMismatch = 5,
    EpochCommitFailed = 6,
    ClearIncomplete = 7,
    DiskFull = 8,
    PermissionDenied = 9,
    IoError = 10,
    ExportExists = 11,
    ExportExpired = 12,
    ExportLimit = 13,
    TotalLimit = 14,
    RecordTooLarge = 15,
};

struct DoctorStoreActionResult {
    DoctorStoreFailure failure = DoctorStoreFailure::None;
    std::int32_t platform_result_code = 0;

    bool ok() const noexcept {
        return failure == DoctorStoreFailure::None;
    }
};

template<class T>
struct DoctorStoreResult {
    T value{};
    DoctorStoreFailure failure = DoctorStoreFailure::None;
    std::int32_t platform_result_code = 0;

    bool ok() const noexcept {
        return failure == DoctorStoreFailure::None;
    }
};

enum class DoctorIoOperation : std::uint32_t {
    PersistControl,
    AppendRecord,
    RotateRecord,
    CommitEpoch,
    DeleteOldRecord,
    CreateExport,
    SaveExport,
};

struct DoctorIoFault {
    DoctorStoreFailure failure = DoctorStoreFailure::IoError;
    std::int32_t platform_result_code = 0;
};

struct DoctorIoFaults {
    virtual ~DoctorIoFaults() = default;
    virtual std::optional<DoctorIoFault> before(
        DoctorIoOperation operation) = 0;
};

struct DoctorClock {
    virtual ~DoctorClock() = default;
    virtual std::int64_t wall_utc_ms() const = 0;
    virtual std::int64_t monotonic_ns() const = 0;
};

struct DoctorStoreLimits {
    std::uint64_t max_raw_bytes = 32ULL * 1024 * 1024;
    std::int64_t max_age_ms = 7LL * 24 * 60 * 60 * 1000;
    std::uint64_t max_record_bytes = 1024ULL * 1024;
    std::uint64_t max_export_bytes = 32ULL * 1024 * 1024;
    std::uint64_t max_raw_and_temp_bytes =
        64ULL * 1024 * 1024;
    std::int64_t export_ttl_ms = 15LL * 60 * 1000;

    static DoctorStoreLimits defaults() {
        return DoctorStoreLimits{};
    }
};

struct DoctorStoreState {
    bool enabled = false;
    bool collecting = false;
    bool pending_clear = false;
    std::int64_t epoch = 0;
    DoctorStoreFailure failure = DoctorStoreFailure::None;
    std::int32_t platform_result_code = 0;
    std::uint64_t raw_bytes = 0;
    std::uint64_t temp_bytes = 0;
    std::uint64_t sequence_gap = 0;
    std::uint64_t ignored_lines = 0;
    bool producer_limit_reached = false;
    bool has_retained_range = false;
    std::int64_t oldest_retained_ms = 0;
    std::int64_t newest_retained_ms = 0;
};

struct DoctorExportWindow {
    std::int64_t before_ms = 300000;
    std::int64_t after_ms = 30000;
};

struct DoctorIssueMark {
    std::int64_t epoch = 0;
    std::int64_t action_time_ms = 0;
    std::int64_t selected_issue_time_ms = 0;
    DoctorExportWindow window{};
};

struct DoctorExportSnapshot {
    std::int64_t epoch = 0;
    std::filesystem::path archive;
};

struct DoctorExportPreview {
    std::int64_t epoch = 0;
    std::size_t record_count = 0;
    std::uint64_t sequence_gap = 0;
    std::int64_t missing_before_ms = 0;
    std::int64_t missing_after_ms = 0;
};

namespace doctor_store_detail {

inline constexpr std::uint32_t kControlMagic = 0x43444b4cU;
inline constexpr std::uint32_t kControlVersion = 1;
inline constexpr std::size_t kMaximumControlBytes = 4096;
inline constexpr std::size_t kMaximumZipEntries = 3;
inline constexpr std::uint32_t kZipLocalSignature = 0x04034b50U;
inline constexpr std::uint32_t kZipCentralSignature = 0x02014b50U;
inline constexpr std::uint32_t kZipEndSignature = 0x06054b50U;
inline constexpr std::uint16_t kZipDosEpochDate = 0x0021U;
inline constexpr std::uint32_t kCrc32Polynomial = 0xedb88320U;

inline std::int32_t hresult_from_win32(DWORD error) noexcept {
    if (error == ERROR_SUCCESS) return static_cast<std::int32_t>(S_OK);
    return static_cast<std::int32_t>(HRESULT_FROM_WIN32(error));
}

inline DoctorStoreActionResult action_result(
    DoctorStoreFailure failure,
    std::int32_t platform_result_code = 0) {
    return DoctorStoreActionResult{failure, platform_result_code};
}

template<class T>
inline DoctorStoreResult<T> store_result(
    DoctorStoreFailure failure,
    std::int32_t platform_result_code = 0) {
    return DoctorStoreResult<T>{T{}, failure, platform_result_code};
}

inline DoctorStoreFailure map_win32(DWORD error) noexcept {
    if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL)
        return DoctorStoreFailure::DiskFull;
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION)
        return DoctorStoreFailure::PermissionDenied;
    return DoctorStoreFailure::IoError;
}

inline DoctorStoreFailure normalize_fault(DoctorStoreFailure failure) noexcept {
    switch (failure) {
    case DoctorStoreFailure::DiskFull:
    case DoctorStoreFailure::PermissionDenied:
    case DoctorStoreFailure::IoError:
    case DoctorStoreFailure::EpochCommitFailed:
    case DoctorStoreFailure::ClearIncomplete:
    case DoctorStoreFailure::ExportLimit:
    case DoctorStoreFailure::TotalLimit:
    case DoctorStoreFailure::RecordTooLarge:
        return failure;
    default:
        return DoctorStoreFailure::IoError;
    }
}

inline std::uint32_t crc32(std::string_view bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                ((crc & 1U) ? kCrc32Polynomial : 0U);
    }
    return ~crc;
}

inline void append_u16(std::string *bytes, std::uint16_t value) {
    bytes->push_back(static_cast<char>(value & 0xffU));
    bytes->push_back(static_cast<char>((value >> 8) & 0xffU));
}

inline void append_u32(std::string *bytes, std::uint32_t value) {
    bytes->push_back(static_cast<char>(value & 0xffU));
    bytes->push_back(static_cast<char>((value >> 8) & 0xffU));
    bytes->push_back(static_cast<char>((value >> 16) & 0xffU));
    bytes->push_back(static_cast<char>((value >> 24) & 0xffU));
}

inline void append_i32(std::string *bytes, std::int32_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(value));
}

inline void append_i64(std::string *bytes, std::int64_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(value) & 0xffffffffULL));
    append_u32(bytes, static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(value) >> 32) & 0xffffffffULL));
}

inline bool read_u16(
    std::string_view bytes,
    std::size_t offset,
    std::uint16_t *value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    *value = static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset]) |
        (static_cast<unsigned char>(bytes[offset + 1]) << 8));
    return true;
}

inline bool read_u32(
    std::string_view bytes,
    std::size_t offset,
    std::uint32_t *value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    *value =
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset])) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[offset + 3])) << 24);
    return true;
}

inline bool read_i32(
    std::string_view bytes,
    std::size_t offset,
    std::int32_t *value) noexcept {
    std::uint32_t raw = 0;
    if (!read_u32(bytes, offset, &raw)) return false;
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}

inline bool read_i64(
    std::string_view bytes,
    std::size_t offset,
    std::int64_t *value) noexcept {
    std::uint32_t low = 0, high = 0;
    if (!read_u32(bytes, offset, &low) ||
        !read_u32(bytes, offset + 4, &high))
        return false;
    const auto raw =
        static_cast<std::uint64_t>(low) |
        (static_cast<std::uint64_t>(high) << 32);
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}

inline bool is_safe_regular(const std::filesystem::path &path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool is_safe_directory(const std::filesystem::path &path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool ensure_directory(const std::filesystem::path &path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    return CreateDirectoryW(path.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS &&
        is_safe_directory(path);
}

inline bool ensure_tree(const std::filesystem::path &root) noexcept {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error || !is_safe_directory(root)) return false;
    return ensure_directory(root / L"records") &&
        ensure_directory(root / L"temp");
}

inline bool file_size_bounded(
    const std::filesystem::path &path,
    std::uint64_t maximum,
    std::uint64_t *size) noexcept {
    if (!is_safe_regular(path)) return false;
    LARGE_INTEGER value{};
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &value)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    if (value.QuadPart < 0 ||
        static_cast<std::uint64_t>(value.QuadPart) > maximum)
        return false;
    *size = static_cast<std::uint64_t>(value.QuadPart);
    return true;
}

inline DoctorStoreActionResult read_file_bounded(
    const std::filesystem::path &path,
    std::uint64_t maximum,
    std::string *bytes) noexcept {
    bytes->clear();
    std::uint64_t size = 0;
    if (!file_size_bounded(path, maximum, &size)) {
        const DWORD error = GetLastError();
        return action_result(
            map_win32(error == ERROR_SUCCESS ? ERROR_FILE_NOT_FOUND : error),
            hresult_from_win32(
                error == ERROR_SUCCESS ? ERROR_FILE_NOT_FOUND : error));
    }
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return action_result(map_win32(error), hresult_from_win32(error));
    }
    try {
        bytes->resize(static_cast<std::size_t>(size));
    } catch (...) {
        CloseHandle(file);
        return action_result(
            DoctorStoreFailure::IoError,
            static_cast<std::int32_t>(E_OUTOFMEMORY));
    }
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const auto remaining = bytes->size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7fffffffU ? 0x7fffffffU : remaining);
        DWORD copied = 0;
        if (!ReadFile(
            file,
            bytes->data() + offset,
            chunk,
            &copied,
            nullptr) ||
            copied == 0) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return action_result(
                map_win32(error == ERROR_SUCCESS ? ERROR_READ_FAULT : error),
                hresult_from_win32(
                    error == ERROR_SUCCESS ? ERROR_READ_FAULT : error));
        }
        offset += copied;
    }
    CloseHandle(file);
    return {};
}

inline DoctorStoreActionResult write_file_exact(
    const std::filesystem::path &path,
    std::string_view bytes,
    bool replace_existing) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return action_result(
            DoctorStoreFailure::PermissionDenied,
            static_cast<std::int32_t>(E_ACCESSDENIED));
    }
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        replace_existing ? CREATE_ALWAYS : CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return action_result(map_win32(error), hresult_from_win32(error));
    }
    std::size_t offset = 0;
    bool ok = true;
    DWORD last_error = ERROR_SUCCESS;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7fffffffU ? 0x7fffffffU : remaining);
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            ok = false;
            last_error = GetLastError();
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) {
        ok = false;
        last_error = GetLastError();
    }
    if (!CloseHandle(file) && ok) {
        ok = false;
        last_error = GetLastError();
    }
    if (!ok) {
        DeleteFileW(path.c_str());
        if (last_error == ERROR_SUCCESS) last_error = ERROR_WRITE_FAULT;
        return action_result(
            map_win32(last_error), hresult_from_win32(last_error));
    }
    return {};
}

inline DoctorStoreActionResult replace_file_atomic(
    const std::filesystem::path &path,
    std::string_view bytes,
    std::filesystem::path stage = {}) noexcept {
    const bool fixed_owned_stage = stage.empty();
    if (fixed_owned_stage) { stage = path; stage += L".tmp"; }
    const DWORD stage_attributes = GetFileAttributesW(stage.c_str());
    if (stage_attributes != INVALID_FILE_ATTRIBUTES) {
        if (!fixed_owned_stage)
            return action_result(DoctorStoreFailure::PermissionDenied,
                hresult_from_win32(ERROR_FILE_EXISTS));
        if ((stage_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return action_result(
                DoctorStoreFailure::PermissionDenied,
                static_cast<std::int32_t>(E_ACCESSDENIED));
        if (!DeleteFileW(stage.c_str())) {
            const DWORD error = GetLastError();
            return action_result(map_win32(error), hresult_from_win32(error));
        }
    }
    const auto written = write_file_exact(stage, bytes, false);
    if (!written.ok()) return written;
    const DWORD target_attributes = GetFileAttributesW(path.c_str());
    if (target_attributes != INVALID_FILE_ATTRIBUTES &&
        (target_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        DeleteFileW(stage.c_str());
        return action_result(
            DoctorStoreFailure::PermissionDenied,
            static_cast<std::int32_t>(E_ACCESSDENIED));
    }
    if (!MoveFileExW(
        stage.c_str(),
        path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(stage.c_str());
        return action_result(map_win32(error), hresult_from_win32(error));
    }
    return {};
}

inline DoctorStoreActionResult append_file(
    const std::filesystem::path &path,
    std::string_view bytes) noexcept {
    if (!is_safe_regular(path)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return action_result(map_win32(error), hresult_from_win32(error));
        }
        return action_result(
            DoctorStoreFailure::IoError,
            static_cast<std::int32_t>(E_INVALIDARG));
    }
    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return action_result(map_win32(error), hresult_from_win32(error));
    }
    std::size_t offset = 0;
    bool ok = true;
    DWORD last_error = ERROR_SUCCESS;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7fffffffU ? 0x7fffffffU : remaining);
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            ok = false;
            last_error = GetLastError();
            break;
        }
        offset += written;
    }
    if (!CloseHandle(file) && ok) {
        ok = false;
        last_error = GetLastError();
    }
    if (!ok) {
        if (last_error == ERROR_SUCCESS) last_error = ERROR_WRITE_FAULT;
        return action_result(
            map_win32(last_error), hresult_from_win32(last_error));
    }
    return {};
}

inline DoctorStoreActionResult delete_owned_file(
    const std::filesystem::path &path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return action_result(map_win32(error), hresult_from_win32(error));
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return action_result(
            DoctorStoreFailure::PermissionDenied,
            static_cast<std::int32_t>(E_ACCESSDENIED));
    if (!DeleteFileW(path.c_str())) {
        const DWORD error = GetLastError();
        return action_result(map_win32(error), hresult_from_win32(error));
    }
    return {};
}

inline bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

inline bool parse_decimal_u64(
    std::string_view value,
    std::uint64_t *output) noexcept {
    if (value.empty() || value.size() > 20) return false;
    std::uint64_t result = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    *output = result;
    return true;
}

inline bool parse_record_name(
    const std::filesystem::path &path,
    std::uint64_t *ordinal) {
    const auto name = path.filename().string();
    constexpr std::string_view prefix = "doctor-record-";
    constexpr std::string_view suffix = ".jsonl";
    if (name.size() <= prefix.size() + suffix.size() ||
        !starts_with(name, prefix) ||
        name.compare(
            name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    const auto number = std::string_view(name).substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    if (number.empty() || (number.size() > 1 && number.front() == '0') ||
        !parse_decimal_u64(number, ordinal))
        return false;
    return *ordinal > 0;
}

inline bool parse_export_name(const std::filesystem::path &path) {
    const auto name = path.filename().string();
    constexpr std::string_view prefix = "doctor-export-";
    constexpr std::string_view suffix = ".zip";
    if (name.size() <= prefix.size() + suffix.size() ||
        !starts_with(name, prefix) ||
        name.compare(
            name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    const auto unique = std::string_view(name).substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    if (unique.empty()) return false;
    for (const char ch : unique) {
        if (!((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') ||
            ch == '-'))
            return false;
    }
    return true;
}

inline bool parse_export_stage_name(const std::filesystem::path &path) {
    auto name = path.filename().string();
    constexpr std::string_view suffix = ".tmp";
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    name.resize(name.size() - suffix.size());
    return parse_export_name(std::filesystem::path(name));
}

inline bool parse_record_stage_name(const std::filesystem::path &path) {
    auto name = path.filename().string();
    constexpr std::string_view suffix = ".tmp";
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    name.resize(name.size() - suffix.size());
    std::uint64_t ignored = 0;
    return parse_record_name(std::filesystem::path(name), &ignored);
}

inline bool store_failure_valid(DoctorStoreFailure failure) {
    return static_cast<std::uint32_t>(failure) <=
        static_cast<std::uint32_t>(DoctorStoreFailure::RecordTooLarge);
}

struct ControlState {
    bool enabled = false;
    bool pending_clear = false;
    std::int64_t epoch = 0;
    DoctorStoreFailure failure = DoctorStoreFailure::None;
    std::int32_t platform_result_code = 0;
    std::uint64_t sequence_gap = 0;
    std::int64_t export_created_ms = 0;
    std::string active_export_name;
};

inline std::string encode_control(const ControlState &control) {
    std::string bytes;
    bytes.reserve(128);
    append_u32(&bytes, kControlMagic);
    append_u32(&bytes, kControlVersion);
    append_u32(&bytes,
        (control.enabled ? 1U : 0U) |
        (control.pending_clear ? 2U : 0U));
    append_i64(&bytes, control.epoch);
    append_u32(&bytes, static_cast<std::uint32_t>(control.failure));
    append_i32(&bytes, control.platform_result_code);
    append_i64(&bytes, control.export_created_ms);
    append_u32(&bytes, static_cast<std::uint32_t>(
        control.sequence_gap & 0xffffffffULL));
    append_u32(&bytes, static_cast<std::uint32_t>(
        control.sequence_gap >> 32));
    append_u32(&bytes, static_cast<std::uint32_t>(
        control.active_export_name.size()));
    bytes += control.active_export_name;
    append_u32(&bytes, crc32(bytes));
    return bytes;
}

inline bool decode_control(
    std::string_view bytes,
    ControlState *control) noexcept {
    if (bytes.size() < 52 || bytes.size() > kMaximumControlBytes) return false;
    std::uint32_t magic = 0, version = 0, flags = 0;
    std::uint32_t failure = 0;
    std::int32_t platform_result = 0;
    std::uint32_t gap_low = 0, gap_high = 0, name_size = 0, stored_crc = 0;
    std::int64_t epoch = 0, export_created = 0;
    if (!read_u32(bytes, 0, &magic) ||
        !read_u32(bytes, 4, &version) ||
        !read_u32(bytes, 8, &flags) ||
        !read_i64(bytes, 12, &epoch) ||
        !read_u32(bytes, 20, &failure) ||
        !read_i32(bytes, 24, &platform_result) ||
        !read_i64(bytes, 28, &export_created) ||
        !read_u32(bytes, 36, &gap_low) ||
        !read_u32(bytes, 40, &gap_high) ||
        !read_u32(bytes, 44, &name_size) ||
        magic != kControlMagic ||
        version != kControlVersion ||
        (flags & ~3U) != 0 ||
        epoch < 0 ||
        failure > static_cast<std::uint32_t>(
            DoctorStoreFailure::RecordTooLarge) ||
        export_created < 0 ||
        name_size > kMaximumControlBytes ||
        bytes.size() != 52 + static_cast<std::size_t>(name_size) ||
        !read_u32(bytes, 48 + name_size, &stored_crc) ||
        stored_crc != crc32(bytes.substr(0, 48 + name_size)))
        return false;
    const std::string name(
        bytes.data() + 48, bytes.data() + 48 + name_size);
    if (!name.empty() &&
        !parse_export_name(std::filesystem::path(name)))
        return false;
    const auto failure_enum = static_cast<DoctorStoreFailure>(failure);
    if (!store_failure_valid(failure_enum)) return false;
    control->enabled = (flags & 1U) != 0;
    control->pending_clear = (flags & 2U) != 0;
    control->epoch = epoch;
    control->failure = failure_enum;
    control->platform_result_code = platform_result;
    control->export_created_ms = export_created;
    control->sequence_gap =
        static_cast<std::uint64_t>(gap_low) |
        (static_cast<std::uint64_t>(gap_high) << 32);
    control->active_export_name = name;
    return true;
}

struct SegmentInfo {
    std::filesystem::path path;
    std::uint64_t ordinal = 0;
    std::uint64_t bytes = 0;
    std::uint64_t count = 0;
    std::int64_t min_wall_ms =
        std::numeric_limits<std::int64_t>::max();
    std::int64_t max_wall_ms = 0;
};

struct StreamId {
    std::int64_t epoch = 0;
    std::int64_t producer_id = 0;
    std::int64_t session_id = 0;

    bool operator<(const StreamId &right) const noexcept {
        if (epoch != right.epoch) return epoch < right.epoch;
        if (producer_id != right.producer_id)
            return producer_id < right.producer_id;
        return session_id < right.session_id;
    }
};

struct ZipEntry {
    std::string name;
    std::string bytes;
};

inline std::string make_zip(const std::vector<ZipEntry> &entries) {
    std::string archive;
    if (entries.size() > kMaximumZipEntries) return archive;
    std::vector<std::uint32_t> offsets;
    offsets.reserve(entries.size());
    std::uint64_t total = 0;
    for (const auto &entry : entries) {
        total += 30 + entry.name.size() + entry.bytes.size() + 46;
        if (total > std::numeric_limits<std::uint32_t>::max())
            return {};
    }
    archive.reserve(static_cast<std::size_t>(total + 22));
    for (const auto &entry : entries) {
        offsets.push_back(static_cast<std::uint32_t>(archive.size()));
        append_u32(&archive, kZipLocalSignature);
        append_u16(&archive, 20);
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u16(&archive, kZipDosEpochDate);
        const auto checksum = crc32(entry.bytes);
        append_u32(&archive, checksum);
        append_u32(&archive, static_cast<std::uint32_t>(entry.bytes.size()));
        append_u32(&archive, static_cast<std::uint32_t>(entry.bytes.size()));
        append_u16(&archive, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(&archive, 0);
        archive += entry.name;
        archive += entry.bytes;
    }
    const auto central_offset = static_cast<std::uint32_t>(archive.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        append_u32(&archive, kZipCentralSignature);
        append_u16(&archive, 20);
        append_u16(&archive, 20);
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u16(&archive, kZipDosEpochDate);
        append_u32(&archive, crc32(entry.bytes));
        append_u32(&archive, static_cast<std::uint32_t>(entry.bytes.size()));
        append_u32(&archive, static_cast<std::uint32_t>(entry.bytes.size()));
        append_u16(&archive, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u16(&archive, 0);
        append_u32(&archive, 0);
        append_u32(&archive, offsets[index]);
        archive += entry.name;
    }
    const auto central_size =
        static_cast<std::uint32_t>(archive.size() - central_offset);
    append_u32(&archive, kZipEndSignature);
    append_u16(&archive, 0);
    append_u16(&archive, 0);
    append_u16(&archive, static_cast<std::uint16_t>(entries.size()));
    append_u16(&archive, static_cast<std::uint16_t>(entries.size()));
    append_u32(&archive, central_size);
    append_u32(&archive, central_offset);
    append_u16(&archive, 0);
    return archive;
}

inline bool read_zip(
    std::string_view archive,
    std::map<std::string, std::string> *entries) {
    entries->clear();
    if (archive.size() < 22) return false;
    std::size_t end = archive.size() - 22;
    std::uint32_t signature = 0;
    if (!read_u32(archive, end, &signature) ||
        signature != kZipEndSignature)
        return false;
    std::uint16_t disk = 0, central_disk = 0, disk_count = 0, count = 0;
    std::uint16_t comment_size = 0;
    std::uint32_t central_size = 0, central_offset = 0;
    if (!read_u16(archive, end + 4, &disk) ||
        !read_u16(archive, end + 6, &central_disk) ||
        !read_u16(archive, end + 8, &disk_count) ||
        !read_u16(archive, end + 10, &count) ||
        !read_u32(archive, end + 12, &central_size) ||
        !read_u32(archive, end + 16, &central_offset) ||
        !read_u16(archive, end + 20, &comment_size) ||
        disk != 0 || central_disk != 0 ||
        disk_count != count ||
        count != kMaximumZipEntries ||
        comment_size != 0 ||
        static_cast<std::uint64_t>(central_offset) + central_size != end)
        return false;
    std::size_t cursor = central_offset;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (cursor > archive.size() || archive.size() - cursor < 46 ||
            !read_u32(archive, cursor, &signature) ||
            signature != kZipCentralSignature)
            return false;
        std::uint16_t method = 0, name_size = 0, extra_size = 0;
        std::uint16_t entry_comment_size = 0;
        std::uint32_t checksum = 0, compressed = 0, uncompressed = 0;
        std::uint32_t local_offset = 0, local_signature = 0;
        std::uint16_t local_method = 0, local_name_size = 0;
        std::uint16_t local_extra_size = 0;
        if (!read_u16(archive, cursor + 10, &method) ||
            !read_u32(archive, cursor + 16, &checksum) ||
            !read_u32(archive, cursor + 20, &compressed) ||
            !read_u32(archive, cursor + 24, &uncompressed) ||
            !read_u16(archive, cursor + 28, &name_size) ||
            !read_u16(archive, cursor + 30, &extra_size) ||
            !read_u16(archive, cursor + 32, &entry_comment_size) ||
            !read_u32(archive, cursor + 42, &local_offset) ||
            method != 0 || compressed != uncompressed ||
            cursor + 46 + name_size + extra_size + entry_comment_size >
                archive.size() ||
            local_offset > archive.size() ||
            archive.size() - local_offset < 30 ||
            !read_u32(archive, local_offset, &local_signature) ||
            local_signature != kZipLocalSignature ||
            !read_u16(archive, local_offset + 8, &local_method) ||
            !read_u16(archive, local_offset + 26, &local_name_size) ||
            !read_u16(archive, local_offset + 28, &local_extra_size) ||
            local_method != 0 ||
            local_name_size != name_size)
            return false;
        const std::string name(
            archive.data() + cursor + 46, name_size);
        const std::size_t data_offset =
            static_cast<std::size_t>(local_offset) +
            30 + local_name_size + local_extra_size;
        if (data_offset > archive.size() ||
            archive.size() - data_offset < compressed ||
            std::string_view(
                archive.data() + local_offset + 30,
                local_name_size) != name)
            return false;
        const std::string bytes(
            archive.data() + data_offset, compressed);
        if (crc32(bytes) != checksum) return false;
        if (!entries->emplace(name, bytes).second) return false;
        cursor += 46 + name_size + extra_size + entry_comment_size;
    }
    return cursor == central_offset + central_size;
}

struct DoctorExportManifest {
    DoctorBuildMetadata build;
    std::int64_t epoch = 0;
    std::int64_t created_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::uint64_t record_count = 0;
    std::int64_t oldest_retained_ms = 0;
    std::int64_t newest_retained_ms = 0;
    std::uint64_t ignored_lines = 0;
    std::uint64_t sequence_gap = 0;
};

struct DoctorExportMark {
    std::int64_t epoch = 0;
    std::int64_t action_time_ms = 0;
    std::int64_t selected_issue_time_ms = 0;
    std::int64_t before_ms = 0;
    std::int64_t after_ms = 0;
};

struct ParsedDoctorExport {
    DoctorExportManifest manifest;
    DoctorExportMark mark;
    std::vector<DoctorEventEnvelope> events;
};

struct LoadedDoctorExport {
    std::string archive;
    ParsedDoctorExport parsed;
    std::int64_t window_begin_ms = 0;
    std::int64_t window_end_ms = 0;
};

inline bool parse_fixed_object(
    std::string_view text,
    const std::set<std::string_view> &expected,
    std::map<std::string, doctor_event_detail::JsonValue> *output) {
    std::vector<std::pair<std::string, doctor_event_detail::JsonValue>> fields;
    DoctorEventFailure failure = DoctorEventFailure::InvalidJson;
    if (!doctor_event_detail::parse_object(text, &fields, &failure) ||
        fields.size() != expected.size())
        return false;
    output->clear();
    for (const auto &field : fields) {
        if (expected.count(field.first) == 0 ||
            !output->emplace(field.first, field.second).second)
            return false;
    }
    return true;
}

inline bool parse_string_field(
    const std::map<std::string, doctor_event_detail::JsonValue> &fields,
    std::string_view name,
    std::string *output) {
    const auto found = fields.find(std::string(name));
    if (found == fields.end() ||
        found->second.type != doctor_event_detail::JsonType::String)
        return false;
    output->assign(found->second.text.data(), found->second.text.size());
    return true;
}

inline bool parse_integer_field(
    const std::map<std::string, doctor_event_detail::JsonValue> &fields,
    std::string_view name,
    std::int64_t *output) {
    const auto found = fields.find(std::string(name));
    return found != fields.end() &&
        found->second.type == doctor_event_detail::JsonType::Integer &&
        doctor_event_detail::parse_signed(found->second.text, output);
}

inline bool parse_count_field(
    const std::map<std::string, doctor_event_detail::JsonValue> &fields,
    std::string_view name,
    std::uint64_t *output) {
    std::int64_t value = 0;
    if (!parse_integer_field(fields, name, &value) || value < 0)
        return false;
    *output = static_cast<std::uint64_t>(value);
    return true;
}

inline bool parse_export_manifest(
    std::string_view text,
    DoctorExportManifest *output) {
    static const std::set<std::string_view> expected{
        "schemaVersion",
        "version",
        "sourceCommit",
        "epoch",
        "createdAtMs",
        "expiresAtMs",
        "recordCount",
        "oldestRetainedMs",
        "newestRetainedMs",
        "ignoredLines",
        "sequenceGap",
    };
    std::map<std::string, doctor_event_detail::JsonValue> fields;
    std::int64_t schema_version = 0;
    DoctorExportManifest parsed;
    if (!parse_fixed_object(text, expected, &fields) ||
        !parse_integer_field(fields, "schemaVersion", &schema_version) ||
        schema_version != kDoctorSchemaVersion ||
        !parse_string_field(fields, "version", &parsed.build.version) ||
        !parse_string_field(
            fields, "sourceCommit", &parsed.build.source_commit) ||
        !doctor_event_detail::valid_build(parsed.build) ||
        !parse_integer_field(fields, "epoch", &parsed.epoch) ||
        !parse_integer_field(fields, "createdAtMs", &parsed.created_at_ms) ||
        !parse_integer_field(fields, "expiresAtMs", &parsed.expires_at_ms) ||
        !parse_count_field(fields, "recordCount", &parsed.record_count) ||
        !parse_integer_field(
            fields, "oldestRetainedMs", &parsed.oldest_retained_ms) ||
        !parse_integer_field(
            fields, "newestRetainedMs", &parsed.newest_retained_ms) ||
        !parse_count_field(fields, "ignoredLines", &parsed.ignored_lines) ||
        !parse_count_field(fields, "sequenceGap", &parsed.sequence_gap) ||
        parsed.epoch < 0 ||
        parsed.created_at_ms < 0 ||
        parsed.expires_at_ms < parsed.created_at_ms)
        return false;
    if (parsed.record_count == 0) {
        if (parsed.oldest_retained_ms != 0 ||
            parsed.newest_retained_ms != 0)
            return false;
    } else if (parsed.oldest_retained_ms < 0 ||
        parsed.newest_retained_ms < parsed.oldest_retained_ms) {
        return false;
    }
    *output = std::move(parsed);
    return true;
}

inline bool parse_export_mark(
    std::string_view text,
    DoctorExportMark *output) {
    static const std::set<std::string_view> expected{
        "schemaVersion",
        "epoch",
        "actionTimeMs",
        "selectedIssueTimeMs",
        "beforeMs",
        "afterMs",
    };
    std::map<std::string, doctor_event_detail::JsonValue> fields;
    std::int64_t schema_version = 0;
    DoctorExportMark parsed;
    if (!parse_fixed_object(text, expected, &fields) ||
        !parse_integer_field(fields, "schemaVersion", &schema_version) ||
        schema_version != kDoctorSchemaVersion ||
        !parse_integer_field(fields, "epoch", &parsed.epoch) ||
        !parse_integer_field(
            fields, "actionTimeMs", &parsed.action_time_ms) ||
        !parse_integer_field(
            fields, "selectedIssueTimeMs", &parsed.selected_issue_time_ms) ||
        !parse_integer_field(fields, "beforeMs", &parsed.before_ms) ||
        !parse_integer_field(fields, "afterMs", &parsed.after_ms) ||
        parsed.epoch < 0 ||
        parsed.action_time_ms < 0 ||
        parsed.selected_issue_time_ms < 0 ||
        parsed.before_ms < 0 ||
        parsed.after_ms < 0)
        return false;
    *output = std::move(parsed);
    return true;
}

inline bool parse_export_archive(
    std::string_view archive,
    ParsedDoctorExport *output) {
    std::map<std::string, std::string> entries;
    if (!read_zip(archive, &entries) ||
        entries.size() != kMaximumZipEntries ||
        entries.count("doctor/manifest-v1.json") != 1 ||
        entries.count("doctor/mark-v1.json") != 1 ||
        entries.count("doctor/events-v1.jsonl") != 1)
        return false;

    ParsedDoctorExport parsed;
    if (!parse_export_manifest(
            entries.at("doctor/manifest-v1.json"),
            &parsed.manifest) ||
        !parse_export_mark(
            entries.at("doctor/mark-v1.json"), &parsed.mark))
        return false;

    const auto &events = entries.at("doctor/events-v1.jsonl");
    if ((!events.empty() && events.back() != '\n') ||
        events.find('\r') != std::string::npos)
        return false;
    const auto decoded = DoctorEventCodec::decode_jsonl(events);
    if (decoded.failure != DoctorEventFailure::None ||
        parsed.manifest.record_count != decoded.events.size())
        return false;

    std::int64_t oldest = 0, newest = 0;
    if (!decoded.events.empty()) {
        oldest = decoded.events.front().event.wall_utc_ms;
        newest = oldest;
        for (const auto &envelope : decoded.events) {
            oldest = std::min(oldest, envelope.event.wall_utc_ms);
            newest = std::max(newest, envelope.event.wall_utc_ms);
        }
    }
    if (parsed.manifest.oldest_retained_ms != oldest ||
        parsed.manifest.newest_retained_ms != newest)
        return false;
    parsed.events = decoded.events;
    *output = std::move(parsed);
    return true;
}

inline bool add_saturating(
    std::int64_t left,
    std::int64_t right,
    std::int64_t *output) noexcept {
    if (right > 0 &&
        left > std::numeric_limits<std::int64_t>::max() - right)
        return false;
    if (right < 0 &&
        left < std::numeric_limits<std::int64_t>::min() - right)
        return false;
    *output = left + right;
    return true;
}

inline std::int64_t saturating_add(
    std::int64_t left,
    std::int64_t right) noexcept {
    if (right > 0 &&
        left > std::numeric_limits<std::int64_t>::max() - right)
        return std::numeric_limits<std::int64_t>::max();
    if (right < 0 &&
        left < std::numeric_limits<std::int64_t>::min() - right)
        return std::numeric_limits<std::int64_t>::min();
    return left + right;
}

} // namespace doctor_store_detail

class DoctorStore {
public:
    DoctorStore(const DoctorStore &) = delete;
    DoctorStore &operator=(const DoctorStore &) = delete;
    DoctorStore(DoctorStore &&) = delete;
    DoctorStore &operator=(DoctorStore &&) = delete;

    static std::unique_ptr<DoctorStore> open(
        const std::filesystem::path &root,
        const DoctorBuildMetadata &build,
        const DoctorClock &clock,
        DoctorStoreLimits limits = DoctorStoreLimits::defaults(),
        DoctorIoFaults *io_faults = nullptr) {
        return std::unique_ptr<DoctorStore>(
            new DoctorStore(root, build, clock, limits, io_faults));
    }

    DoctorStoreState status() const {
        DoctorStoreState result;
        result.enabled = control_.enabled;
        result.pending_clear = control_.pending_clear;
        result.epoch = control_.epoch;
        result.failure = runtime_failure_;
        result.platform_result_code = runtime_platform_result_;
        result.raw_bytes = raw_bytes_;
        result.temp_bytes = temp_bytes_;
        result.sequence_gap = control_.sequence_gap + observed_gap_;
        result.ignored_lines = ignored_lines_;
        result.collecting = control_.enabled &&
            !control_.pending_clear &&
            result.failure == DoctorStoreFailure::None;
        if (!control_.pending_clear && result.failure != DoctorStoreFailure::EpochCommitFailed) {
            for (const auto &segment : segments_) {
                if (!segment.count) continue;
                result.oldest_retained_ms = result.has_retained_range
                    ? std::min(result.oldest_retained_ms, segment.min_wall_ms) : segment.min_wall_ms;
                result.newest_retained_ms = result.has_retained_range
                    ? std::max(result.newest_retained_ms, segment.max_wall_ms) : segment.max_wall_ms;
                result.has_retained_range = true;
            }
        }
        return result;
    }

    DoctorStoreActionResult enable() {
        const auto guard = operation_guard();
        if (!guard.ok()) return guard;
        if (control_.enabled && !control_.pending_clear) {
            clear_runtime_failure();
            return {};
        }
        if (!doctor_store_detail::ensure_tree(root_)) {
            const DWORD error = GetLastError();
            return set_runtime_failure(
                doctor_store_detail::map_win32(error),
                doctor_store_detail::hresult_from_win32(error));
        }
        auto next = control_;
        next.enabled = true;
        next.failure = DoctorStoreFailure::None;
        next.platform_result_code = 0;
        const auto written = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!written.ok()) return set_runtime_failure(written);
        control_ = std::move(next);
        clear_runtime_failure();
        rebuild_metadata();
        return {};
    }

    DoctorStoreActionResult disable() {
        const auto guard = operation_guard();
        if (!guard.ok()) return guard;
        auto next = control_;
        next.enabled = false;
        next.failure = DoctorStoreFailure::None;
        next.platform_result_code = 0;
        if (!doctor_store_detail::ensure_tree(root_)) {
            const DWORD error = GetLastError();
            return set_runtime_failure(
                doctor_store_detail::map_win32(error),
                doctor_store_detail::hresult_from_win32(error));
        }
        const auto written = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!written.ok()) return set_runtime_failure(written);
        control_ = std::move(next);
        clear_runtime_failure();
        return {};
    }

    DoctorStoreActionResult append(const DoctorEventRecord &event) {
        if (!control_.enabled || control_.pending_clear)
            return doctor_store_detail::action_result(
                DoctorStoreFailure::Disabled);
        const auto guard = operation_guard();
        if (!guard.ok()) return guard;
        if (event.epoch != control_.epoch)
            return doctor_store_detail::action_result(
                DoctorStoreFailure::EpochMismatch);
        const auto encoded =
            DoctorEventCodec::encode_line(build_, event);
        if (encoded.failure != DoctorEventFailure::None) {
            const auto is_privacy =
                encoded.failure ==
                    DoctorEventFailure::RestrictedInputEvent ||
                encoded.failure ==
                    DoctorEventFailure::RestrictedCorrelation ||
                encoded.failure ==
                    DoctorEventFailure::RestrictedCountField ||
                encoded.failure ==
                    DoctorEventFailure::RestrictedSequenceGap;
            return set_runtime_result(
                is_privacy
                    ? DoctorStoreFailure::PrivacyRejected
                    : DoctorStoreFailure::InvalidSchema);
        }
        std::string line = encoded.line;
        line.push_back('\n');
        if (line.size() > limits_.max_record_bytes)
            return set_runtime_result(DoctorStoreFailure::RecordTooLarge);

        const auto cleanup = enforce_retention();
        if (!cleanup.ok()) return set_runtime_failure(cleanup);

        const bool rotate =
            active_segment_index_ >= segments_.size() ||
            segments_[active_segment_index_].bytes >
                limits_.max_record_bytes ||
            line.size() >
                limits_.max_record_bytes -
                    segments_[active_segment_index_].bytes;
        if (rotate) {
            const auto rotated = rotate_segment();
            if (!rotated.ok()) return set_runtime_failure(rotated);
        }
        if (active_segment_index_ >= segments_.size())
            return set_runtime_failure(
                doctor_store_detail::action_result(
                    DoctorStoreFailure::IoError,
                    static_cast<std::int32_t>(E_UNEXPECTED)));

        const auto fault = inject(DoctorIoOperation::AppendRecord);
        if (!fault.ok()) return set_runtime_failure(fault);
        const auto appended = doctor_store_detail::append_file(
            segments_[active_segment_index_].path, line);
        if (!appended.ok()) return set_runtime_failure(appended);

        auto &segment = segments_[active_segment_index_];
        segment.bytes += line.size();
        ++segment.count;
        segment.min_wall_ms =
            std::min(segment.min_wall_ms, event.wall_utc_ms);
        segment.max_wall_ms =
            std::max(segment.max_wall_ms, event.wall_utc_ms);
        raw_bytes_ += line.size();
        update_sequence_gap(event);
        clear_runtime_failure();

        const auto capped = enforce_raw_byte_cap();
        if (!capped.ok()) return set_runtime_failure(capped);
        return {};
    }

    DoctorStoreResult<std::vector<DoctorEventRecord>> read_records() const {
        return read_records(control_.epoch);
    }

    DoctorStoreResult<std::vector<DoctorEventRecord>> read_records(
        std::int64_t epoch) const {
        if (epoch != control_.epoch)
            return doctor_store_detail::store_result<
                std::vector<DoctorEventRecord>>(
                    DoctorStoreFailure::EpochMismatch);
        const auto guard = operation_guard();
        if (!guard.ok()) return {{}, guard.failure, guard.platform_result_code};
        const auto cleanup = enforce_retention();
        if (!cleanup.ok())
            return DoctorStoreResult<std::vector<DoctorEventRecord>>{
                {}, cleanup.failure, cleanup.platform_result_code};
        return scan_records(true);
    }

    DoctorStoreActionResult clear() {
        if (control_.pending_clear) {
            complete_pending_clear();
            return control_.pending_clear
                ? doctor_store_detail::action_result(DoctorStoreFailure::ClearIncomplete, runtime_platform_result_)
                : DoctorStoreActionResult{};
        }
        const auto fail_closed = [this](const DoctorStoreActionResult &failure) {
            control_.failure = DoctorStoreFailure::EpochCommitFailed;
            control_.platform_result_code = failure.platform_result_code;
            set_runtime_failure(DoctorStoreFailure::EpochCommitFailed, failure.platform_result_code);
            return failure;
        };
        if (control_.epoch == std::numeric_limits<std::int64_t>::max())
            return fail_closed(
                doctor_store_detail::action_result(
                    DoctorStoreFailure::EpochCommitFailed,
                    static_cast<std::int32_t>(E_BOUNDS)));
        if (!doctor_store_detail::ensure_tree(root_)) {
            const DWORD observed = GetLastError();
            const DWORD error = observed == ERROR_SUCCESS ? ERROR_CANNOT_MAKE : observed;
            return fail_closed(doctor_store_detail::action_result(
                doctor_store_detail::map_win32(error),
                doctor_store_detail::hresult_from_win32(error)));
        }

        const auto old_export = active_export_path();
        const bool enabled_after_clear = control_.enabled;
        auto stopped = control_;
        stopped.enabled = false;
        stopped.failure = DoctorStoreFailure::EpochCommitFailed;
        stopped.platform_result_code = 0;
        // A durable intent keeps old history inaccessible after a failed epoch
        // write even if writing the subsequent failure record also fails.
        const auto intent = persist_control(DoctorIoOperation::PersistControl, stopped);
        if (!intent.ok()) {
            // No intent committed: preserve consent for retry without exposing
            // the old records or allowing more collection in this writer.
            return fail_closed(intent);
        }
        control_ = stopped;
        auto next = control_;
        next.enabled = enabled_after_clear;
        ++next.epoch;
        next.pending_clear = true;
        next.active_export_name.clear();
        next.export_created_ms = 0;
        next.failure = DoctorStoreFailure::None;
        next.platform_result_code = 0;
        const auto committed = persist_control(
            DoctorIoOperation::CommitEpoch, next);
        if (!committed.ok()) {
            auto failed = control_;
            failed.enabled = false;
            failed.pending_clear = false;
            failed.failure = DoctorStoreFailure::EpochCommitFailed;
            failed.platform_result_code = committed.platform_result_code;
            const auto fail_closed = persist_control_unchecked(failed);
            control_ = failed;
            runtime_failure_ = DoctorStoreFailure::EpochCommitFailed;
            runtime_platform_result_ =
                committed.platform_result_code;
            if (!fail_closed.ok())
                runtime_platform_result_ =
                    fail_closed.platform_result_code;
            return doctor_store_detail::action_result(
                DoctorStoreFailure::EpochCommitFailed,
                committed.platform_result_code);
        }
        control_ = next;
        runtime_failure_ = DoctorStoreFailure::None;
        runtime_platform_result_ = 0;
        temp_bytes_ = 0;

        if (!old_export.empty()) {
            const auto removed = delete_with_fault(old_export);
            if (!removed.ok()) {
                remember_clear_incomplete(removed);
                return doctor_store_detail::action_result(
                    DoctorStoreFailure::ClearIncomplete,
                    removed.platform_result_code);
            }
        }
        if (!delete_all_owned_records() ||
            !delete_all_owned_exports()) {
            DoctorStoreActionResult failure{
                DoctorStoreFailure::ClearIncomplete,
                runtime_platform_result_};
            remember_clear_incomplete(failure);
            return failure;
        }

        next = control_;
        next.pending_clear = false;
        next.failure = DoctorStoreFailure::None;
        next.platform_result_code = 0;
        next.sequence_gap = 0;
        const auto completed = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!completed.ok()) {
            const DoctorStoreActionResult incomplete{
                DoctorStoreFailure::ClearIncomplete,
                completed.platform_result_code};
            remember_clear_incomplete(incomplete);
            return incomplete;
        }
        control_ = std::move(next);
        segments_.clear();
        active_segment_index_ = std::numeric_limits<std::size_t>::max();
        active_ordinal_ = 0;
        raw_bytes_ = 0;
        temp_bytes_ = 0;
        observed_gap_ = 0;
        ignored_lines_ = 0;
        last_sequences_.clear();
        clear_runtime_failure();
        return {};
    }

    DoctorStoreResult<DoctorIssueMark> mark_issue(
        std::int64_t selected_issue_time_ms,
        DoctorExportWindow window = DoctorExportWindow{300000, 30000}) {
        const auto guard = operation_guard();
        if (!guard.ok()) return {{}, guard.failure, guard.platform_result_code};
        if (selected_issue_time_ms < 0 ||
            window.before_ms < 0 || window.after_ms < 0)
            return doctor_store_detail::store_result<DoctorIssueMark>(
                DoctorStoreFailure::InvalidSchema);
        DoctorIssueMark mark;
        mark.action_time_ms = clock_.wall_utc_ms();
        if (mark.action_time_ms < 0)
            return doctor_store_detail::store_result<DoctorIssueMark>(
                DoctorStoreFailure::InvalidSchema);
        mark.epoch = control_.epoch;
        mark.selected_issue_time_ms = selected_issue_time_ms;
        mark.window = window;
        return DoctorStoreResult<DoctorIssueMark>{
            mark, DoctorStoreFailure::None, 0};
    }

    DoctorStoreResult<DoctorExportSnapshot> prepare_export(
        const DoctorIssueMark &mark) {
        const auto guard = operation_guard();
        if (!guard.ok()) return {{}, guard.failure, guard.platform_result_code};
        if (mark.action_time_ms < 0 ||
            mark.selected_issue_time_ms < 0 ||
            mark.window.before_ms < 0 ||
            mark.window.after_ms < 0)
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::InvalidSchema);
        if (mark.epoch != control_.epoch)
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::EpochMismatch);
        const auto expiration = expire_active_export();
        if (!expiration.ok())
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, expiration.failure, expiration.platform_result_code};
        if (!control_.active_export_name.empty())
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::ExportExists);

        const auto retained = read_record_envelopes(control_.epoch);
        if (!retained.ok())
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, retained.failure, retained.platform_result_code};

        std::int64_t begin = 0, end = 0;
        if (!doctor_store_detail::add_saturating(
            mark.selected_issue_time_ms, -mark.window.before_ms, &begin) ||
            !doctor_store_detail::add_saturating(
                mark.selected_issue_time_ms, mark.window.after_ms, &end) ||
            end < begin) {
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::InvalidSchema);
        }
        begin = std::max<std::int64_t>(0, begin);

        std::string events;
        std::int64_t oldest = 0, newest = 0;
        std::uint64_t record_count = 0;
        for (const auto &envelope : retained.value) {
            const auto &record = envelope.event;
            if (record.wall_utc_ms < begin || record.wall_utc_ms > end)
                continue;
            const auto encoded =
                DoctorEventCodec::encode_line(envelope.build, record);
            if (encoded.failure != DoctorEventFailure::None)
                return doctor_store_detail::store_result<
                    DoctorExportSnapshot>(
                        DoctorStoreFailure::InvalidSchema);
            events += encoded.line;
            events.push_back('\n');
            if (record_count == 0) {
                oldest = record.wall_utc_ms;
                newest = record.wall_utc_ms;
            } else {
                oldest = std::min(oldest, record.wall_utc_ms);
                newest = std::max(newest, record.wall_utc_ms);
            }
            ++record_count;
        }

        const auto created_at_ms = clock_.wall_utc_ms();
        if (created_at_ms < 0 || limits_.export_ttl_ms < 0)
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::InvalidSchema);
        const auto expires_at_ms = doctor_store_detail::saturating_add(
            created_at_ms, limits_.export_ttl_ms);
        std::string manifest;
        manifest += "{\"schemaVersion\":1,\"version\":\"";
        manifest += build_.version;
        manifest += "\",\"sourceCommit\":\"";
        manifest += build_.source_commit;
        manifest += "\",\"epoch\":";
        manifest += std::to_string(control_.epoch);
        manifest += ",\"createdAtMs\":";
        manifest += std::to_string(created_at_ms);
        manifest += ",\"expiresAtMs\":";
        manifest += std::to_string(expires_at_ms);
        manifest += ",\"recordCount\":";
        manifest += std::to_string(record_count);
        manifest += ",\"oldestRetainedMs\":";
        manifest += std::to_string(oldest);
        manifest += ",\"newestRetainedMs\":";
        manifest += std::to_string(newest);
        manifest += ",\"ignoredLines\":";
        manifest += std::to_string(ignored_lines_);
        manifest += ",\"sequenceGap\":";
        manifest += std::to_string(
            control_.sequence_gap + observed_gap_);
        manifest += '}';
        std::string mark_bytes;
        mark_bytes += "{\"schemaVersion\":1,\"epoch\":";
        mark_bytes += std::to_string(mark.epoch);
        mark_bytes += ",\"actionTimeMs\":";
        mark_bytes += std::to_string(mark.action_time_ms);
        mark_bytes += ",\"selectedIssueTimeMs\":";
        mark_bytes += std::to_string(mark.selected_issue_time_ms);
        mark_bytes += ",\"beforeMs\":";
        mark_bytes += std::to_string(mark.window.before_ms);
        mark_bytes += ",\"afterMs\":";
        mark_bytes += std::to_string(mark.window.after_ms);
        mark_bytes += '}';

        const std::string archive = doctor_store_detail::make_zip({
            {"doctor/manifest-v1.json", manifest},
            {"doctor/mark-v1.json", mark_bytes},
            {"doctor/events-v1.jsonl", events},
        });
        if (archive.empty())
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_OUTOFMEMORY));
        if (archive.size() > limits_.max_export_bytes)
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::ExportLimit);
        if (raw_bytes_ > limits_.max_raw_and_temp_bytes ||
            archive.size() >
                limits_.max_raw_and_temp_bytes - raw_bytes_)
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::TotalLimit);

        const auto fault = inject(DoctorIoOperation::CreateExport);
        if (!fault.ok())
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, fault.failure, fault.platform_result_code};
        auto snapshot_path = next_export_path();
        if (snapshot_path.empty())
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_UNEXPECTED));
        auto stage = snapshot_path;
        stage += L".tmp";
        const auto created =
            doctor_store_detail::write_file_exact(stage, archive, false);
        if (!created.ok())
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, created.failure, created.platform_result_code};
        if (!MoveFileExW(
            stage.c_str(), snapshot_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(stage.c_str());
            return doctor_store_detail::store_result<DoctorExportSnapshot>(
                doctor_store_detail::map_win32(error),
                doctor_store_detail::hresult_from_win32(error));
        }

        auto next = control_;
        next.active_export_name = snapshot_path.filename().string();
        next.export_created_ms = created_at_ms;
        const auto activated = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!activated.ok()) {
            DeleteFileW(snapshot_path.c_str());
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, activated.failure, activated.platform_result_code};
        }
        control_ = std::move(next);
        temp_bytes_ = archive.size();
        return DoctorStoreResult<DoctorExportSnapshot>{
            DoctorExportSnapshot{control_.epoch, snapshot_path},
            DoctorStoreFailure::None,
            0};
    }

    DoctorStoreResult<DoctorExportPreview> preview_export(
        const DoctorExportSnapshot &snapshot) {
        const auto loaded = load_export_snapshot(snapshot);
        if (!loaded.ok())
            return DoctorStoreResult<DoctorExportPreview>{
                {}, loaded.failure, loaded.platform_result_code};
        const auto &archive = loaded.value.parsed;
        std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
        std::int64_t latest = 0;
        for (const auto &envelope : archive.events) {
            const auto &record = envelope.event;
            earliest = std::min(earliest, record.wall_utc_ms);
            latest = std::max(latest, record.wall_utc_ms);
        }
        DoctorExportPreview preview;
        preview.epoch = snapshot.epoch;
        preview.record_count = archive.events.size();
        preview.sequence_gap = archive.manifest.sequence_gap;
        if (archive.events.empty()) {
            preview.missing_before_ms =
                loaded.value.window_end_ms -
                loaded.value.window_begin_ms;
            preview.missing_after_ms = 0;
        } else {
            preview.missing_before_ms =
                earliest > loaded.value.window_begin_ms
                    ? earliest - loaded.value.window_begin_ms
                    : 0;
            preview.missing_after_ms =
                latest < loaded.value.window_end_ms
                    ? loaded.value.window_end_ms - latest
                    : 0;
        }
        return DoctorStoreResult<DoctorExportPreview>{
            preview, DoctorStoreFailure::None, 0};
    }

    DoctorStoreActionResult save_export(
        const DoctorExportSnapshot &snapshot,
        const std::filesystem::path &destination) {
        const auto loaded = load_export_snapshot(snapshot);
        if (!loaded.ok())
            return set_runtime_failure(doctor_store_detail::action_result(
                loaded.failure, loaded.platform_result_code));
        std::error_code identity_error;
        if (std::filesystem::equivalent(snapshot.archive, destination, identity_error))
            return doctor_store_detail::action_result(DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_INVALIDARG));
        if (!destination.has_parent_path())
            return doctor_store_detail::action_result(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_INVALIDARG));
        const auto fault = inject(DoctorIoOperation::SaveExport);
        if (!fault.ok()) return set_runtime_failure(fault);
        const auto parent = destination.parent_path();
        const DWORD parent_attributes = GetFileAttributesW(parent.c_str());
        if (parent_attributes == INVALID_FILE_ATTRIBUTES ||
            (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (parent_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return set_runtime_failure(
                doctor_store_detail::action_result(
                    DoctorStoreFailure::IoError,
                    static_cast<std::int32_t>(E_INVALIDARG)));
        auto stage = destination;
        stage += L".doctor-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(unique_counter()) + L".tmp";
        const auto written = doctor_store_detail::replace_file_atomic(
            destination, loaded.value.archive, stage);
        if (!written.ok()) return set_runtime_failure(written);
        const auto removed =
            doctor_store_detail::delete_owned_file(snapshot.archive);
        if (!removed.ok()) {
            return set_runtime_failure(removed);
        }
        auto next = control_;
        next.active_export_name.clear();
        next.export_created_ms = 0;
        const auto persisted = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!persisted.ok()) return set_runtime_failure(persisted);
        control_ = std::move(next);
        temp_bytes_ = 0;
        clear_runtime_failure();
        return {};
    }

    DoctorStoreActionResult cancel_export(
        const DoctorExportSnapshot &snapshot) {
        const auto valid = validate_snapshot(snapshot);
        if (!valid.ok()) {
            if (valid.failure == DoctorStoreFailure::ExportExpired)
                return {};
            if ((valid.failure == DoctorStoreFailure::IoError ||
                    valid.failure == DoctorStoreFailure::PermissionDenied) &&
                snapshot.epoch == control_.epoch &&
                snapshot.archive.lexically_normal() ==
                    active_export_path().lexically_normal()) {
                auto next = control_;
                next.active_export_name.clear();
                next.export_created_ms = 0;
                const auto persisted = persist_control(
                    DoctorIoOperation::PersistControl, next);
                if (!persisted.ok()) return set_runtime_failure(persisted);
                control_ = std::move(next);
                temp_bytes_ = 0;
                return {};
            }
            return valid;
        }
        const auto removed =
            doctor_store_detail::delete_owned_file(snapshot.archive);
        if (!removed.ok()) return set_runtime_failure(removed);
        auto next = control_;
        next.active_export_name.clear();
        next.export_created_ms = 0;
        const auto persisted = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!persisted.ok()) return set_runtime_failure(persisted);
        control_ = std::move(next);
        temp_bytes_ = 0;
        clear_runtime_failure();
        return {};
    }

    DoctorStoreActionResult cancel_active_export() {
        if (control_.active_export_name.empty()) return {};
        return cancel_export(DoctorExportSnapshot{
            control_.epoch, active_export_path()});
    }

private:
    DoctorStore(
        std::filesystem::path root,
        DoctorBuildMetadata build,
        const DoctorClock &clock,
        DoctorStoreLimits limits,
        DoctorIoFaults *io_faults)
        : root_(std::move(root)),
          build_(std::move(build)),
          clock_(clock),
          limits_(limits),
          io_faults_(io_faults),
          active_segment_index_(std::numeric_limits<std::size_t>::max()) {
        initialize();
    }

    using StreamId = doctor_store_detail::StreamId;
    using SegmentInfo = doctor_store_detail::SegmentInfo;

    void initialize() {
        if (!doctor_event_detail::valid_build(build_)) {
            runtime_failure_ = DoctorStoreFailure::InvalidSchema;
            return;
        }
        if (!doctor_store_detail::ensure_tree(root_)) {
            const DWORD error = GetLastError();
            runtime_failure_ = doctor_store_detail::map_win32(error);
            runtime_platform_result_ =
                doctor_store_detail::hresult_from_win32(error);
            return;
        }
        cleanup_staging();
        const auto control_path = root_ / L"doctor-control-v1.bin";
        const DWORD attributes = GetFileAttributesW(control_path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                runtime_failure_ = DoctorStoreFailure::PermissionDenied;
                runtime_platform_result_ =
                    static_cast<std::int32_t>(E_ACCESSDENIED);
                control_.enabled = false;
                return;
            }
            std::string bytes;
            const auto loaded = doctor_store_detail::read_file_bounded(
                control_path,
                doctor_store_detail::kMaximumControlBytes,
                &bytes);
            if (!loaded.ok() ||
                !doctor_store_detail::decode_control(bytes, &control_)) {
                control_ = doctor_store_detail::ControlState{};
                runtime_failure_ = DoctorStoreFailure::InvalidSchema;
                runtime_platform_result_ =
                    loaded.ok()
                        ? static_cast<std::int32_t>(E_INVALIDARG)
                        : loaded.platform_result_code;
                return;
            }
        }
        if (runtime_failure_ == DoctorStoreFailure::None &&
            !control_.pending_clear && control_.failure != DoctorStoreFailure::None) {
            runtime_failure_ = control_.failure;
            runtime_platform_result_ = control_.platform_result_code;
        }
        cleanup_orphan_exports();
        rebuild_metadata();
        if (runtime_failure_ == DoctorStoreFailure::None &&
            !control_.active_export_name.empty()) {
            refresh_temp_bytes();
        }
        if (control_.pending_clear) complete_pending_clear();
    }

    void clear_runtime_failure() noexcept {
        runtime_failure_ = DoctorStoreFailure::None;
        runtime_platform_result_ = 0;
    }

    DoctorStoreActionResult set_runtime_result(
        DoctorStoreFailure failure,
        std::int32_t platform_result_code = 0) {
        runtime_failure_ = failure;
        runtime_platform_result_ = platform_result_code;
        return doctor_store_detail::action_result(
            failure, platform_result_code);
    }

    DoctorStoreActionResult set_runtime_failure(
        DoctorStoreActionResult failure) {
        runtime_failure_ = failure.failure;
        runtime_platform_result_ = failure.platform_result_code;
        return failure;
    }

    DoctorStoreActionResult set_runtime_failure(
        DoctorStoreFailure failure,
        std::int32_t platform_result_code) {
        runtime_failure_ = failure;
        runtime_platform_result_ = platform_result_code;
        return doctor_store_detail::action_result(
            failure, platform_result_code);
    }

    DoctorStoreActionResult inject(DoctorIoOperation operation) {
        if (!io_faults_) return {};
        const auto failure = io_faults_->before(operation);
        if (!failure) return {};
        const auto reason =
            doctor_store_detail::normalize_fault(failure->failure);
        return doctor_store_detail::action_result(
            reason,
            failure->platform_result_code);
    }

    DoctorStoreActionResult persist_control(
        DoctorIoOperation operation,
        const doctor_store_detail::ControlState &control) {
        const auto fault = inject(operation);
        if (!fault.ok()) return fault;
        return persist_control_unchecked(control);
    }

    DoctorStoreActionResult persist_control_unchecked(
        const doctor_store_detail::ControlState &control) const {
        return doctor_store_detail::replace_file_atomic(
            root_ / L"doctor-control-v1.bin",
            doctor_store_detail::encode_control(control));
    }

    void cleanup_staging() {
        const auto control_stage =
            root_ / L"doctor-control-v1.bin.tmp";
        const auto control_removed =
            doctor_store_detail::delete_owned_file(control_stage);
        if (!control_removed.ok()) {
            runtime_failure_ = control_removed.failure;
            runtime_platform_result_ =
                control_removed.platform_result_code;
        }
        const auto records = root_ / L"records";
        if (doctor_store_detail::is_safe_directory(records)) {
            std::error_code error;
            for (std::filesystem::directory_iterator
                current(records, error), end;
                !error && current != end; current.increment(error)) {
                if (!doctor_store_detail::parse_record_stage_name(
                    current->path()))
                    continue;
                if ((GetFileAttributesW(current->path().c_str()) &
                    FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    continue;
                const auto removed =
                    doctor_store_detail::delete_owned_file(
                        current->path());
                if (!removed.ok() &&
                    runtime_failure_ == DoctorStoreFailure::None) {
                    runtime_failure_ = removed.failure;
                    runtime_platform_result_ =
                        removed.platform_result_code;
                }
            }
        }
        const auto temp = root_ / L"temp";
        if (doctor_store_detail::is_safe_directory(temp)) {
            std::error_code error;
            for (std::filesystem::directory_iterator
                current(temp, error), end;
                !error && current != end; current.increment(error)) {
                if (!doctor_store_detail::parse_export_stage_name(
                    current->path()))
                    continue;
                if ((GetFileAttributesW(current->path().c_str()) &
                    FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    continue;
                const auto removed =
                    doctor_store_detail::delete_owned_file(
                        current->path());
                if (!removed.ok() &&
                    runtime_failure_ == DoctorStoreFailure::None) {
                    runtime_failure_ = removed.failure;
                    runtime_platform_result_ =
                        removed.platform_result_code;
                }
            }
        }
    }

    void cleanup_orphan_exports() {
        const auto temp = root_ / L"temp";
        if (!doctor_store_detail::is_safe_directory(temp)) return;
        std::error_code error;
        for (std::filesystem::directory_iterator current(temp, error), end;
            !error && current != end; current.increment(error)) {
            if (!doctor_store_detail::parse_export_name(current->path()))
                continue;
            if ((GetFileAttributesW(current->path().c_str()) &
                FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                continue;
            if (current->path().filename().string() ==
                control_.active_export_name)
                continue;
            const auto removed =
                doctor_store_detail::delete_owned_file(current->path());
            if (!removed.ok() &&
                runtime_failure_ == DoctorStoreFailure::None) {
                runtime_failure_ = removed.failure;
                runtime_platform_result_ =
                    removed.platform_result_code;
            }
        }
        refresh_temp_bytes();
    }

    void refresh_temp_bytes() const {
        temp_bytes_ = 0;
        if (control_.active_export_name.empty()) return;
        const auto path = active_export_path();
        std::uint64_t size = 0;
        if (doctor_store_detail::file_size_bounded(
            path, limits_.max_export_bytes, &size))
            temp_bytes_ = size;
    }

    DoctorStoreResult<std::vector<DoctorEventRecord>>
    scan_records(bool repair) const {
        const auto envelopes = scan_record_envelopes(repair);
        if (!envelopes.ok())
            return doctor_store_detail::store_result<
                std::vector<DoctorEventRecord>>(
                    envelopes.failure, envelopes.platform_result_code);
        std::vector<DoctorEventRecord> records;
        records.reserve(envelopes.value.size());
        for (const auto &envelope : envelopes.value)
            records.push_back(envelope.event);
        return DoctorStoreResult<std::vector<DoctorEventRecord>>{
            std::move(records), DoctorStoreFailure::None, 0};
    }

    DoctorStoreResult<std::vector<DoctorEventEnvelope>>
    read_record_envelopes(std::int64_t epoch) const {
        if (epoch != control_.epoch)
            return doctor_store_detail::store_result<
                std::vector<DoctorEventEnvelope>>(
                    DoctorStoreFailure::EpochMismatch);
        const auto guard = operation_guard();
        if (!guard.ok())
            return {{}, guard.failure, guard.platform_result_code};
        const auto cleanup = enforce_retention();
        if (!cleanup.ok())
            return DoctorStoreResult<std::vector<DoctorEventEnvelope>>{
                {}, cleanup.failure, cleanup.platform_result_code};
        return scan_record_envelopes(true);
    }

    DoctorStoreResult<std::vector<DoctorEventEnvelope>>
    scan_record_envelopes(bool repair) const {
        std::vector<DoctorEventEnvelope> records;
        std::uint64_t raw = 0;
        std::uint64_t ignored = 0;
        std::uint64_t observed_gap = 0;
        DoctorStoreFailure failure = DoctorStoreFailure::None;
        std::int32_t platform_result = 0;
        std::map<StreamId, std::int64_t> tails;
        std::vector<SegmentInfo> fresh_segments;
        std::uint64_t ordinal = 0;
        const auto records_directory = root_ / L"records";
        if (!doctor_store_detail::is_safe_directory(records_directory)) {
            raw_bytes_ = 0;
            segments_.clear();
            last_sequences_.clear();
            return DoctorStoreResult<std::vector<DoctorEventEnvelope>>{
                {}, DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))};
        }

        std::vector<std::filesystem::path> paths;
        std::error_code iterator_error;
        for (std::filesystem::directory_iterator
            current(records_directory, iterator_error), end;
            !iterator_error && current != end;
            current.increment(iterator_error)) {
            std::uint64_t parsed = 0;
            if (doctor_store_detail::parse_record_name(
                current->path(), &parsed))
                paths.push_back(current->path());
        }
        if (iterator_error) {
            return DoctorStoreResult<std::vector<DoctorEventEnvelope>>{
                {}, DoctorStoreFailure::IoError,
                doctor_store_detail::hresult_from_win32(
                    static_cast<DWORD>(iterator_error.value()))};
        }
        std::sort(paths.begin(), paths.end(), [](const auto &left, const auto &right) {
            std::uint64_t first = 0, second = 0;
            doctor_store_detail::parse_record_name(left, &first);
            doctor_store_detail::parse_record_name(right, &second);
            return first != second ? first < second : left < right;
        });
        for (const auto &path : paths) {
            std::uint64_t segment_ordinal = 0;
            doctor_store_detail::parse_record_name(
                path, &segment_ordinal);
            ordinal = std::max(ordinal, segment_ordinal);
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (failure == DoctorStoreFailure::None)
                    failure = DoctorStoreFailure::PermissionDenied;
                ++ignored;
                continue;
            }
            std::string bytes;
            const auto read = doctor_store_detail::read_file_bounded(
                path, limits_.max_raw_bytes, &bytes);
            if (!read.ok()) {
                if (failure == DoctorStoreFailure::None) {
                    failure = read.failure;
                    platform_result = read.platform_result_code;
                }
                ++ignored;
                continue;
            }
            raw += bytes.size();
            SegmentInfo info;
            info.path = path;
            info.ordinal = segment_ordinal;
            info.bytes = bytes.size();
            std::string valid_bytes;
            bool needs_repair = false;
            std::size_t cursor = 0;
            std::size_t line_number = 1;
            while (cursor < bytes.size()) {
                const auto newline = bytes.find('\n', cursor);
                const auto end = newline == std::string::npos
                    ? bytes.size() : newline;
                std::string line =
                    bytes.substr(cursor, end - cursor);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const auto decoded =
                    DoctorEventCodec::decode_jsonl(line);
                if (decoded.failure != DoctorEventFailure::None ||
                    decoded.events.empty()) {
                    needs_repair = true;
                    ++ignored;
                    ++observed_gap;
                    if (failure == DoctorStoreFailure::None) {
                        failure =
                            decoded.failure == DoctorEventFailure::TornLine
                                ? DoctorStoreFailure::TornTail
                                : DoctorStoreFailure::InvalidSchema;
                    }
                } else {
                    const auto &envelope = decoded.events.front();
                    if (envelope.event.epoch != control_.epoch) {
                        needs_repair = true;
                        ++ignored;
                        ++observed_gap;
                        if (failure == DoctorStoreFailure::None)
                            failure = DoctorStoreFailure::InvalidSchema;
                    } else {
                        const StreamId stream{
                            envelope.event.epoch,
                            envelope.event.producer_id,
                            envelope.event.session_id};
                        const auto previous = tails.find(stream);
                        if (previous != tails.end() &&
                            envelope.event.sequence > previous->second &&
                            envelope.event.sequence -
                                previous->second > 1) {
                            observed_gap += static_cast<std::uint64_t>(
                                envelope.event.sequence -
                                previous->second - 1);
                        }
                        if (previous == tails.end() ||
                            envelope.event.sequence > previous->second)
                            tails[stream] = envelope.event.sequence;
                        records.push_back(envelope);
                        valid_bytes += line;
                        valid_bytes.push_back('\n');
                        ++info.count;
                        info.min_wall_ms =
                            std::min(
                                info.min_wall_ms,
                                envelope.event.wall_utc_ms);
                        info.max_wall_ms =
                            std::max(
                                info.max_wall_ms,
                                envelope.event.wall_utc_ms);
                    }
                }
                if (newline == std::string::npos) break;
                cursor = newline + 1;
                ++line_number;
            }
            fresh_segments.push_back(info);
            if (needs_repair && repair && !valid_bytes.empty()) {
                const auto repaired =
                    doctor_store_detail::replace_file_atomic(
                        path, valid_bytes);
                if (!repaired.ok() && failure == DoctorStoreFailure::None) {
                    failure = repaired.failure;
                    platform_result = repaired.platform_result_code;
                } else if (repaired.ok()) {
                    raw -= bytes.size() - valid_bytes.size();
                    info.bytes = valid_bytes.size();
                    fresh_segments.back().bytes = info.bytes;
                }
            } else if (needs_repair && repair && valid_bytes.empty()) {
                const auto removed =
                    doctor_store_detail::delete_owned_file(path);
                if (!removed.ok() && failure == DoctorStoreFailure::None) {
                    failure = removed.failure;
                    platform_result = removed.platform_result_code;
                } else if (removed.ok()) {
                    raw -= bytes.size();
                }
                if (!fresh_segments.empty()) fresh_segments.pop_back();
            }
        }

        raw_bytes_ = raw;
        segments_ = std::move(fresh_segments);
        active_ordinal_ = ordinal;
        active_segment_index_ = segments_.empty()
            ? std::numeric_limits<std::size_t>::max()
            : segments_.size() - 1;
        // A clean rescan must not erase evidence of a tail already repaired.
        observed_gap_ = std::max(observed_gap_, observed_gap);
        ignored_lines_ = std::max(ignored_lines_, ignored);
        last_sequences_ = std::move(tails);
        if (failure != DoctorStoreFailure::None) {
            runtime_failure_ = failure;
            runtime_platform_result_ = platform_result;
        }
        return DoctorStoreResult<std::vector<DoctorEventEnvelope>>{
            std::move(records), DoctorStoreFailure::None, 0};
    }

    void rebuild_metadata() const {
        const auto result = scan_records(true);
        if (!result.ok() &&
            runtime_failure_ == DoctorStoreFailure::None) {
            runtime_failure_ = result.failure;
            runtime_platform_result_ = result.platform_result_code;
        }
        const auto cleanup = enforce_retention();
        if (!cleanup.ok() &&
            runtime_failure_ == DoctorStoreFailure::None) {
            runtime_failure_ = cleanup.failure;
            runtime_platform_result_ = cleanup.platform_result_code;
        }
        const auto cap = enforce_raw_byte_cap();
        if (!cap.ok() &&
            runtime_failure_ == DoctorStoreFailure::None) {
            runtime_failure_ = cap.failure;
            runtime_platform_result_ = cap.platform_result_code;
        }
        refresh_temp_bytes();
    }

    std::int64_t expiry_cutoff() const {
        const auto now = clock_.wall_utc_ms();
        if (now <= limits_.max_age_ms) return 0;
        return now - limits_.max_age_ms;
    }

    DoctorStoreActionResult enforce_retention() const {
        if (segments_.empty()) return {};
        const auto cutoff = expiry_cutoff();
        bool changed = false;
        for (const auto &segment : segments_) {
            if (segment.count != 0 &&
                segment.max_wall_ms < cutoff) {
                const auto fault =
                    const_cast<DoctorStore *>(this)->inject(
                        DoctorIoOperation::DeleteOldRecord);
                if (!fault.ok()) return fault;
                const auto removed =
                    doctor_store_detail::delete_owned_file(segment.path);
                if (!removed.ok()) return removed;
                changed = true;
                control_.sequence_gap += segment.count;
            }
        }
        if (changed) {
            const auto refreshed = scan_records(true);
            if (!refreshed.ok())
                return doctor_store_detail::action_result(
                    refreshed.failure, refreshed.platform_result_code);
            auto next = control_;
            const auto persisted =
                const_cast<DoctorStore *>(this)->persist_control(
                    DoctorIoOperation::PersistControl, next);
            if (!persisted.ok()) return persisted;
            control_ = next;
        }
        for (const auto &segment : segments_) {
            if (segment.min_wall_ms < cutoff &&
                segment.max_wall_ms >= cutoff) {
                const auto rebuilt = rewrite_segment_without_expired(
                    segment, cutoff);
                if (!rebuilt.ok()) return rebuilt;
                changed = true;
            }
        }
        if (changed) {
            const auto refreshed = scan_records(true);
            if (!refreshed.ok())
                return doctor_store_detail::action_result(
                    refreshed.failure, refreshed.platform_result_code);
        }
        return {};
    }

    DoctorStoreActionResult rewrite_segment_without_expired(
        const SegmentInfo &segment,
        std::int64_t cutoff) const {
        std::string bytes;
        const auto loaded = doctor_store_detail::read_file_bounded(
            segment.path, limits_.max_raw_bytes, &bytes);
        if (!loaded.ok()) return loaded;
        std::string retained;
        std::size_t cursor = 0;
        std::uint64_t removed = 0;
        while (cursor < bytes.size()) {
            const auto newline = bytes.find('\n', cursor);
            const auto end = newline == std::string::npos
                ? bytes.size() : newline;
            const auto line = std::string_view(bytes).substr(
                cursor, end - cursor);
            const auto decoded = DoctorEventCodec::decode_jsonl(line);
            if (decoded.failure == DoctorEventFailure::None &&
                !decoded.events.empty()) {
                if (decoded.events.front().event.wall_utc_ms >= cutoff) {
                    retained.append(line.data(), line.size());
                    retained.push_back('\n');
                } else {
                    ++removed;
                }
            }
            if (newline == std::string::npos) break;
            cursor = newline + 1;
        }
        DoctorStoreActionResult written;
        if (retained.empty()) {
            written = doctor_store_detail::delete_owned_file(
                segment.path);
        } else {
            written = doctor_store_detail::replace_file_atomic(
                segment.path, retained);
        }
        if (written.ok() && removed != 0) {
            control_.sequence_gap += removed;
            const auto persisted = persist_control_unchecked(control_);
            if (!persisted.ok()) return persisted;
        }
        return written;
    }

    DoctorStoreActionResult enforce_raw_byte_cap() const {
        if (raw_bytes_ <= limits_.max_raw_bytes) return {};
        auto ordered = segments_;
        std::sort(
            ordered.begin(), ordered.end(),
            [](const SegmentInfo &left, const SegmentInfo &right) {
                return left.ordinal < right.ordinal;
            });
        bool changed = false;
        for (const auto &segment : ordered) {
            if (raw_bytes_ <= limits_.max_raw_bytes) break;
            if (segments_.size() == 1 &&
                segment.bytes > limits_.max_raw_bytes)
                return doctor_store_detail::action_result(
                    DoctorStoreFailure::RecordTooLarge);
            const auto removed =
                const_cast<DoctorStore *>(this)->delete_with_fault(
                    segment.path);
            if (!removed.ok()) return removed;
            raw_bytes_ -= std::min(raw_bytes_, segment.bytes);
            control_.sequence_gap += segment.count;
            changed = true;
        }
        if (changed) {
            const auto scan = scan_records(true);
            if (!scan.ok())
                return doctor_store_detail::action_result(
                    scan.failure, scan.platform_result_code);
            auto next = control_;
            const auto persisted =
                const_cast<DoctorStore *>(this)->persist_control(
                    DoctorIoOperation::PersistControl, next);
            if (!persisted.ok()) return persisted;
            control_ = next;
        }
        return raw_bytes_ <= limits_.max_raw_bytes
            ? DoctorStoreActionResult{}
            : doctor_store_detail::action_result(
                DoctorStoreFailure::TotalLimit);
    }

    DoctorStoreActionResult rotate_segment() {
        const auto fault = inject(DoctorIoOperation::RotateRecord);
        if (!fault.ok()) return fault;
        ++active_ordinal_;
        auto path = root_ / L"records" /
            (L"doctor-record-" +
                std::to_wstring(active_ordinal_) + L".jsonl");
        const auto created =
            doctor_store_detail::write_file_exact(path, {}, false);
        if (!created.ok()) {
            --active_ordinal_;
            return created;
        }
        SegmentInfo segment;
        segment.path = std::move(path);
        segment.ordinal = active_ordinal_;
        segments_.push_back(std::move(segment));
        active_segment_index_ = segments_.size() - 1;
        return {};
    }

    void update_sequence_gap(const DoctorEventRecord &event) {
        const StreamId stream{
            event.epoch, event.producer_id, event.session_id};
        const auto previous = last_sequences_.find(stream);
        if (previous != last_sequences_.end() &&
            event.sequence > previous->second &&
            event.sequence - previous->second > 1)
            observed_gap_ += static_cast<std::uint64_t>(
                event.sequence - previous->second - 1);
        if (previous == last_sequences_.end() ||
            event.sequence > previous->second)
            last_sequences_[stream] = event.sequence;
    }

    DoctorStoreActionResult delete_with_fault(
        const std::filesystem::path &path) {
        const auto fault = inject(DoctorIoOperation::DeleteOldRecord);
        if (!fault.ok()) return fault;
        return doctor_store_detail::delete_owned_file(path);
    }

    bool delete_all_owned_records() {
        const auto records = root_ / L"records";
        if (!doctor_store_detail::is_safe_directory(records)) return false;
        std::error_code error;
        for (std::filesystem::directory_iterator
            current(records, error), end;
            !error && current != end; current.increment(error)) {
            std::uint64_t ordinal = 0;
            if (!doctor_store_detail::parse_record_name(current->path(), &ordinal) &&
                !doctor_store_detail::parse_record_stage_name(current->path()))
                continue;
            const DWORD attributes = GetFileAttributesW(current->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                continue;
            const auto removed = delete_with_fault(current->path());
            if (!removed.ok()) {
                runtime_failure_ = removed.failure;
                runtime_platform_result_ = removed.platform_result_code;
                return false;
            }
        }
        if (error) {
            runtime_failure_ = DoctorStoreFailure::IoError;
            runtime_platform_result_ =
                doctor_store_detail::hresult_from_win32(
                    static_cast<DWORD>(error.value()));
            return false;
        }
        return true;
    }

    bool delete_all_owned_exports() {
        const auto temp = root_ / L"temp";
        if (!doctor_store_detail::is_safe_directory(temp)) return false;
        std::error_code error;
        for (std::filesystem::directory_iterator
            current(temp, error), end;
            !error && current != end; current.increment(error)) {
            const bool owned =
                doctor_store_detail::parse_export_name(current->path()) ||
                doctor_store_detail::parse_export_stage_name(
                    current->path());
            if (!owned) continue;
            if ((GetFileAttributesW(current->path().c_str()) &
                FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                continue;
            const auto removed = delete_with_fault(current->path());
            if (!removed.ok()) {
                runtime_failure_ = removed.failure;
                runtime_platform_result_ = removed.platform_result_code;
                return false;
            }
        }
        if (error) {
            runtime_failure_ = DoctorStoreFailure::IoError;
            runtime_platform_result_ =
                doctor_store_detail::hresult_from_win32(
                    static_cast<DWORD>(error.value()));
            return false;
        }
        return true;
    }

    void remember_clear_incomplete(
        const DoctorStoreActionResult &failure) {
        runtime_failure_ = failure.failure;
        runtime_platform_result_ = failure.platform_result_code;
        auto pending = control_;
        pending.pending_clear = true;
        pending.failure = failure.failure;
        pending.platform_result_code = failure.platform_result_code;
        if (persist_control_unchecked(pending).ok()) control_ = pending;
    }

    void complete_pending_clear() {
        bool complete =
            delete_all_owned_records() &&
            delete_all_owned_exports();
        if (!complete) return;
        auto next = control_;
        next.pending_clear = false;
        next.failure = DoctorStoreFailure::None;
        next.platform_result_code = 0;
        next.sequence_gap = 0;
        if (!persist_control_unchecked(next).ok()) return;
        control_ = next;
        segments_.clear();
        active_segment_index_ =
            std::numeric_limits<std::size_t>::max();
        active_ordinal_ = 0;
        raw_bytes_ = 0;
        temp_bytes_ = 0;
        observed_gap_ = 0;
        ignored_lines_ = 0;
        last_sequences_.clear();
        clear_runtime_failure();
    }

    std::filesystem::path active_export_path() const {
        if (control_.active_export_name.empty())
            return {};
        return root_ / L"temp" /
            std::filesystem::path(control_.active_export_name);
    }

    static std::uint64_t unique_counter() noexcept {
        static volatile LONG64 counter = 0;
        return static_cast<std::uint64_t>(
            InterlockedIncrement64(&counter));
    }

    std::filesystem::path next_export_path() const {
        const auto unique = std::string(
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" +
            std::to_string(unique_counter()));
        auto path = root_ / L"temp" /
            std::filesystem::path(
                "doctor-export-" + unique + ".zip");
        for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
            if (GetFileAttributesW(path.c_str()) ==
                INVALID_FILE_ATTRIBUTES) return path;
            path = root_ / L"temp" /
                std::filesystem::path(
                    "doctor-export-" + unique + "-" +
                    std::to_string(attempt) + ".zip");
        }
        return {};
    }

    DoctorStoreActionResult expire_active_export() {
        if (control_.active_export_name.empty()) return {};
        if (!active_export_expired()) return {};
        const auto removed =
            doctor_store_detail::delete_owned_file(
                active_export_path());
        if (!removed.ok()) return set_runtime_failure(removed);
        auto next = control_;
        next.active_export_name.clear();
        next.export_created_ms = 0;
        const auto persisted = persist_control(
            DoctorIoOperation::PersistControl, next);
        if (!persisted.ok()) return set_runtime_failure(persisted);
        control_ = std::move(next);
        temp_bytes_ = 0;
        return {};
    }

    bool active_export_expired() const {
        if (control_.active_export_name.empty()) return false;
        const auto now = clock_.wall_utc_ms();
        if (now < 0 || now < control_.export_created_ms ||
            limits_.export_ttl_ms < 0)
            return true;
        return doctor_store_detail::saturating_add(
            control_.export_created_ms, limits_.export_ttl_ms) <= now;
    }

    DoctorStoreActionResult validate_snapshot(
        const DoctorExportSnapshot &snapshot) {
        if (snapshot.epoch != control_.epoch)
            return doctor_store_detail::action_result(
                DoctorStoreFailure::EpochMismatch);
        const auto guard = operation_guard();
        if (!guard.ok()) return guard;
        if (control_.active_export_name.empty())
            return doctor_store_detail::action_result(
                DoctorStoreFailure::ExportExpired);
        const auto expected = active_export_path();
        std::error_code error;
        if (!doctor_store_detail::parse_export_name(snapshot.archive) ||
            snapshot.archive.lexically_normal() !=
                expected.lexically_normal())
            return doctor_store_detail::action_result(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_INVALIDARG));
        if (active_export_expired()) {
            const auto expired = expire_active_export();
            if (!expired.ok()) return expired;
            return doctor_store_detail::action_result(
                DoctorStoreFailure::ExportExpired);
        }
        if (!doctor_store_detail::is_safe_regular(expected)) {
            return doctor_store_detail::action_result(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_INVALIDARG));
        }
        return {};
    }

    DoctorStoreResult<doctor_store_detail::LoadedDoctorExport>
    load_export_snapshot(const DoctorExportSnapshot &snapshot) {
        const auto valid = validate_snapshot(snapshot);
        if (!valid.ok())
            return doctor_store_detail::store_result<
                doctor_store_detail::LoadedDoctorExport>(
                    valid.failure, valid.platform_result_code);

        doctor_store_detail::LoadedDoctorExport loaded;
        const auto read = doctor_store_detail::read_file_bounded(
            snapshot.archive,
            limits_.max_export_bytes,
            &loaded.archive);
        if (!read.ok())
            return doctor_store_detail::store_result<
                doctor_store_detail::LoadedDoctorExport>(
                    read.failure, read.platform_result_code);
        if (!doctor_store_detail::parse_export_archive(
            loaded.archive, &loaded.parsed))
            return doctor_store_detail::store_result<
                doctor_store_detail::LoadedDoctorExport>(
                    DoctorStoreFailure::IoError,
                    static_cast<std::int32_t>(E_INVALIDARG));

        const auto &manifest = loaded.parsed.manifest;
        const auto &mark = loaded.parsed.mark;
        if (manifest.epoch != snapshot.epoch ||
            mark.epoch != snapshot.epoch)
            return doctor_store_detail::store_result<
                doctor_store_detail::LoadedDoctorExport>(
                    DoctorStoreFailure::EpochMismatch);
        for (const auto &envelope : loaded.parsed.events) {
            if (envelope.event.epoch != snapshot.epoch)
                return doctor_store_detail::store_result<
                    doctor_store_detail::LoadedDoctorExport>(
                        DoctorStoreFailure::EpochMismatch);
        }

        if (limits_.export_ttl_ms < 0 ||
            manifest.created_at_ms != control_.export_created_ms ||
            manifest.expires_at_ms != doctor_store_detail::saturating_add(
                control_.export_created_ms, limits_.export_ttl_ms))
            return doctor_store_detail::store_result<
                doctor_store_detail::LoadedDoctorExport>(
                    DoctorStoreFailure::IoError,
                    static_cast<std::int32_t>(E_INVALIDARG));

        loaded.window_begin_ms = std::max<std::int64_t>(
            0,
            doctor_store_detail::saturating_add(
                mark.selected_issue_time_ms, -mark.before_ms));
        loaded.window_end_ms = doctor_store_detail::saturating_add(
            mark.selected_issue_time_ms, mark.after_ms);
        for (const auto &envelope : loaded.parsed.events) {
            if (envelope.event.wall_utc_ms < loaded.window_begin_ms ||
                envelope.event.wall_utc_ms > loaded.window_end_ms)
                return doctor_store_detail::store_result<
                    doctor_store_detail::LoadedDoctorExport>(
                        DoctorStoreFailure::IoError,
                        static_cast<std::int32_t>(E_INVALIDARG));
        }
        return DoctorStoreResult<
            doctor_store_detail::LoadedDoctorExport>{
                std::move(loaded), DoctorStoreFailure::None, 0};
    }

    DoctorStoreActionResult operation_guard() const {
        if (control_.pending_clear)
            return doctor_store_detail::action_result(DoctorStoreFailure::ClearIncomplete, control_.platform_result_code);
        if (control_.failure == DoctorStoreFailure::EpochCommitFailed)
            return doctor_store_detail::action_result(DoctorStoreFailure::EpochCommitFailed, control_.platform_result_code);
        return {};
    }

    std::filesystem::path root_;
    DoctorBuildMetadata build_;
    const DoctorClock &clock_;
    DoctorStoreLimits limits_;
    DoctorIoFaults *io_faults_ = nullptr;
    mutable doctor_store_detail::ControlState control_{};
    mutable std::vector<SegmentInfo> segments_;
    mutable std::uint64_t raw_bytes_ = 0;
    mutable std::uint64_t temp_bytes_ = 0;
    mutable std::uint64_t observed_gap_ = 0;
    mutable std::uint64_t ignored_lines_ = 0;
    mutable std::map<StreamId, std::int64_t> last_sequences_;
    mutable std::size_t active_segment_index_ = 0;
    mutable std::uint64_t active_ordinal_ = 0;
    mutable DoctorStoreFailure runtime_failure_ =
        DoctorStoreFailure::None;
    mutable std::int32_t runtime_platform_result_ = 0;
};

} // namespace boshiamy::doctor

#endif
