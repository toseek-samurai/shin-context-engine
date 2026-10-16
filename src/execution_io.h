#pragma once

#include "context_types.h"

namespace shin::context {

std::string read_execution_file(const fs::path& root, const std::string& relative_path,
                                std::size_t maximum_bytes);
std::string read_execution_report_file(const fs::path& path, std::size_t maximum_bytes);

}  // namespace shin::context
