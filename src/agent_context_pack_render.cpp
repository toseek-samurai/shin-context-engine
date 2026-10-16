#include "agent_context_pack.h"
#include "context_internal.h"

namespace shin::context {
namespace {

constexpr std::size_t kPackBudgetWarningBasisPoints = 8500;
constexpr std::size_t kPackBudgetCriticalBasisPoints = 9500;
constexpr std::size_t kPackBudgetContributorLimit = 5;

struct PackRecordContributor {
    std::string evidence_id;
    std::string path;
    std::size_t serialized_bytes = 0;
    std::vector<std::string> requirement_ids;
};

struct PackDocumentContributor {
    std::string path;
    std::size_t serialized_record_bytes = 0;
    std::size_t records = 0;
    std::vector<std::string> evidence_ids;
};

std::size_t utilization_basis_points(std::size_t used, std::size_t limit) {
    if (limit == 0) {
        return 10000;
    }
    return std::min<std::size_t>(10000, used * 10000 / limit);
}

std::string pack_pressure_level(std::size_t peak_basis_points,
                                bool budget_constrained) {
    if (budget_constrained || peak_basis_points >= kPackBudgetCriticalBasisPoints) {
        return "critical";
    }
    if (peak_basis_points >= kPackBudgetWarningBasisPoints) {
        return "warning";
    }
    return "normal";
}

std::vector<std::string> contributor_requirement_ids(const Json& evidence) {
    std::vector<std::string> ids;
    const auto matches = evidence.find("matches");
    if (matches == evidence.end() || !matches->is_array()) {
        return ids;
    }
    for (const Json& match : *matches) {
        const std::string id = json_string_value(match, "requirement_id");
        if (!id.empty()) {
            append_unique_string(ids, id);
        }
    }
    return ids;
}

Json pack_budget_pressure_json(const TaskRequest& request,
                               const AgentContextPack& pack) {
    const std::size_t returned_records = pack.evidence_ids.size();
    const std::size_t max_records = static_cast<std::size_t>(request.budget.max_records);
    const std::size_t byte_basis_points = utilization_basis_points(
        pack.evidence_bytes, request.budget.max_evidence_bytes);
    const std::size_t record_basis_points = utilization_basis_points(
        returned_records, max_records);
    const std::size_t peak_basis_points = std::max(byte_basis_points, record_basis_points);
    const bool constrained = pack.byte_budget_constrained || pack.record_budget_constrained ||
                             pack.per_document_budget_constrained;
    const std::string level = pack_pressure_level(peak_basis_points, constrained);

    std::vector<PackRecordContributor> records;
    std::map<std::string, PackDocumentContributor> documents_by_path;
    records.reserve(pack.evidence.size());
    for (const Json& evidence : pack.evidence) {
        PackRecordContributor record;
        record.evidence_id = json_string_value(evidence, "evidence_id");
        if (const Json* location = find_json_field(evidence, "location");
            location != nullptr && location->is_object()) {
            record.path = json_string_value(*location, "path");
        }
        record.serialized_bytes = evidence.dump().size();
        record.requirement_ids = contributor_requirement_ids(evidence);
        records.push_back(record);

        PackDocumentContributor& document = documents_by_path[record.path];
        document.path = record.path;
        document.serialized_record_bytes += record.serialized_bytes;
        ++document.records;
        document.evidence_ids.push_back(record.evidence_id);
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return std::tuple(-static_cast<long long>(left.serialized_bytes), left.path,
                          left.evidence_id) <
               std::tuple(-static_cast<long long>(right.serialized_bytes), right.path,
                          right.evidence_id);
    });

    std::vector<PackDocumentContributor> documents;
    documents.reserve(documents_by_path.size());
    for (auto& [path, document] : documents_by_path) {
        (void)path;
        documents.push_back(std::move(document));
    }
    std::sort(documents.begin(), documents.end(), [](const auto& left, const auto& right) {
        return std::tuple(-static_cast<long long>(left.serialized_record_bytes), left.path) <
               std::tuple(-static_cast<long long>(right.serialized_record_bytes), right.path);
    });

    Json top_records = Json::array();
    for (std::size_t index = 0;
         index < std::min(records.size(), kPackBudgetContributorLimit); ++index) {
        const PackRecordContributor& record = records[index];
        top_records.push_back({
            {"evidence_id", record.evidence_id},
            {"path", record.path},
            {"serialized_bytes", record.serialized_bytes},
            {"share_basis_points",
             utilization_basis_points(record.serialized_bytes, pack.evidence_bytes)},
            {"requirement_ids", record.requirement_ids}
        });
    }

    Json top_documents = Json::array();
    for (std::size_t index = 0;
         index < std::min(documents.size(), kPackBudgetContributorLimit); ++index) {
        const PackDocumentContributor& document = documents[index];
        top_documents.push_back({
            {"path", document.path},
            {"serialized_record_bytes", document.serialized_record_bytes},
            {"share_basis_points",
             utilization_basis_points(document.serialized_record_bytes,
                                      pack.evidence_bytes)},
            {"records", document.records},
            {"evidence_ids", document.evidence_ids}
        });
    }

    Json warning_codes = Json::array();
    if (level == "warning") {
        warning_codes.push_back("pack-budget-near-limit");
    } else if (level == "critical") {
        warning_codes.push_back(constrained ? "pack-budget-constrained"
                                            : "pack-budget-critical");
    }

    std::string dominant_dimension = "bytes";
    if (record_basis_points > byte_basis_points) {
        dominant_dimension = "records";
    } else if (record_basis_points == byte_basis_points) {
        dominant_dimension = "bytes-and-records";
    }
    return {
        {"policy", "fixed-utilization-thresholds-v1"},
        {"level", level},
        {"warning", level != "normal"},
        {"warning_codes", std::move(warning_codes)},
        {"dominant_dimension", dominant_dimension},
        {"thresholds_basis_points", {
            {"warning", kPackBudgetWarningBasisPoints},
            {"critical", kPackBudgetCriticalBasisPoints}
        }},
        {"bytes", {
            {"used", pack.evidence_bytes},
            {"limit", request.budget.max_evidence_bytes},
            {"remaining", request.budget.max_evidence_bytes - pack.evidence_bytes},
            {"utilization_basis_points", byte_basis_points}
        }},
        {"records", {
            {"used", returned_records},
            {"limit", max_records},
            {"remaining", max_records - returned_records},
            {"utilization_basis_points", record_basis_points}
        }},
        {"top_records", std::move(top_records)},
        {"top_documents", std::move(top_documents)}
    };
}

Json pack_completion_json(const PackCompletion& completion) {
    return {
        {"complete", completion.complete},
        {"coverage_complete", completion.coverage_complete},
        {"required_requirements", completion.required_requirements},
        {"satisfied_required_requirements", completion.satisfied_required_requirements},
        {"required_requirements_with_complete_coverage",
         completion.required_requirements_with_complete_coverage},
        {"unsatisfied_required_requirement_ids",
         completion.unsatisfied_required_requirement_ids},
        {"required_requirement_ids_with_uncovered_groups",
         completion.required_requirement_ids_with_uncovered_groups},
        {"optional_requirements", completion.optional_requirements},
        {"satisfied_optional_requirements", completion.satisfied_optional_requirements},
        {"unsatisfied_optional_requirement_ids",
         completion.unsatisfied_optional_requirement_ids}
    };
}

Json pack_budget_json(const TaskRequest& request, const AgentContextPack& pack) {
    return {
        {"policy", "compact-json-evidence-array-bytes-v1"},
        {"max_evidence_bytes", request.budget.max_evidence_bytes},
        {"evidence_bytes", pack.evidence_bytes},
        {"max_records", request.budget.max_records},
        {"eligible_records", pack.eligible_records},
        {"returned_records", pack.evidence_ids.size()},
        {"budgeted_out_records", pack.eligible_records - pack.evidence_ids.size()},
        {"byte_budget_constrained", pack.byte_budget_constrained},
        {"record_budget_constrained", pack.record_budget_constrained},
        {"per_document_budget_constrained", pack.per_document_budget_constrained},
        {"per_query_records", request.budget.per_query_records},
        {"per_document_records", request.budget.per_document_records},
        {"pressure", pack_budget_pressure_json(request, pack)}
    };
}

Json full_pack_receipt_json(const RuntimeOptions& options,
                            const RetrievalSession& session,
                            const AgentContextPack& pack) {
    return {
        {"schema_version", "shin.context-pack-receipt.v1"},
        {"receipt_id", pack.receipt_id},
        {"engine_version", kProductVersion},
        {"generator_version", kGeneratorVersion},
        {"profile", options.profile.id},
        {"request_fingerprint", pack.request_fingerprint},
        {"snapshot_id", session.freshness().snapshot_id},
        {"evidence_set_fingerprint", pack.evidence_set_fingerprint},
        {"evidence_ids", pack.evidence_ids},
        {"identity_algorithm", "fnv1a64"},
        {"deterministic", true},
        {"cryptographic_authentication", false}
    };
}

Json render_full_context_pack(const RuntimeOptions& options,
                              const PackRenderOptions& render_options,
                              const TaskRequest& request,
                              const RetrievalSession& session,
                              const AgentContextPack& pack) {
    Json output = make_result_document(
        "shin.agent-context-pack.v1", "pack",
        !render_options.strict || pack.completion.complete);
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["out_root"] = options.out_root.generic_string();
    output["request_id"] = request.request_id;
    output["objective"] = request.objective;
    output["strict"] = render_options.strict;
    output["require_current"] = render_options.require_current;
    output["snapshot_id"] = session.freshness().snapshot_id;
    output["index_freshness"] = index_freshness_json(session.freshness());
    output["freshness_scope"] = "session-open";
    output["evidence_types"] = request.evidence_types;
    output["execution"] = {
        {"snapshot_pinned", true},
        {"requirement_count", request.requirements.size()},
        {"jobs", pack.jobs},
        {"result_order", "deterministic-selection-order"},
        {"retrieval_policy", "brief-execution-per-requirement-v1"},
        {"selection_policy", "replacement-aware-requirement-seed-ranked-fill-v4"},
        {"deduplication_policy", "canonical-record-v1"}
    };
    output["completion"] = pack_completion_json(pack.completion);
    output["budget"] = pack_budget_json(request, pack);
    output["requirements"] = pack.requirements;
    output["verification_paths"] = pack.verification_paths;
    output["evidence"] = pack.evidence;
    output["receipt"] = full_pack_receipt_json(options, session, pack);
    return output;
}

Json compact_coverage_policy(const Json& policy) {
    Json compact = {{"mode", policy.at("mode")}};
    const int minimum = policy.at("minimum_covered_groups").get<int>();
    if (minimum > 0) {
        compact["minimum_covered_groups"] = minimum;
    }
    return compact;
}

Json compact_selected_coverage(const Json& coverage) {
    return {
        {"total_groups", coverage.at("total_groups")},
        {"covered_groups", coverage.at("covered_groups")},
        {"uncovered_groups", coverage.at("uncovered_groups")},
        {"uncovered_group_indices", coverage.at("uncovered_group_indices")}
    };
}

Json compact_requirement_summaries(const Json& requirements) {
    Json summaries = Json::array();
    for (const Json& requirement : requirements) {
        const Json& coverage = requirement.at("query_coverage");
        summaries.push_back({
            {"id", requirement.at("id")},
            {"kind", requirement.at("kind")},
            {"required", requirement.at("required")},
            {"satisfied", requirement.at("satisfied")},
            {"status", requirement.at("status")},
            {"coverage_policy",
             compact_coverage_policy(requirement.at("coverage_policy"))},
            {"selected_query_coverage",
             compact_selected_coverage(requirement.at("selected_query_coverage"))},
            {"selected_evidence", requirement.at("selected_evidence")},
            {"query_coverage", {
                {"total_groups", coverage.at("total_groups")},
                {"covered_groups", coverage.at("covered_groups")},
                {"uncovered_groups", coverage.at("uncovered_groups")}
            }}
        });
    }
    return summaries;
}

Json compact_requirement_gaps(const Json& requirements) {
    Json gaps = Json::array();
    for (const Json& requirement : requirements) {
        if (!requirement.at("satisfied").get<bool>()) {
            gaps.push_back(requirement);
        }
    }
    return gaps;
}

Json render_compact_context_pack(const RuntimeOptions& options,
                                 const PackRenderOptions& render_options,
                                 const TaskRequest& request,
                                 const RetrievalSession& session,
                                 const AgentContextPack& pack) {
    Json output = make_result_document(
        "shin.agent-context-pack-compact.v1", "pack",
        !render_options.strict || pack.completion.complete);
    output["projection"] = "compact";
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["request_id"] = request.request_id;
    output["objective"] = request.objective;
    output["strict"] = render_options.strict;
    output["require_current"] = render_options.require_current;
    output["snapshot_id"] = session.freshness().snapshot_id;
    output["index_freshness"] = index_freshness_json(session.freshness());
    output["completion"] = pack_completion_json(pack.completion);
    output["budget"] = pack_budget_json(request, pack);
    output["requirements"] = compact_requirement_summaries(pack.requirements);
    output["gaps"] = compact_requirement_gaps(pack.requirements);
    output["verification_paths"] = pack.verification_paths;
    output["evidence"] = pack.evidence;
    output["receipt"] = {
        {"schema_version", "shin.context-pack-receipt.v1"},
        {"receipt_id", pack.receipt_id},
        {"request_fingerprint", pack.request_fingerprint},
        {"evidence_set_fingerprint", pack.evidence_set_fingerprint},
        {"identity_algorithm", "fnv1a64"},
        {"deterministic", true}
    };
    return output;
}

}  // namespace

std::string pack_projection_name(PackProjection projection) {
    return projection == PackProjection::compact ? "compact" : "full";
}

Json render_agent_context_pack(const RuntimeOptions& options,
                               const PackRenderOptions& render_options,
                               const TaskRequest& request,
                               const RetrievalSession& session,
                               const AgentContextPack& pack) {
    if (render_options.projection == PackProjection::compact) {
        return render_compact_context_pack(options, render_options, request, session, pack);
    }
    return render_full_context_pack(options, render_options, request, session, pack);
}

Json pack_telemetry_json(const PackTelemetry& telemetry) {
    auto rounded_ms = [](double value) {
        return std::round(value * 1000.0) / 1000.0;
    };
    return {
        {"schema_version", "shin.pack-telemetry.v1"},
        {"semantics", "single-command-pre-emission-v1"},
        {"clock", "steady-clock"},
        {"phases_ms", {
            {"request_parse", rounded_ms(telemetry.request_parse_ms)},
            {"session_open", rounded_ms(telemetry.session_open_ms)},
            {"retrieval", rounded_ms(telemetry.retrieval_ms)},
            {"selection", rounded_ms(telemetry.selection_ms)},
            {"projection", rounded_ms(telemetry.projection_ms)},
            {"serialization", rounded_ms(telemetry.serialization_ms)},
            {"total_before_emit", rounded_ms(telemetry.total_ms)}
        }},
        {"process_peak_rss_bytes",
         telemetry.process_peak_rss_bytes > 0
             ? Json(telemetry.process_peak_rss_bytes)
             : Json(nullptr)},
        {"projected_payload_bytes", telemetry.projected_payload_bytes}
    };
}

}  // namespace shin::context
