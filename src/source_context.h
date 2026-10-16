#pragma once
#include "context_types.h"

namespace shin::context {
// Called only inside the source provider's publication lock and live checks.
Json collect_source_context(const RuntimeOptions& options, const std::string& snapshot,
                            const Json& candidates, const std::map<std::string, std::string>& sources);
}
