#pragma once

#include "model.h"

namespace shin::server::journal {
inline constexpr std::size_t maximum_facts = 8192;
inline constexpr std::size_t maximum_producers = 128;
inline constexpr std::size_t maximum_reclaim_producers = 32;
Json normalize_producer(const Json& provenance);
Json normalize_batch(const Json& batch);
}
