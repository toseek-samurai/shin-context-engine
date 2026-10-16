#pragma once

#include "io.h"

namespace shin::observer {
Json new_metrics();
void record_metrics(Json& metrics, const Json& observation);
void verify_metrics(const Json& state);
} // namespace shin::observer
