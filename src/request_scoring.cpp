#include "request_scoring.h"
#include "context_internal.h"

namespace shin::context {
namespace {

// Conservative payload accounting includes owned strings, vectors and map nodes.
// Entry count separately bounds allocator/container overhead. This is not RSS.
class Charge {
public:
    explicit Charge(std::size_t limit) : limit_(limit) {}
    void add(std::size_t bytes) { bytes_ = bytes > limit_ - std::min(bytes_, limit_) ? limit_ : bytes_ + bytes; }
    void text(const std::string& text) {
        add(sizeof(std::string) + 128);
        add(text.size()); add(text.size());
    }
    void texts(const std::vector<std::string>& values) {
        for (const auto& value : values) text(value);
    }
    std::size_t bytes() const { return bytes_; }
private:
    std::size_t limit_, bytes_ = 0;
};

void charge_row(Charge& charge, const SectionRow& row) {
    for (const auto* value : {&row.document_id, &row.path, &row.status, &row.authority_level,
         &row.heading, &row.anchor, &row.preview, &row.indexed_text, &row.row_type,
         &row.evidence_kind, &row.verification_level}) charge.text(*value);
    charge.texts(row.topics);
}
void charge_row(Charge& charge, const FactRow& row) {
    for (const auto* value : {&row.id, &row.record_type, &row.path, &row.status, &row.area,
         &row.title, &row.summary, &row.source, &row.authority_level, &row.evidence_kind,
         &row.verification_level, &row.symbol_name, &row.span_hash, &row.range_origin}) charge.text(*value);
    charge.texts(row.topics); charge.texts(row.identifier_terms);
}

MatchScore require_groups(MatchScore score, int required, std::size_t group_count) {
    const int threshold = required > 0 ? required : static_cast<int>(group_count);
    if (score.matched_groups < threshold) {
        score.total = 0;
        score.components.clear();
        score.matched_fields.clear();
    }
    return score;
}

} // namespace

RequestScoring::RequestScoring(const PreparedTermGroups& groups, std::size_t budget, std::size_t entry_limit)
    : budget_(std::min(budget, std::size_t{4U * 1024U * 1024U})),
      entry_limit_(std::min(entry_limit, std::size_t{1024})) {
    if (budget_ == 0 || entry_limit_ == 0) return;
    Charge charge(budget_);
    charge.add(sizeof(PreparedTermGroups));
    for (const auto& group : groups) {
        charge.add(sizeof(PreparedTermGroup));
        for (const auto& term : group) {
            charge.add(sizeof(PreparedMatchTerm));
            charge.text(term.raw); charge.text(term.normalized); charge.text(term.compact);
            charge.texts(term.phrase_keys);
        }
    }
    if (charge.bytes() < budget_) {
        groups_ = groups;
        groups_admitted_ = true;
        retained_bytes_ = charge.bytes();
    }
}

template<class Row>
MatchScore RequestScoring::score(const Row& row, const PreparedTermGroups& groups,
                               int required, std::vector<FieldMatchInput>* fields) {
    ++calls_;
    const auto compute = [&](int threshold) {
        ++computed_;
        if constexpr (std::is_same_v<Row, SectionRow>) return score_section_match(row, groups, threshold, fields);
        else return score_fact_match(row, groups, threshold, fields);
    };
    if (fields || !groups_admitted_ || groups != groups_) {
        ++bypassed_;
        return compute(required);
    }
    auto& entries = [&]() -> auto& {
        if constexpr (std::is_same_v<Row, SectionRow>) return sections_;
        else return facts_;
    }();
    if (const auto found = entries.find(row); found != entries.end()) {
        ++reused_;
        return require_groups(found->second, required, groups.size());
    }
    // Compute complete group evidence once. The caller's required threshold is
    // applied afterwards, including zero-score/group-provenance behavior.
    auto result = compute(1);
    if (sections_.size() + facts_.size() < entry_limit_) {
        const auto available = budget_ - retained_bytes_;
        Charge charge(available);
        charge.add(sizeof(Row) + sizeof(MatchScore) + 256);
        charge_row(charge, row);
        charge.texts(result.matched_fields);
        for (const auto& [name, value] : result.components) { (void)value; charge.text(name); }
        for (const auto group : result.matched_group_indices) { (void)group; charge.add(2 * sizeof(int)); }
        if (charge.bytes() < available) {
            retained_bytes_ += charge.bytes();
            entries.emplace(row, result);
        }
    }
    return require_groups(std::move(result), required, groups.size());
}

MatchScore RequestScoring::section(const SectionRow& row, const PreparedTermGroups& groups,
                                   int required, std::vector<FieldMatchInput>* fields) {
    return score(row, groups, required, fields);
}
MatchScore RequestScoring::fact(const FactRow& row, const PreparedTermGroups& groups,
                                int required, std::vector<FieldMatchInput>* fields) {
    return score(row, groups, required, fields);
}
Json RequestScoring::metrics() const {
    return {{"scope", "single-query"}, {"score_calls", calls_}, {"score_computations", computed_},
        {"score_reuses", reused_}, {"uncacheable_calls", bypassed_},
        {"retained_entries", sections_.size() + facts_.size()}, {"entry_limit", entry_limit_},
        {"retained_payload_bytes_estimate", retained_bytes_}, {"payload_budget_bytes", budget_}};
}

} // namespace shin::context
