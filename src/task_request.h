#pragma once

#include "context_types.h"

namespace shin::context {

enum class TaskRequirementKind {
    scope,
    must,
    should
};

struct TaskRequirement {
    std::string id;
    TaskRequirementKind kind = TaskRequirementKind::must;
    QueryRequest query;
    bool require_full_coverage = false;
    int min_covered_groups = 0;
    std::size_t ordinal = 0;
};

struct TaskEvidenceBudget {
    std::size_t max_evidence_bytes = 32U * 1024U;
    int max_records = 24;
    int per_query_records = 8;
    int per_document_records = 2;
};

struct TaskRequest {
    std::string request_id;
    std::string objective;
    std::vector<TaskRequirement> requirements;
    std::vector<std::string> evidence_types;
    TaskEvidenceBudget budget;
    Json canonical_document;
};

Json read_task_request_document(const fs::path& input, bool stdin_input);
TaskRequest parse_task_request(const Json& document, const Profile& profile);
std::string task_requirement_kind_name(TaskRequirementKind kind);
bool task_requirement_is_required(TaskRequirementKind kind) noexcept;

}  // namespace shin::context
