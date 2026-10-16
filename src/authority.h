#pragma once

#include "context_types.h"

namespace shin::context {

std::string explicit_metadata_status(const Json* json_doc);
std::string explicit_document_status(const std::string& content);
std::vector<int> unfenced_markdown_heading_levels(const std::vector<std::string>& lines);
std::string explicit_section_status(const std::vector<std::string>& lines,
                                    std::size_t start,
                                    std::size_t end);
std::string fallback_status_from_path(const std::string& path);
std::string infer_authority_level(const std::string& doc_type,
                                  const std::string& record_type,
                                  const std::string& path,
                                  const std::string& status);
std::string fact_authority_level_or_default(const SourceDoc& doc,
                                            const std::string& record_type,
                                            const std::string& status,
                                            const std::string& explicit_level);

}  // namespace shin::context
