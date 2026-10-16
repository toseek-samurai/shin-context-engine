#pragma once

#include "observer.h"

namespace shin::observer {
Json new_outbox(const Config& config, const std::string& stream_id);
void capture_outbox(const Config& config, Json& state);
void verify_outbox(const Config& config, const Json& state);
Json outbox_summary(const Config& config, const Json& outbox);
void acknowledge_outbox(const Config& config, Json& state, const Json& receipt);
} // namespace shin::observer
