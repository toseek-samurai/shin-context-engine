#pragma once

#include "context_types.h"

namespace shin::context {

SectionRow decode_generated_section_row(std::string_view line);
SourceDoc decode_generated_document_row(std::string_view line);
LinkRow decode_generated_link_row(std::string_view line);
FactRow decode_generated_fact_row(std::string_view line);
FactRow decode_generated_decision_row(std::string_view line);
FactRow decode_generated_typed_reference_row(std::string_view line);
TermPosting decode_generated_term_posting(std::string_view line,
                                          std::string* parsed_term = nullptr);

std::string generated_row_decoder_version();
std::string generated_row_decoder_implementation();

}  // namespace shin::context
