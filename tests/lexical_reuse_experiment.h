#pragma once
#include <string>
#include <string_view>
#include <vector>

// Linked only by the optional experiment, never by the installed Core library.
namespace shin::context::experiment {
std::vector<std::string> lexical_terms(std::string_view text);
}
