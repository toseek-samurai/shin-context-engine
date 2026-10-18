#pragma once
#include "shin/observer/io.h"

namespace shin::observer::client_config {
// Editors change only the selected Shin entry, retaining unrelated source bytes.
Json merge_server(const Json& before, const Json& desired, bool opencode);
std::string merge_json(const std::string& source, const Json& desired, bool opencode);
std::string merge_toml(const std::string& source, const Json& desired);
std::string merge_yaml(const std::string& source, const Json& desired);
}
