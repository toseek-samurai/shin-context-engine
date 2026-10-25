#pragma once
#include "shin/observer/io.h"

namespace shin::observer {
// Private numerical projections only; never exports call bodies or file labels.
Json analyze_session_activity(const Json& calls, const Json& usage, const std::string& session,
                              std::int64_t from, std::int64_t until, bool calls_complete);
}
