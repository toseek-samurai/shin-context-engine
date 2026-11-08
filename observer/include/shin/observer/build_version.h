#pragma once

#include "product_version.h"

namespace shin::observer {
extern const char* const build_version;
extern const char* const build_stability;
}

#define SHIN_OBSERVER_VERSION (::shin::observer::build_version)
#define SHIN_OBSERVER_STABILITY (::shin::observer::build_stability)
