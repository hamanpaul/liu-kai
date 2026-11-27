#pragma once
#if __has_include("liukai_build_info.generated.hpp")
#include "liukai_build_info.generated.hpp"
#endif

// Standalone native tests and unversioned local builds are explicitly development builds.
#ifndef LIUKAI_PRODUCT_VERSION
#define LIUKAI_PRODUCT_VERSION "development"
#endif
#ifndef LIUKAI_SOURCE_COMMIT
#define LIUKAI_SOURCE_COMMIT "0000000"
#endif

namespace boshiamy::build_info {
inline constexpr const char* product_version = LIUKAI_PRODUCT_VERSION;
inline constexpr const char* source_commit = LIUKAI_SOURCE_COMMIT;
}
