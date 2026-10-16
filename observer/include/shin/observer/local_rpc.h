#pragma once
#include "io.h"

namespace shin::observer::local_rpc {
Json reply(const std::string& id);
Json failure(const std::string& id, const std::string& code, const std::string& message, const Json& diagnostic = nullptr);
Json project_response(const Json& response, const std::string& id);
void validate_request(const Json& value);
std::string string_field(const Json& value, const char* key, std::size_t limit);
}
