#pragma once

#include "source_syntax.h"
#include <tree_sitter/api.h>

namespace shin::context {

bool is_objective_c_node(TSNode node) noexcept;
void inspect_objective_c_declaration(SyntaxFacts& facts, TSNode node, std::string_view content);

} // namespace shin::context
