#include "agent_context_pack.h"
#include "context_internal.h"
#include "profile_quality.h"

namespace shin::context {
namespace {

constexpr std::size_t kPackEvalFailurePayloadBytes = 16U * 1024U;

struct PackEvalOptions {
    bool strict = false;
    bool require_current = false;
    bool failed_only = false;
};

struct PreparedPackEvalCase {
    const PackEvalCase* config = nullptr;
    TaskRequest request;
};

struct PackEvalCaseResult {
    std::string id;
    bool passed = false;
    bool expected_complete = true;
    bool actual_complete = false;
    bool identity_required = true;
    bool serial_parallel_identity = false;
    std::vector<std::string> missing_paths;
    std::vector<std::string> unexpected_paths;
    std::vector<std::string> failure_reasons;
    std::size_t evidence_bytes = 0;
    std::size_t evidence_records = 0;
    std::size_t compact_bytes = 0;
    int max_evidence_bytes = -1;
    int max_records = -1;
    int max_compact_bytes = -1;
    unsigned int parallel_jobs = 1;
    std::string receipt_id;
    std::string evidence_set_fingerprint;
    Json requirement_queries = Json::array();
};

struct PackEvalSuiteExecution {
    SuiteEvaluation evaluation;
    std::vector<PackEvalCaseResult> results;
};

PackEvalOptions parse_pack_eval_options(const std::vector<std::string>& args) {
    PackEvalOptions options;
    bool strict_seen = false;
    bool current_seen = false;
    bool failed_only_seen = false;
    for (const std::string& argument : args) {
        if (argument == "--strict") {
            if (strict_seen) {
                throw_usage_error("duplicate-option", "pack-eval accepts --strict only once.");
            }
            options.strict = true;
            strict_seen = true;
        } else if (argument == "--require-current") {
            if (current_seen) {
                throw_usage_error("duplicate-option",
                                  "pack-eval accepts --require-current only once.");
            }
            options.require_current = true;
            current_seen = true;
        } else if (argument == "--failed-only") {
            if (failed_only_seen) {
                throw_usage_error("duplicate-option", "pack-eval accepts --failed-only only once.");
            }
            options.failed_only = true;
            failed_only_seen = true;
        } else {
            throw_usage_error("unsupported-option",
                              "pack-eval only accepts --strict, --require-current, "
                              "and --failed-only.");
        }
    }
    return options;
}

std::set<std::string> pack_paths(const AgentContextPack& pack) {
    std::set<std::string> paths;
    for (const Json& path : pack.verification_paths) {
        paths.insert(path.at("path").get<std::string>());
    }
    return paths;
}

bool same_pack_identity(const AgentContextPack& serial,
                        const AgentContextPack& parallel) {
    return serial.receipt_id == parallel.receipt_id &&
           serial.request_fingerprint == parallel.request_fingerprint &&
           serial.evidence_set_fingerprint == parallel.evidence_set_fingerprint &&
           serial.evidence_ids == parallel.evidence_ids &&
           serial.evidence == parallel.evidence &&
           serial.verification_paths == parallel.verification_paths &&
           serial.requirements == parallel.requirements &&
           serial.completion == parallel.completion &&
           serial.eligible_records == parallel.eligible_records &&
           serial.evidence_bytes == parallel.evidence_bytes &&
           serial.byte_budget_constrained == parallel.byte_budget_constrained &&
           serial.record_budget_constrained == parallel.record_budget_constrained &&
           serial.per_document_budget_constrained == parallel.per_document_budget_constrained;
}

PackEvalCaseResult run_pack_eval_case(const RuntimeOptions& options,
                                      const PackEvalOptions& eval_options,
                                      const RetrievalSession& session,
                                      const PreparedPackEvalCase& prepared) {
    const PackEvalCase& config = *prepared.config;
    const AgentContextPack serial = execute_agent_context_pack(
        options, prepared.request, session, 1);
    const AgentContextPack parallel = execute_agent_context_pack(
        options, prepared.request, session, kMaxPackJobs);

    PackEvalCaseResult result;
    result.id = config.id;
    result.expected_complete = config.expected_complete;
    result.actual_complete = serial.completion.complete;
    result.identity_required = config.require_serial_parallel_identity;
    result.serial_parallel_identity = same_pack_identity(serial, parallel);
    result.evidence_bytes = serial.evidence_bytes;
    result.evidence_records = serial.evidence_ids.size();
    result.max_evidence_bytes = config.max_evidence_bytes;
    result.max_records = config.max_records;
    result.max_compact_bytes = config.max_compact_bytes;
    result.parallel_jobs = parallel.jobs;
    result.receipt_id = serial.receipt_id;
    result.evidence_set_fingerprint = serial.evidence_set_fingerprint;
    for (const TaskRequirement& requirement : prepared.request.requirements) {
        result.requirement_queries.push_back(
            {{"id", requirement.id},
             {"kind", task_requirement_kind_name(requirement.kind)},
             {"query", requirement.query.query},
             {"limit", requirement.query.limit},
             {"per_doc_limit", requirement.query.per_doc_limit}});
    }

    const PackRenderOptions render_options = {
        PackProjection::compact,
        eval_options.require_current,
        false
    };
    result.compact_bytes = render_agent_context_pack(
        options, render_options, prepared.request, session, serial).dump(2).size();

    const std::set<std::string> returned_paths = pack_paths(serial);
    for (const std::string& path : config.must_paths) {
        if (!returned_paths.contains(path)) {
            result.missing_paths.push_back(path);
        }
    }
    for (const std::string& path : config.must_not_paths) {
        if (returned_paths.contains(path)) {
            result.unexpected_paths.push_back(path);
        }
    }

    if (result.actual_complete != result.expected_complete) {
        result.failure_reasons.push_back("completion-mismatch");
    }
    if (!result.missing_paths.empty()) {
        result.failure_reasons.push_back("missing-required-path");
    }
    if (!result.unexpected_paths.empty()) {
        result.failure_reasons.push_back("forbidden-path-returned");
    }
    if (result.identity_required && !result.serial_parallel_identity) {
        result.failure_reasons.push_back("serial-parallel-identity-mismatch");
    }
    if (result.max_evidence_bytes >= 0 &&
        result.evidence_bytes > static_cast<std::size_t>(result.max_evidence_bytes)) {
        result.failure_reasons.push_back("evidence-byte-limit-exceeded");
    }
    if (result.max_records >= 0 &&
        result.evidence_records > static_cast<std::size_t>(result.max_records)) {
        result.failure_reasons.push_back("record-limit-exceeded");
    }
    if (result.max_compact_bytes >= 0 &&
        result.compact_bytes > static_cast<std::size_t>(result.max_compact_bytes)) {
        result.failure_reasons.push_back("compact-byte-limit-exceeded");
    }
    result.passed = result.failure_reasons.empty();
    return result;
}

void validate_pack_eval_suite(const Profile& profile) {
    if (!profile.pack_eval_case_config_errors.empty()) {
        throw CliError("invalid-pack-eval-configuration", "configuration",
                       "Profile packEvalCases contains invalid field types or unknown fields.",
                       2, false,
                       "Run profile-check --strict and correct every invalid-pack-eval-field issue.");
    }
    if (profile.pack_eval_cases.size() > kMaxPackEvalCases) {
        throw CliError("pack-eval-suite-too-large", "resource",
                       "Profile packEvalCases exceeds the bounded local evaluation limit.",
                       1, false, "Use at most 32 pack eval cases per profile.");
    }
}

std::vector<PreparedPackEvalCase> prepare_pack_eval_suite(const Profile& profile) {
    validate_pack_eval_suite(profile);

    std::vector<PreparedPackEvalCase> prepared;
    prepared.reserve(profile.pack_eval_cases.size());
    for (const PackEvalCase& config : profile.pack_eval_cases) {
        prepared.push_back({&config, parse_task_request(config.request_document, profile)});
    }
    return prepared;
}

PackEvalSuiteExecution execute_pack_eval_suite(
    const RetrievalSession& session,
    const std::vector<PreparedPackEvalCase>& prepared,
    const PackEvalOptions& eval_options,
    bool retain_details) {
    PackEvalSuiteExecution suite;
    if (retain_details) {
        suite.results.reserve(prepared.size());
    }
    for (const PreparedPackEvalCase& item : prepared) {
        PackEvalCaseResult result = run_pack_eval_case(
            session.options(), eval_options, session, item);
        suite.evaluation.total += 1;
        suite.evaluation.passed += result.passed ? 1U : 0U;
        if (!result.passed && suite.evaluation.failed_case_ids.size() < kMaxPackEvalCases) {
            suite.evaluation.failed_case_ids.push_back(result.id);
        }
        if (retain_details) {
            suite.results.push_back(std::move(result));
        }
    }
    suite.evaluation.failed = suite.evaluation.total - suite.evaluation.passed;
    return suite;
}

Json pack_eval_case_json(const PackEvalCaseResult& result) {
    return {
        {"id", result.id},
        {"passed", result.passed},
        {"completion", {
            {"expected", result.expected_complete},
            {"actual", result.actual_complete}
        }},
        {"serial_parallel", {
            {"required", result.identity_required},
            {"identical", result.serial_parallel_identity},
            {"parallel_jobs", result.parallel_jobs}
        }},
        {"receipt_id", result.receipt_id},
        {"evidence_set_fingerprint", result.evidence_set_fingerprint},
        {"missing_paths", result.missing_paths},
        {"unexpected_paths", result.unexpected_paths},
        {"failure_reasons", result.failure_reasons},
        {"limits", {
            {"evidence_bytes", result.evidence_bytes},
            {"max_evidence_bytes", result.max_evidence_bytes >= 0
                                           ? Json(result.max_evidence_bytes)
                                           : Json(nullptr)},
            {"records", result.evidence_records},
            {"max_records", result.max_records >= 0
                                ? Json(result.max_records)
                                : Json(nullptr)},
            {"compact_bytes", result.compact_bytes},
            {"max_compact_bytes", result.max_compact_bytes >= 0
                                      ? Json(result.max_compact_bytes)
                                      : Json(nullptr)}
        }}
    };
}

Json pack_eval_failure_json(const RuntimeOptions& options, const PackEvalCaseResult& result) {
    Json output = pack_eval_case_json(result);
    output["diagnostic_id"] = fnv1a64(result.id);
    output["detail_level"] = "full";
    output["requirement_queries"] = result.requirement_queries;
    Json diagnose = Json::array();
    for (const std::string& path : result.missing_paths) {
        for (const Json& requirement : result.requirement_queries) {
            diagnose.push_back(
                {{"requirement_id", requirement.value("id", "")},
                 {"path", path},
                 {"argv",
                  {"shin-context", "--profile", options.profile.id, "--profile-dir",
                   fs::absolute(options.profile_dir).lexically_normal().generic_string(), "--root",
                   fs::absolute(options.root).lexically_normal().generic_string(), "--out-dir",
                   fs::absolute(options.out_root).lexically_normal().generic_string(), "diagnose",
                   requirement.value("query", ""), "--expected-path", path, "--limit",
                   std::to_string(requirement.value("limit", 6)), "--per-doc",
                   std::to_string(requirement.value("per_doc_limit", 2)), "--require-current",
                   "--strict"}},
                 {"automatic_execution", false},
                 {"mutation", {{"consumer", false}, {"index", false}}}});
        }
    }
    output["remediation"] = {{"diagnose_candidates", std::move(diagnose)}};
    return output;
}

Json pack_eval_failure_header_json(const PackEvalCaseResult& result) {
    Json primary_path = nullptr;
    if (!result.missing_paths.empty()) {
        primary_path = result.missing_paths.front();
    } else if (!result.unexpected_paths.empty()) {
        primary_path = result.unexpected_paths.front();
    }
    return {{"diagnostic_id", fnv1a64(result.id)},
            {"detail_level", "header"},
            {"id", result.id},
            {"failure_reasons", result.failure_reasons},
            {"primary_path", std::move(primary_path)},
            {"details_omitted", true}};
}

}  // namespace

SuiteEvaluation evaluate_pack_cases(RetrievalSession& session) {
    const std::vector<PreparedPackEvalCase> prepared =
        prepare_pack_eval_suite(session.options().profile);
    const PackEvalOptions strict_options = {.strict = true};
    return execute_pack_eval_suite(session, prepared, strict_options, false).evaluation;
}

int run_pack_eval(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const PackEvalOptions eval_options = parse_pack_eval_options(args);
    const std::vector<PreparedPackEvalCase> prepared = prepare_pack_eval_suite(options.profile);
    if (!generated_index_available(options)) {
        throw_index_error("index-unavailable", "pack-eval requires a complete generated index.");
    }

    RetrievalSession session(options, eval_options.strict || eval_options.require_current);
    PackEvalSuiteExecution suite = execute_pack_eval_suite(session, prepared, eval_options, true);
    const std::vector<PackEvalCaseResult>& results = suite.results;
    const int total = static_cast<int>(suite.evaluation.total);
    const int passed = static_cast<int>(suite.evaluation.passed);
    const int failed = static_cast<int>(suite.evaluation.failed);
    const IndexFreshness& freshness = session.freshness();
    const bool freshness_ok = !(eval_options.strict || eval_options.require_current) ||
                              (freshness.current && freshness.fully_verified);
    const bool ok = total > 0 && failed == 0 && freshness_ok;
    Json output = make_result_document(
        eval_options.failed_only ? "shin.pack-eval-failures.v1" : "shin.pack-eval.v1",
        "pack-eval", ok);
    if (eval_options.failed_only) {
        output["projection"] = "failed-only";
    }
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["out_root"] = options.out_root.generic_string();
    output["strict"] = eval_options.strict;
    output["require_current"] = eval_options.require_current;
    output["snapshot_id"] = freshness.snapshot_id;
    output["index_freshness"] = index_freshness_json(freshness);
    output["summary"] = {{"total", total}, {"passed", passed}, {"failed", failed}};
    if (eval_options.failed_only) {
        output["suite_failure_reasons"] = Json::array();
        if (total == 0) {
            output["suite_failure_reasons"].push_back("empty-suite");
        }
        if ((eval_options.strict || eval_options.require_current) && !freshness.current) {
            output["suite_failure_reasons"].push_back("index-not-current");
        }
        if ((eval_options.strict || eval_options.require_current) &&
            !freshness.fully_verified) {
            output["suite_failure_reasons"].push_back("index-not-fully-verified");
        }
        if (failed > 0) {
            output["suite_failure_reasons"].push_back("case-failures");
        }
    }
    output["cases"] = Json::array();
    std::size_t failure_bytes = 0;
    std::size_t eligible_failures = 0;
    std::size_t detail_records_omitted = 0;
    for (const PackEvalCaseResult& result : results) {
        if (!eval_options.failed_only) {
            output["cases"].push_back(pack_eval_case_json(result));
            continue;
        }
        if (!result.passed) {
            ++eligible_failures;
            Json candidate = pack_eval_failure_json(options, result);
            const std::size_t separator = output["cases"].empty() ? 0 : 1;
            const std::size_t candidate_bytes = candidate.dump().size() + separator;
            const std::size_t remaining = kPackEvalFailurePayloadBytes -
                                          std::min(failure_bytes,
                                                   kPackEvalFailurePayloadBytes);
            if (candidate_bytes <= remaining) {
                failure_bytes += candidate_bytes;
                output["cases"].push_back(std::move(candidate));
            } else {
                Json header = pack_eval_failure_header_json(result);
                const std::size_t header_bytes = header.dump().size() + separator;
                if (header_bytes <= remaining) {
                    failure_bytes += header_bytes;
                    output["cases"].push_back(std::move(header));
                    ++detail_records_omitted;
                }
            }
        }
    }
    if (eval_options.failed_only) {
        output["failure_budget"] = {{"policy", "whole-record-byte-budget-with-header-v2"},
                                    {"max_records", kMaxPackEvalCases},
                                    {"max_bytes", kPackEvalFailurePayloadBytes},
                                    {"eligible_records", eligible_failures},
                                    {"returned_records", output["cases"].size()},
                                    {"omitted_records", eligible_failures - output["cases"].size()},
                                    {"detail_records_omitted", detail_records_omitted},
                                    {"used_bytes", failure_bytes},
                                    {"truncated", eligible_failures != output["cases"].size() ||
                                                      detail_records_omitted > 0}};
    }
    emit_json_document(std::cout, output);
    return ((eval_options.strict || eval_options.require_current) && !ok) ? 1 : 0;
}

}  // namespace shin::context
