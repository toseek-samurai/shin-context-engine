#pragma once
#include "shin/server/config.h"
#include <vector>

unsigned check_scoped_cleanup(const shin::server::Config& config, std::vector<std::uint64_t>& jobs);
