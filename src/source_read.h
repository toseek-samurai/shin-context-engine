#pragma once

#include "context_types.h"

namespace shin::context {

struct SourceRedaction {
    struct Range {
        std::size_t begin, end;
        unsigned rules, copied_rules;
    };

    std::string reason;
    std::vector<Range> ranges;

    bool intersects(std::size_t begin, std::size_t end) const;
    void annotate_body(Json& body, const std::vector<std::size_t>& offsets,
                       std::size_t begin, std::size_t end) const;
};

// Mask complete sensitive lines in a private copy. Preserve byte offsets and
// physical line breaks so evidence stays bound to the admitted source.
SourceRedaction redact_source(std::string& content, std::string_view path);
std::string_view source_redaction_rule(std::string_view reason);

std::string read_bound_source(const fs::path& root, const std::string& path,
                              const std::string& expected_hash, std::size_t maximum_bytes);
std::vector<std::size_t> source_line_offsets(const std::string& content);
std::string source_physical_range(const std::string& content,
                                  const std::vector<std::size_t>& offsets, int start, int end);
SecretScanHit source_range_secret(const std::string& text, std::string_view path);

}  // namespace shin::context
