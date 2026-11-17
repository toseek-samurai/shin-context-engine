#pragma once
#include "context_types.h"

namespace shin::context {

// One request owns all attempts, including rejected probes. Counts are row visits,
// not unique rows, elapsed time, posting bytes or a relevance/completeness verdict.
class RetrievalPlan {
public:
    explicit RetrievalPlan(std::size_t budget = 8 * kMaxIndexedCandidateRows) : budget_(budget) {}
    bool admits(std::size_t rows) {
        if (rows > budget_ - std::min(visited_, budget_) || stages_.size() >= 16) {
            limited_ = true;
            return false;
        }
        return true;
    }
    void record(const char* stage, const BriefResult& result) {
        visited_ += result.candidate_rows;
        sections_ += result.matched_sections;
        facts_ += result.matched_facts;
        const auto pruned = result.query_plan.value("section_score_prunes", std::size_t{0});
        pruned_sections_ += pruned;
        if (stages_.size() < 16) {
            Json item = {{"stage", stage}, {"candidate_rows", result.candidate_rows},
                {"matched_sections", result.matched_sections}, {"matched_facts", result.matched_facts}};
            if (pruned) item["section_score_prunes"] = pruned;
            stages_.push_back(std::move(item));
        }
    }
    template<class Work> BriefResult run(const char* stage, Work&& work) {
        auto result = work();
        record(stage, result);
        return result;
    }
    BriefResult finish(BriefResult result) const {
        result.query_plan = {{"policy", "bounded-adaptive-retrieval-v1"}, {"stages", stages_},
            {"candidate_row_visits", visited_}, {"matched_section_visits", sections_},
            {"matched_fact_visits", facts_}, {"optional_row_visit_budget", budget_},
            {"optional_work_limited", limited_}};
        if (pruned_sections_) result.query_plan["section_score_prunes"] = pruned_sections_;
        return result;
    }
private:
    std::size_t budget_, visited_ = 0, sections_ = 0, facts_ = 0;
    std::size_t pruned_sections_ = 0;
    bool limited_ = false;
    Json stages_ = Json::array();
};

} // namespace shin::context
