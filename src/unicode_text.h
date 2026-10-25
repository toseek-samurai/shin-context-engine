#pragma once
#include <string>
#include <string_view>

namespace shin::context {
bool has_non_ascii(std::string_view text) noexcept;
std::string unicode_casefold(std::string_view text);
std::string unicode_query_spacing(std::string_view text);
std::string unicode_trim_query_token(std::string_view text);
std::string unicode_match_text(std::string_view text);
bool unicode_index_term(std::string_view text);
bool unicode_prefix_term(std::string_view text);
}
