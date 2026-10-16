#include "context_internal.h"
#include "task_request.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxTaskInputBytes = 1024U * 1024U;
constexpr std::size_t kMaxTaskRequestIdBytes = 128;
constexpr std::size_t kMaxTaskObjectiveBytes = 16384;
constexpr std::size_t kMaxTaskQueryBytes = 16384;
constexpr std::size_t kMaxTaskRequirements = 32;
constexpr std::size_t kMaxTaskTotalQueryBytes = 256U * 1024U;
constexpr std::size_t kMaxTaskTotalGroups = 512;
constexpr std::size_t kMaxTaskTotalExpandedTerms = 4096;
constexpr std::size_t kMaxTaskEvidenceBytes = 4U * 1024U * 1024U;
constexpr int kMaxTaskRecords = 128;
constexpr int kMaxTaskPerQueryRecords = 64;
constexpr int kMaxTaskPerDocumentRecords = 8;

std::string read_bounded_task_stream(std::istream& input, const std::string& label) {
    std::string content;
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            continue;
        }
        const std::size_t bytes = static_cast<std::size_t>(count);
        if (content.size() > kMaxTaskInputBytes ||
            bytes > kMaxTaskInputBytes - content.size()) {
            throw CliError("task-input-too-large", "resource",
                           label + " exceeds the 1 MiB task input limit.", 1, false,
                           "Split the task into a smaller request document.");
        }
        content.append(buffer.data(), bytes);
    }
    if (input.bad()) {
        throw CliError("task-input-read-failed", "filesystem",
                       "Cannot read " + label + ".", 1, true,
                       "Verify the input path and retry.");
    }
    return content;
}

void require_only_task_fields(const Json& object,
                              const std::set<std::string>& allowed,
                              const std::string& location) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (!allowed.contains(field.key())) {
            throw_usage_error("unknown-task-field",
                              location + " has unknown field: " + field.key() + ".");
        }
    }
}

bool control_free_identifier(const std::string& value) {
    if (value.empty() || value.size() > kMaxTaskRequestIdBytes) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7fU;
    });
}

long long bounded_task_integer(const Json& object,
                               const std::string& field,
                               long long fallback,
                               long long minimum,
                               long long maximum,
                               const std::string& location) {
    const auto value = object.find(field);
    if (value == object.end()) {
        return fallback;
    }
    if (!value->is_number_integer()) {
        throw_usage_error("invalid-task-field",
                          location + "." + field + " must be an integer.");
    }
    long long parsed = 0;
    try {
        parsed = value->get<long long>();
    } catch (const Json::exception&) {
        throw_usage_error("invalid-task-field",
                          location + "." + field + " is outside the integer range.");
    }
    if (parsed < minimum || parsed > maximum) {
        throw_usage_error("invalid-task-field",
                          location + "." + field + " must be between " +
                              std::to_string(minimum) + " and " +
                              std::to_string(maximum) + ".");
    }
    return parsed;
}

std::vector<std::string> parse_evidence_types(const Json& document) {
    static const std::array<std::string_view, 5> supported = {
        "contract", "decision", "runbook", "fact", "section"
    };
    const Json* configured = find_json_field(document, "evidence_types");
    std::set<std::string> selected;
    if (configured == nullptr) {
        for (std::string_view type : supported) {
            selected.emplace(type);
        }
    } else {
        if (!configured->is_array() || configured->empty()) {
            throw_usage_error("invalid-task-evidence-types",
                              "evidence_types must be a non-empty array of strings.");
        }
        for (std::size_t index = 0; index < configured->size(); ++index) {
            const Json& value = (*configured)[index];
            if (!value.is_string()) {
                throw_usage_error("invalid-task-evidence-types",
                                  "evidence_types[" + std::to_string(index) +
                                      "] must be a string.");
            }
            const std::string type = value.get<std::string>();
            const bool known = std::find(supported.begin(), supported.end(), type) !=
                               supported.end();
            if (!known) {
                throw_usage_error("invalid-task-evidence-type",
                                  "Unsupported evidence type: " + type + ".");
            }
            if (!selected.insert(type).second) {
                throw_usage_error("duplicate-task-evidence-type",
                                  "evidence_types contains a duplicate: " + type + ".");
            }
        }
    }

    std::vector<std::string> normalized;
    for (std::string_view type : supported) {
        if (selected.contains(std::string(type))) {
            normalized.emplace_back(type);
        }
    }
    return normalized;
}

TaskEvidenceBudget parse_task_budget(const Json& document) {
    TaskEvidenceBudget budget;
    const Json* configured = find_json_field(document, "budget");
    if (configured == nullptr) {
        return budget;
    }
    if (!configured->is_object()) {
        throw_usage_error("invalid-task-budget", "budget must be an object.");
    }
    require_only_task_fields(*configured,
                             {"max_evidence_bytes", "max_records",
                              "per_query_records", "per_document_records"},
                             "budget");
    budget.max_evidence_bytes = static_cast<std::size_t>(bounded_task_integer(
        *configured, "max_evidence_bytes", static_cast<long long>(budget.max_evidence_bytes),
        2, static_cast<long long>(kMaxTaskEvidenceBytes), "budget"));
    budget.max_records = static_cast<int>(bounded_task_integer(
        *configured, "max_records", budget.max_records, 1, kMaxTaskRecords, "budget"));
    budget.per_query_records = static_cast<int>(bounded_task_integer(
        *configured, "per_query_records", budget.per_query_records, 1,
        kMaxTaskPerQueryRecords, "budget"));
    budget.per_document_records = static_cast<int>(bounded_task_integer(
        *configured, "per_document_records", budget.per_document_records, 1,
        kMaxTaskPerDocumentRecords, "budget"));
    return budget;
}

void prepare_task_query(const Profile& profile,
                        QueryRequest& request,
                        const std::string& location) {
    if (request.query.empty()) {
        throw_usage_error("invalid-task-query", location + ".query must be non-empty.");
    }
    if (request.query.size() > kMaxTaskQueryBytes) {
        throw CliError("task-query-too-large", "resource",
                       location + ".query exceeds 16384 bytes.", 1, false,
                       "Split the evidence need into smaller queries.");
    }
    request.terms = parse_terms(request.query);
    if (request.terms.empty()) {
        throw_usage_error("invalid-task-query",
                          location + ".query has no meaningful terms.");
    }
    apply_profile_query_expansion(profile, request);
    if (request.term_groups.size() > kMaxQueryGroups) {
        throw CliError("task-query-too-complex", "resource",
                       location + ".query expands beyond 64 term groups.", 1, false,
                       "Use a narrower evidence query.");
    }
    std::size_t expanded_terms = 0;
    for (const PreparedTermGroup& group : request.prepared_term_groups) {
        if (group.size() > kMaxExpandedQueryTerms - expanded_terms) {
            throw CliError("task-query-too-complex", "resource",
                           location + ".query expands beyond 512 terms.", 1, false,
                           "Reduce profile aliases or use a narrower evidence query.");
        }
        expanded_terms += group.size();
    }
}

void append_requirement_group(std::vector<TaskRequirement>& requirements,
                              std::set<std::string>& ids,
                              const Json& task,
                              const std::string& field,
                              TaskRequirementKind kind,
                              const Profile& profile,
                              const TaskEvidenceBudget& budget) {
    const Json* values = find_json_field(task, field);
    if (values == nullptr) {
        return;
    }
    if (!values->is_array()) {
        throw_usage_error("invalid-task-requirements",
                          "task." + field + " must be an array.");
    }
    for (std::size_t index = 0; index < values->size(); ++index) {
        const Json& value = (*values)[index];
        const std::string location =
            "task." + field + "[" + std::to_string(index) + "]";
        if (!value.is_object()) {
            throw_usage_error("invalid-task-requirement",
                              location + " must be an object.");
        }
        require_only_task_fields(value,
                                 {"id", "query", "require_full_coverage",
                                  "min_covered_groups"},
                                 location);
        if (!value.contains("id") || !value.at("id").is_string()) {
            throw_usage_error("invalid-task-requirement-id",
                              location + ".id must be a string.");
        }
        if (!value.contains("query") || !value.at("query").is_string()) {
            throw_usage_error("invalid-task-query", location + ".query must be a string.");
        }
        const std::string id = value.at("id").get<std::string>();
        if (!control_free_identifier(id)) {
            throw_usage_error("invalid-task-requirement-id",
                              location + ".id must contain 1 to 128 non-control bytes.");
        }
        if (!ids.insert(id).second) {
            throw_usage_error("duplicate-task-requirement-id",
                              "Task requirement id appears more than once: " + id + ".");
        }
        QueryRequest query;
        query.query = trim(value.at("query").get<std::string>());
        query.limit = budget.per_query_records;
        query.per_doc_limit = budget.per_query_records;
        prepare_task_query(profile, query, location);

        bool require_full_coverage = false;
        if (const auto configured = value.find("require_full_coverage");
            configured != value.end()) {
            if (!configured->is_boolean()) {
                throw_usage_error("invalid-task-coverage-policy",
                                  location + ".require_full_coverage must be a boolean.");
            }
            require_full_coverage = configured->get<bool>();
        }
        int min_covered_groups = 0;
        if (value.contains("min_covered_groups")) {
            min_covered_groups = static_cast<int>(bounded_task_integer(
                value, "min_covered_groups", 0, 1,
                static_cast<long long>(query.term_groups.size()), location));
        }
        if (require_full_coverage && min_covered_groups > 0) {
            throw_usage_error(
                "conflicting-task-coverage-policy",
                location + " cannot combine require_full_coverage with min_covered_groups.");
        }

        TaskRequirement requirement;
        requirement.id = id;
        requirement.kind = kind;
        requirement.query = std::move(query);
        requirement.require_full_coverage = require_full_coverage;
        requirement.min_covered_groups = min_covered_groups;
        requirement.ordinal = requirements.size();
        requirements.push_back(std::move(requirement));
        if (requirements.size() > kMaxTaskRequirements) {
            throw CliError("task-too-large", "resource",
                           "Task contains more than 32 evidence requirements.", 1, false,
                           "Split the task into smaller context packs.");
        }
    }
}

Json canonical_requirement_json(const TaskRequirement& requirement) {
    Json canonical = {
        {"id", requirement.id},
        {"query", requirement.query.query}
    };
    if (requirement.require_full_coverage) {
        canonical["require_full_coverage"] = true;
    } else if (requirement.min_covered_groups > 0) {
        canonical["min_covered_groups"] = requirement.min_covered_groups;
    }
    return canonical;
}

Json canonical_task_request(const TaskRequest& request) {
    Json scope = Json::array();
    Json must = Json::array();
    Json should = Json::array();
    for (const TaskRequirement& requirement : request.requirements) {
        Json* target = &must;
        if (requirement.kind == TaskRequirementKind::scope) {
            target = &scope;
        } else if (requirement.kind == TaskRequirementKind::should) {
            target = &should;
        }
        target->push_back(canonical_requirement_json(requirement));
    }
    return {
        {"schema_version", "shin.task-request.v1"},
        {"request_id", request.request_id},
        {"task", {
            {"objective", request.objective},
            {"scope", std::move(scope)},
            {"must", std::move(must)},
            {"should", std::move(should)}
        }},
        {"evidence_types", request.evidence_types},
        {"budget", {
            {"max_evidence_bytes", request.budget.max_evidence_bytes},
            {"max_records", request.budget.max_records},
            {"per_query_records", request.budget.per_query_records},
            {"per_document_records", request.budget.per_document_records}
        }}
    };
}

void require_aggregate_task_bounds(const TaskRequest& request) {
    std::size_t query_bytes = 0;
    std::size_t groups = 0;
    std::size_t expanded_terms = 0;
    for (const TaskRequirement& requirement : request.requirements) {
        if (requirement.query.query.size() > kMaxTaskTotalQueryBytes - query_bytes) {
            throw CliError("task-query-budget-exceeded", "resource",
                           "Task query text exceeds the 256 KiB aggregate limit.", 1, false,
                           "Split the task into smaller context packs.");
        }
        query_bytes += requirement.query.query.size();
        groups += requirement.query.term_groups.size();
        for (const PreparedTermGroup& group : requirement.query.prepared_term_groups) {
            expanded_terms += group.size();
        }
    }
    if (groups > kMaxTaskTotalGroups || expanded_terms > kMaxTaskTotalExpandedTerms) {
        throw CliError("task-aggregate-budget-exceeded", "resource",
                       "Task exceeds its aggregate query-group or expanded-term budget.", 1,
                       false, "Use at most 512 groups and 4096 expanded terms per task.");
    }
}

}  // namespace

Json read_task_request_document(const fs::path& input, bool stdin_input) {
    if (stdin_input) {
        return parse_json_text(read_bounded_task_stream(std::cin, "standard input"),
                               "task input");
    }
    std::error_code size_error;
    const std::uintmax_t bytes = fs::file_size(input, size_error);
    if (size_error) {
        throw CliError("task-input-unavailable", "filesystem",
                       "Cannot determine task input size: " + input.string() + ".", 1, false,
                       "Provide a readable regular JSON file or use --input -.");
    }
    if (bytes > kMaxTaskInputBytes) {
        throw CliError("task-input-too-large", "resource",
                       "Task input exceeds the 1 MiB limit.", 1, false,
                       "Split the task into a smaller request document.");
    }
    std::ifstream stream(input, std::ios::binary);
    if (!stream) {
        throw CliError("task-input-unavailable", "filesystem",
                       "Cannot read task input: " + input.string() + ".", 1, false,
                       "Provide a readable regular JSON file or use --input -.");
    }
    return parse_json_text(read_bounded_task_stream(stream, input.string()), input.string());
}

TaskRequest parse_task_request(const Json& document, const Profile& profile) {
    if (!document.is_object()) {
        throw_usage_error("invalid-task-document", "Task input must be a JSON object.");
    }
    require_only_task_fields(document,
                             {"schema_version", "request_id", "task",
                              "evidence_types", "budget"},
                             "task input");
    if (json_string_value(document, "schema_version") != "shin.task-request.v1") {
        throw_usage_error("unsupported-task-schema",
                          "Task schema_version must be shin.task-request.v1.");
    }
    if (!document.contains("request_id") || !document.at("request_id").is_string()) {
        throw_usage_error("invalid-task-request-id", "request_id must be a string.");
    }
    const Json* task = find_json_field(document, "task");
    if (task == nullptr || !task->is_object()) {
        throw_usage_error("invalid-task-object", "task must be an object.");
    }
    require_only_task_fields(*task, {"objective", "scope", "must", "should"}, "task");
    if (!task->contains("objective") || !task->at("objective").is_string()) {
        throw_usage_error("invalid-task-objective", "task.objective must be a string.");
    }

    TaskRequest request;
    request.request_id = document.at("request_id").get<std::string>();
    if (!control_free_identifier(request.request_id)) {
        throw_usage_error("invalid-task-request-id",
                          "request_id must contain 1 to 128 non-control bytes.");
    }
    request.objective = trim(task->at("objective").get<std::string>());
    if (request.objective.empty() || request.objective.size() > kMaxTaskObjectiveBytes) {
        throw_usage_error("invalid-task-objective",
                          "task.objective must contain 1 to 16384 bytes after trimming.");
    }
    request.evidence_types = parse_evidence_types(document);
    request.budget = parse_task_budget(document);

    std::set<std::string> requirement_ids;
    append_requirement_group(request.requirements, requirement_ids, *task, "scope",
                             TaskRequirementKind::scope, profile, request.budget);
    append_requirement_group(request.requirements, requirement_ids, *task, "must",
                             TaskRequirementKind::must, profile, request.budget);
    append_requirement_group(request.requirements, requirement_ids, *task, "should",
                             TaskRequirementKind::should, profile, request.budget);
    const bool has_must = std::any_of(request.requirements.begin(), request.requirements.end(),
                                      [](const TaskRequirement& requirement) {
        return requirement.kind == TaskRequirementKind::must;
    });
    if (!has_must) {
        throw_usage_error("task-must-required",
                          "task.must must contain at least one evidence requirement.");
    }
    require_aggregate_task_bounds(request);
    request.canonical_document = canonical_task_request(request);
    return request;
}

std::string task_requirement_kind_name(TaskRequirementKind kind) {
    switch (kind) {
        case TaskRequirementKind::scope:
            return "scope";
        case TaskRequirementKind::must:
            return "must";
        case TaskRequirementKind::should:
            return "should";
    }
    throw std::logic_error("Unknown task requirement kind.");
}

bool task_requirement_is_required(TaskRequirementKind kind) noexcept {
    return kind == TaskRequirementKind::scope || kind == TaskRequirementKind::must;
}

}  // namespace shin::context
