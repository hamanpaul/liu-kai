#pragma once
#include "settings.hpp"

namespace boshiamy::doctor {
// The EXE store and language-bar metadata use the same per-user location.
// Computing it reads the process environment, never the filesystem.
inline std::filesystem::path doctor_directory() {
    return settings_file_path().parent_path() / L"doctor";
}
}
