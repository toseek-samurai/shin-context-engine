#pragma once
#include "io.h"

namespace shin::observer {
// Read-only presentation of the existing status contract; never probes projects.
std::string local_status_text(const Json& status);
}
