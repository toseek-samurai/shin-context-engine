#pragma once

#include "context_types.h"

namespace shin::context {

using TermRowCounts = std::array<std::uint32_t, 5>;
// Portable little-endian v1: header, N+1 record offsets, sorted term records.
// Each record owns its only key and five count + delta-varint posting lists.
std::string encode_compact_terms(const TermPostingMap& postings, const TermRowCounts& counts);
TermRowCounts term_row_counts(const Json& manifest);
std::uint32_t compact_term_count(const Json& manifest);

class CompactTerms {
public:
    CompactTerms(std::istream& input, const TermRowCounts& expected);
    std::uint32_t size() const { return count_; }
    std::string term(std::uint32_t id);
    TermPosting posting(std::uint32_t id);
    std::uint32_t lower_bound(std::string_view key);
    void validate_all();
private:
    std::istream& input_;
    TermRowCounts counts_;
    std::uint32_t count_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t data_start_ = 0;
    std::pair<std::uint64_t, std::uint64_t> range(std::uint32_t id);
    std::string read(std::uint64_t offset, std::size_t bytes);
};

} // namespace shin::context
