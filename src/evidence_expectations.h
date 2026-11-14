#pragma once

#include "context_types.h"

namespace shin::context {

// Assertions inspect only the selected display records, never hidden indexed text.
Json parse_evidence_expectations(const Json& object, const char* field,
                                const std::string& location, std::vector<std::string>& errors);
struct EvidenceAssessment {
    Json missing = Json::array();
    Json forbidden = Json::array();
    bool passed() const { return missing.empty() && forbidden.empty(); }
};
EvidenceAssessment assess_evidence(const Json& records, const Json& required, const Json& forbidden);
Json selected_brief_records(const BriefSelection& selection);

} // namespace shin::context
