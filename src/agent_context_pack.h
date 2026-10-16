#pragma once

#include "retrieval_session.h"
#include "task_request.h"

namespace shin::context {

inline constexpr int kMaxPackJobs = 8;
inline constexpr std::size_t kMaxPackOutputBytes = 16U * 1024U * 1024U;

enum class PackProjection {
    full,
    compact
};

struct PackCompletion {
    bool complete = false;
    bool coverage_complete = false;
    std::size_t required_requirements = 0;
    std::size_t satisfied_required_requirements = 0;
    std::size_t required_requirements_with_complete_coverage = 0;
    std::vector<std::string> unsatisfied_required_requirement_ids;
    std::vector<std::string> required_requirement_ids_with_uncovered_groups;
    std::size_t optional_requirements = 0;
    std::size_t satisfied_optional_requirements = 0;
    std::vector<std::string> unsatisfied_optional_requirement_ids;

    bool operator==(const PackCompletion&) const = default;
};

struct AgentContextPack {
    Json requirements = Json::array();
    Json evidence = Json::array();
    Json verification_paths = Json::array();
    std::vector<std::string> evidence_ids;
    PackCompletion completion;
    std::size_t eligible_records = 0;
    std::size_t evidence_bytes = 2;
    bool byte_budget_constrained = false;
    bool record_budget_constrained = false;
    bool per_document_budget_constrained = false;
    std::string request_fingerprint;
    std::string evidence_set_fingerprint;
    std::string receipt_id;
    unsigned int jobs = 1;
    double retrieval_ms = 0.0;
    double selection_ms = 0.0;
};

struct PackRenderOptions {
    PackProjection projection = PackProjection::full;
    bool require_current = false;
    bool strict = false;
};

struct PackTelemetry {
    double request_parse_ms = 0.0;
    double session_open_ms = 0.0;
    double retrieval_ms = 0.0;
    double selection_ms = 0.0;
    double projection_ms = 0.0;
    double serialization_ms = 0.0;
    double total_ms = 0.0;
    std::uint64_t process_peak_rss_bytes = 0;
    std::size_t projected_payload_bytes = 0;
};

std::string pack_projection_name(PackProjection projection);
unsigned int pack_active_jobs(const TaskRequest& request, int requested_jobs);
AgentContextPack execute_agent_context_pack(const RuntimeOptions& options,
                                            const TaskRequest& request,
                                            const RetrievalSession& session,
                                            int requested_jobs);
Json render_agent_context_pack(const RuntimeOptions& options,
                               const PackRenderOptions& render_options,
                               const TaskRequest& request,
                               const RetrievalSession& session,
                               const AgentContextPack& pack);
Json pack_telemetry_json(const PackTelemetry& telemetry);

}  // namespace shin::context
