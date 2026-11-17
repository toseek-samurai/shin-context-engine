#pragma once

#include "context_types.h"

namespace shin::context {

// Reuse pure scoring work inside one query. Keys own every row field; changed
// text, authority, provenance or term aliases can never reuse another score.
// Capacity exhaustion falls through to the ordinary scorer, not partial search.
class RequestScoring {
public:
    explicit RequestScoring(const PreparedTermGroups& groups,
                            std::size_t budget = 4U * 1024U * 1024U,
                            std::size_t entry_limit = 1024);
    RequestScoring(const RequestScoring&) = delete;
    RequestScoring& operator=(const RequestScoring&) = delete;
    MatchScore section(const SectionRow& row, const PreparedTermGroups& groups,
                       int required, std::vector<FieldMatchInput>* fields);
    MatchScore fact(const FactRow& row, const PreparedTermGroups& groups,
                    int required, std::vector<FieldMatchInput>* fields);
    Json metrics() const;

private:
    template<class Row> MatchScore score(const Row&, const PreparedTermGroups&, int,
                                        std::vector<FieldMatchInput>*);
    PreparedTermGroups groups_;
    std::size_t budget_, entry_limit_, retained_bytes_ = 0;
    std::size_t calls_ = 0, computed_ = 0, reused_ = 0, bypassed_ = 0;
    bool groups_admitted_ = false;
    std::map<SectionRow, MatchScore> sections_;
    std::map<FactRow, MatchScore> facts_;
};

} // namespace shin::context
