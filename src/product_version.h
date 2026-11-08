#pragma once

#include <string_view>

namespace shin::context {
// Release metadata is linked once, rather than a compiler flag on every file.
extern const char* const product_version_text;
extern const std::string_view kProductVersion;
}

#define SHIN_VERSION (::shin::context::product_version_text)
