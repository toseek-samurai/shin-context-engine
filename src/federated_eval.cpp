#include "context_internal.h"
#include "federation.h"

namespace shin::context {

namespace {

constexpr std::uintmax_t kMaxFederationEvalBytes = 1024 * 1024;
constexpr std::size_t kMaxFederationEvalCases = 128;
constexpr std::size_t kMaxFederationEvalQueryBytes = 16 * 1024;
constexpr std::size_t kMaxFederationEvalPathBytes = 4096;
constexpr int kMaxFederationEvalLimit = 64;
constexpr int kMaxFederationEvalPerDoc = 8;

struct FederatedEvalRequest {
    fs::path config_path;
    fs::path suite_path;
    bool strict = false;
    bool require_current = false;
};

struct EvidenceExpectation {
    std::string member;
    std::string path;
    std::string evidence_type;
    std::string record_type;
};

struct MemberBound {
    std::string id;
    int minimum = -1;
    int maximum = -1;
};

struct FederatedEvalCase {
    std::string id;
    std::string query;
    int limit = 8;
    int per_doc_limit = 2;
    int must_top_n = 0;
    int max_candidate_records = -1;
    int max_budgeted_out_records = -1;
    std::vector<EvidenceExpectation> must_evidence;
    std::vector<EvidenceExpectation> must_top_evidence;
    std::vector<EvidenceExpectation> must_not_evidence;
    std::vector<std::string> expected_evidence_types;
    std::vector<MemberBound> member_bounds;
};

struct FederatedEvalSuite {
    std::string id;
    std::string federation_id;
    fs::path path;
    std::string content_hash;
    std::vector<std::string> required_member_ids;
    std::vector<FederatedEvalCase> cases;
};

bool safe_id(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](char raw) {
               const unsigned char c = static_cast<unsigned char>(raw);
               return is_ascii_alnum(c) || raw == '-' || raw == '_';
           });
}

[[noreturn]] void throw_invalid_eval(const std::string& message) {
    throw CliError("invalid-federation-eval", "configuration", message, 2, false,
                   "Repair the federated eval cases file and retry.");
}

void reject_unknown_fields(const Json& object,
                           const std::set<std::string>& allowed,
                           const std::string& context) {
    if (!object.is_object()) {
        throw_invalid_eval(context + " must be a JSON object.");
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (allowed.find(field.key()) == allowed.end()) {
            throw_invalid_eval(context + " contains unknown field: " + field.key());
        }
    }
}

std::string required_string(const Json& object,
                            const std::string& field,
                            const std::string& context) {
    const Json* value = find_json_field(object, field);
    if (value == nullptr || !value->is_string()) {
        throw_invalid_eval(context + " requires string field " + field + ".");
    }
    const std::string text = value->get<std::string>();
    if (text.empty()) {
        throw_invalid_eval(context + " field " + field + " must not be empty.");
    }
    return text;
}

int bounded_integer(const Json& object,
                    const std::string& field,
                    int fallback,
                    int minimum,
                    int maximum,
                    const std::string& context) {
    const Json* value = find_json_field(object, field);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_number_integer()) {
        throw_invalid_eval(context + " field " + field + " must be an integer.");
    }
    const long long parsed = value->get<long long>();
    if (parsed < minimum || parsed > maximum) {
        throw_invalid_eval(context + " field " + field + " is outside the supported range.");
    }
    return static_cast<int>(parsed);
}

fs::path resolve_eval_file(const fs::path& input) {
    const fs::path absolute = fs::absolute(input).lexically_normal();
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(absolute, ec);
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw_invalid_eval("Federated eval cases must be a regular non-symlink file: " +
                           absolute.generic_string());
    }
    const std::uintmax_t bytes = fs::file_size(absolute, ec);
    if (ec || bytes > kMaxFederationEvalBytes) {
        throw_invalid_eval("Federated eval cases exceed the 1 MiB limit or cannot be inspected.");
    }
    return fs::weakly_canonical(absolute);
}

FederatedEvalRequest parse_request(const std::vector<std::string>& args) {
    FederatedEvalRequest request;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" || args[i] == "--suite") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", args[i] + " requires a value.");
            }
            fs::path& target = args[i] == "--config" ? request.config_path : request.suite_path;
            if (!target.empty()) {
                throw_usage_error("duplicate-option", args[i] + " may be provided only once.");
            }
            target = fs::absolute(args[++i]).lexically_normal();
        } else if (args[i] == "--strict") {
            if (request.strict) {
                throw_usage_error("duplicate-option", "--strict may be provided only once.");
            }
            request.strict = true;
        } else if (args[i] == "--require-current") {
            if (request.require_current) {
                throw_usage_error("duplicate-option", "--require-current may be provided only once.");
            }
            request.require_current = true;
        } else {
            throw_usage_error("unsupported-option", "Unsupported federated-eval option: " + args[i]);
        }
    }
    if (request.config_path.empty()) {
        throw_usage_error("missing-option-value", "federated-eval requires --config FILE.");
    }
    if (request.suite_path.empty()) {
        throw_usage_error("missing-option-value", "federated-eval requires --suite FILE.");
    }
    return request;
}

std::vector<EvidenceExpectation> parse_expectations(const Json& object,
                                                    const std::string& field,
                                                    const std::string& context) {
    const Json* values = find_json_field(object, field);
    if (values == nullptr) {
        return {};
    }
    if (!values->is_array()) {
        throw_invalid_eval(context + " field " + field + " must be an array.");
    }
    std::vector<EvidenceExpectation> expectations;
    std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
    for (const Json& value : *values) {
        reject_unknown_fields(value, {"member", "path", "evidenceType", "recordType"},
                              context + " evidence expectation");
        EvidenceExpectation expectation;
        expectation.member = required_string(value, "member", context + " evidence expectation");
        expectation.path = required_string(value, "path", context + " evidence expectation");
        expectation.evidence_type = to_lower(json_string_value(value, "evidenceType"));
        expectation.record_type = to_lower(json_string_value(value, "recordType"));
        const fs::path expected_path(expectation.path);
        const bool parent_traversal =
            std::any_of(expected_path.begin(), expected_path.end(), [](const fs::path& component) {
                return component == "..";
            });
        if (!safe_id(expectation.member) || expectation.path.size() > kMaxFederationEvalPathBytes ||
            expected_path.is_absolute() || parent_traversal) {
            throw_invalid_eval(context + " contains an invalid evidence member or path.");
        }
        const std::set<std::string> allowed_types =
            {"", "contract", "decision", "runbook", "fact", "section"};
        if (allowed_types.find(expectation.evidence_type) == allowed_types.end()) {
            throw_invalid_eval(context + " contains an invalid evidenceType.");
        }
        if (expectation.record_type.size() > 128) {
            throw_invalid_eval(context + " contains an oversized recordType.");
        }
        if (!seen.insert({expectation.member, expectation.path, expectation.evidence_type,
                          expectation.record_type}).second) {
            throw_invalid_eval(context + " contains a duplicate evidence expectation.");
        }
        expectations.push_back(std::move(expectation));
    }
    return expectations;
}

std::vector<std::string> parse_required_members(const Json& root) {
    const Json* values = find_json_field(root, "requiredMemberIds");
    if (values == nullptr || !values->is_array() || values->empty()) {
        throw_invalid_eval("Federated eval suite requires non-empty requiredMemberIds.");
    }
    std::vector<std::string> members;
    std::set<std::string> seen;
    for (const Json& value : *values) {
        if (!value.is_string()) {
            throw_invalid_eval("Federated eval requiredMemberIds must contain strings.");
        }
        const std::string id = value.get<std::string>();
        if (!safe_id(id) || !seen.insert(id).second) {
            throw_invalid_eval("Federated eval requiredMemberIds contain an invalid or duplicate id.");
        }
        members.push_back(id);
    }
    std::sort(members.begin(), members.end());
    return members;
}

std::vector<std::string> parse_evidence_types(const Json& object, const std::string& context) {
    const Json* values = find_json_field(object, "expectedEvidenceTypes");
    if (values == nullptr) {
        return {};
    }
    if (!values->is_array()) {
        throw_invalid_eval(context + " field expectedEvidenceTypes must be an array.");
    }
    const std::set<std::string> allowed = {"contract", "decision", "runbook", "fact", "section"};
    std::vector<std::string> types;
    std::set<std::string> seen;
    for (const Json& value : *values) {
        if (!value.is_string()) {
            throw_invalid_eval(context + " expected evidence types must be strings.");
        }
        const std::string type = to_lower(value.get<std::string>());
        if (allowed.find(type) == allowed.end()) {
            throw_invalid_eval(context + " contains unsupported evidence type: " + type);
        }
        if (!seen.insert(type).second) {
            throw_invalid_eval(context + " contains duplicate evidence type: " + type);
        }
        types.push_back(type);
    }
    return types;
}

std::vector<MemberBound> parse_member_bounds(const Json& object, const std::string& context) {
    const Json* values = find_json_field(object, "memberBounds");
    if (values == nullptr) {
        return {};
    }
    if (!values->is_array()) {
        throw_invalid_eval(context + " field memberBounds must be an array.");
    }
    std::vector<MemberBound> bounds;
    std::set<std::string> seen;
    for (const Json& value : *values) {
        reject_unknown_fields(value, {"id", "minReturnedRecords", "maxReturnedRecords"},
                              context + " member bound");
        MemberBound bound;
        bound.id = required_string(value, "id", context + " member bound");
        if (!safe_id(bound.id) || !seen.insert(bound.id).second) {
            throw_invalid_eval(context + " contains an invalid or duplicate member bound id.");
        }
        bound.minimum = bounded_integer(value, "minReturnedRecords", -1, 0,
                                        kMaxFederationEvalLimit, context + " member bound");
        bound.maximum = bounded_integer(value, "maxReturnedRecords", -1, 0,
                                        kMaxFederationEvalLimit, context + " member bound");
        if (bound.minimum < 0 && bound.maximum < 0) {
            throw_invalid_eval(context + " member bound requires a minimum or maximum.");
        }
        if (bound.minimum >= 0 && bound.maximum >= 0 && bound.minimum > bound.maximum) {
            throw_invalid_eval(context + " member bound minimum exceeds maximum.");
        }
        bounds.push_back(std::move(bound));
    }
    return bounds;
}

FederatedEvalSuite load_suite(const fs::path& input) {
    FederatedEvalSuite suite;
    suite.path = resolve_eval_file(input);
    const std::string suite_text = read_file(suite.path);
    suite.content_hash = fnv1a64(suite_text);
    const Json root = parse_json_text(suite_text, suite.path.string());
    reject_unknown_fields(root,
                          {"schemaVersion", "id", "federationId", "requiredMemberIds", "cases"},
                          "Federated eval suite");
    if (json_string_value(root, "schemaVersion") != "shin.federated-eval-suite.v1") {
        throw_invalid_eval("Federated eval schemaVersion must be shin.federated-eval-suite.v1.");
    }
    suite.id = required_string(root, "id", "Federated eval suite");
    suite.federation_id = required_string(root, "federationId", "Federated eval suite");
    if (!safe_id(suite.id) || !safe_id(suite.federation_id)) {
        throw_invalid_eval("Federated eval suite and federation ids must be bounded safe identifiers.");
    }
    suite.required_member_ids = parse_required_members(root);
    const Json* cases = find_json_field(root, "cases");
    if (cases == nullptr || !cases->is_array() || cases->empty() ||
        cases->size() > kMaxFederationEvalCases) {
        throw_invalid_eval("Federated eval cases must contain 1-128 entries.");
    }

    std::set<std::string> case_ids;
    for (const Json& value : *cases) {
        reject_unknown_fields(value,
                              {"id", "query", "limit", "perDocLimit", "mustEvidence",
                               "mustTopEvidence", "mustTopN", "mustNotEvidence",
                               "expectedEvidenceTypes", "memberBounds", "maxCandidateRecords",
                               "maxBudgetedOutRecords"},
                              "Federated eval case");
        FederatedEvalCase item;
        item.id = required_string(value, "id", "Federated eval case");
        item.query = trim(required_string(value, "query", "Federated eval case"));
        if (!safe_id(item.id) || !case_ids.insert(item.id).second) {
            throw_invalid_eval("Federated eval case ids must be unique bounded safe identifiers.");
        }
        if (item.query.empty()) {
            throw_invalid_eval("Federated eval query must not contain only whitespace.");
        }
        if (item.query.size() > kMaxFederationEvalQueryBytes) {
            throw_invalid_eval("Federated eval query exceeds 16 KiB.");
        }
        item.limit = bounded_integer(value, "limit", item.limit, 1,
                                     kMaxFederationEvalLimit, "Federated eval case");
        item.per_doc_limit = bounded_integer(value, "perDocLimit", item.per_doc_limit, 1,
                                             kMaxFederationEvalPerDoc, "Federated eval case");
        item.must_top_n = bounded_integer(value, "mustTopN", item.limit, 1, item.limit,
                                          "Federated eval case");
        item.max_candidate_records = bounded_integer(value, "maxCandidateRecords", -1, 0,
                                                      kMaxFederationEvalLimit * 16,
                                                      "Federated eval case");
        item.max_budgeted_out_records = bounded_integer(value, "maxBudgetedOutRecords", -1, 0,
                                                         kMaxFederationEvalLimit * 16,
                                                         "Federated eval case");
        item.must_evidence = parse_expectations(value, "mustEvidence", "Federated eval case");
        item.must_top_evidence =
            parse_expectations(value, "mustTopEvidence", "Federated eval case");
        item.must_not_evidence =
            parse_expectations(value, "mustNotEvidence", "Federated eval case");
        item.expected_evidence_types = parse_evidence_types(value, "Federated eval case");
        item.member_bounds = parse_member_bounds(value, "Federated eval case");
        for (const MemberBound& bound : item.member_bounds) {
            if ((bound.minimum >= 0 && bound.minimum > item.limit) ||
                (bound.maximum >= 0 && bound.maximum > item.limit)) {
                throw_invalid_eval("Federated eval member bounds must not exceed the case limit.");
            }
        }
        const bool has_assertions = !item.must_evidence.empty() || !item.must_top_evidence.empty() ||
                                    !item.must_not_evidence.empty() ||
                                    !item.expected_evidence_types.empty() ||
                                    !item.member_bounds.empty() || item.max_candidate_records >= 0 ||
                                    item.max_budgeted_out_records >= 0;
        if (!has_assertions) {
            throw_invalid_eval("Every federated eval case requires at least one assertion.");
        }
        suite.cases.push_back(std::move(item));
    }
    return suite;
}

bool array_contains_string(const Json& array, const std::string& expected) {
    return array.is_array() &&
           std::any_of(array.begin(), array.end(), [&](const Json& value) {
               return value.is_string() && value.get<std::string>() == expected;
           });
}

bool evidence_matches(const Json& evidence, const EvidenceExpectation& expectation) {
    const Json* members = find_json_field(evidence, "member_ids");
    const Json* hit = find_json_field(evidence, "hit");
    if (members == nullptr || hit == nullptr || !hit->is_object() ||
        !array_contains_string(*members, expectation.member) ||
        json_string_value(*hit, "path") != expectation.path) {
        return false;
    }
    if (!expectation.evidence_type.empty() &&
        json_string_value(evidence, "evidence_type") != expectation.evidence_type) {
        return false;
    }
    return expectation.record_type.empty() ||
           to_lower(json_string_value(*hit, "record_type")) == expectation.record_type;
}

Json expectation_document(const EvidenceExpectation& expectation) {
    Json output = {{"member", expectation.member}, {"path", expectation.path}};
    if (!expectation.evidence_type.empty()) {
        output["evidence_type"] = expectation.evidence_type;
    }
    if (!expectation.record_type.empty()) {
        output["record_type"] = expectation.record_type;
    }
    return output;
}

Json expectation_array(const std::vector<EvidenceExpectation>& expectations) {
    Json output = Json::array();
    for (const EvidenceExpectation& expectation : expectations) {
        output.push_back(expectation_document(expectation));
    }
    return output;
}

std::vector<EvidenceExpectation> missing_expectations(
    const Json& evidence,
    const std::vector<EvidenceExpectation>& expected,
    int top_n = -1) {
    std::vector<EvidenceExpectation> missing;
    for (const EvidenceExpectation& expectation : expected) {
        bool found = false;
        const std::size_t bound = top_n < 0
                                      ? evidence.size()
                                      : std::min(evidence.size(), static_cast<std::size_t>(top_n));
        for (std::size_t index = 0; index < bound; ++index) {
            if (evidence_matches(evidence[index], expectation)) {
                found = true;
                break;
            }
        }
        if (!found) {
            missing.push_back(expectation);
        }
    }
    return missing;
}

Json returned_evidence_summary(const Json& evidence) {
    Json output = Json::array();
    for (const Json& item : evidence) {
        const Json* hit = find_json_field(item, "hit");
        output.push_back({
            {"global_rank", item.value("global_rank", 0)},
            {"primary_member", json_string_value(item, "primary_member")},
            {"member_ids", item.value("member_ids", Json::array())},
            {"evidence_type", json_string_value(item, "evidence_type")},
            {"path", hit != nullptr && hit->is_object() ? json_string_value(*hit, "path") : ""},
            {"record_type", hit != nullptr && hit->is_object()
                                ? json_string_value(*hit, "record_type") : ""}
        });
    }
    return output;
}

void validate_member_references(const FederatedEvalSuite& suite,
                                const std::vector<std::string>& member_ids) {
    const std::set<std::string> known(member_ids.begin(), member_ids.end());
    for (const FederatedEvalCase& item : suite.cases) {
        for (const std::vector<EvidenceExpectation>* expectations :
             {&item.must_evidence, &item.must_top_evidence, &item.must_not_evidence}) {
            for (const EvidenceExpectation& expectation : *expectations) {
                if (known.find(expectation.member) == known.end()) {
                    throw_invalid_eval("Case " + item.id + " references unknown member: " +
                                       expectation.member);
                }
            }
        }
        for (const MemberBound& bound : item.member_bounds) {
            if (known.find(bound.id) == known.end()) {
                throw_invalid_eval("Case " + item.id + " references unknown member bound: " +
                                   bound.id);
            }
        }
    }
}

Json evaluate_case(const FederationSession& session,
                   const FederatedEvalCase& item,
                   bool exact_freshness) {
    FederationQueryOptions options;
    options.query = item.query;
    options.limit = item.limit;
    options.per_doc_limit = item.per_doc_limit;
    options.verify_current = exact_freshness;
    const Json brief = session.brief(options);
    const Json& evidence = brief.at("evidence");
    const Json& members = brief.at("members");
    const Json& budget = brief.at("evidence_budget");
    const Json& freshness = brief.at("index_freshness");

    const std::vector<EvidenceExpectation> missing =
        missing_expectations(evidence, item.must_evidence);
    const std::vector<EvidenceExpectation> missing_top =
        missing_expectations(evidence, item.must_top_evidence, item.must_top_n);
    std::vector<EvidenceExpectation> unexpected;
    for (const EvidenceExpectation& expectation : item.must_not_evidence) {
        if (missing_expectations(evidence, {expectation}).empty()) {
            unexpected.push_back(expectation);
        }
    }

    std::set<std::string> returned_types;
    for (const Json& value : evidence) {
        returned_types.insert(json_string_value(value, "evidence_type"));
    }
    std::vector<std::string> missing_types;
    for (const std::string& expected : item.expected_evidence_types) {
        if (returned_types.find(expected) == returned_types.end()) {
            missing_types.push_back(expected);
        }
    }

    Json member_results = Json::array();
    std::map<std::string, int> returned_by_member;
    for (const Json& member : members) {
        const std::string id = json_string_value(member, "id");
        const int returned = member.value("returned_records", 0);
        returned_by_member[id] = returned;
        member_results.push_back({
            {"id", id},
            {"candidate_records", member.value("candidate_records", 0)},
            {"returned_records", returned},
            {"retrieval_mode", json_string_value(member, "retrieval_mode")}
        });
    }
    Json member_bound_violations = Json::array();
    for (const MemberBound& bound : item.member_bounds) {
        const int actual = returned_by_member[bound.id];
        if ((bound.minimum >= 0 && actual < bound.minimum) ||
            (bound.maximum >= 0 && actual > bound.maximum)) {
            member_bound_violations.push_back({
                {"id", bound.id},
                {"actual", actual},
                {"minimum", bound.minimum},
                {"maximum", bound.maximum}
            });
        }
    }

    Json ceiling_violations = Json::array();
    const int candidate_records = budget.value("candidate_records", 0);
    const int budgeted_out_records = budget.value("budgeted_out_records", 0);
    if (item.max_candidate_records >= 0 && candidate_records > item.max_candidate_records) {
        ceiling_violations.push_back({
            {"field", "candidate_records"},
            {"actual", candidate_records},
            {"maximum", item.max_candidate_records}
        });
    }
    if (item.max_budgeted_out_records >= 0 &&
        budgeted_out_records > item.max_budgeted_out_records) {
        ceiling_violations.push_back({
            {"field", "budgeted_out_records"},
            {"actual", budgeted_out_records},
            {"maximum", item.max_budgeted_out_records}
        });
    }
    const bool freshness_ok = !exact_freshness ||
                              (freshness.value("current", false) &&
                               freshness.value("fully_verified", false));
    const bool passed = missing.empty() && missing_top.empty() && unexpected.empty() &&
                        missing_types.empty() && member_bound_violations.empty() &&
                        ceiling_violations.empty() && freshness_ok;

    Json result = {
        {"id", item.id},
        {"query", item.query},
        {"limit", item.limit},
        {"per_doc_limit", item.per_doc_limit},
        {"passed", passed},
        {"snapshot_set_id", brief.at("federation").at("snapshot_set_id")},
        {"index_freshness", freshness},
        {"evidence_budget", budget},
        {"member_results", member_results},
        {"returned_evidence", returned_evidence_summary(evidence)},
        {"must_evidence", expectation_array(item.must_evidence)},
        {"must_top_evidence", expectation_array(item.must_top_evidence)},
        {"must_top_n", item.must_top_n},
        {"must_not_evidence", expectation_array(item.must_not_evidence)},
        {"expected_evidence_types", item.expected_evidence_types},
        {"missing_evidence", expectation_array(missing)},
        {"missing_top_evidence", expectation_array(missing_top)},
        {"unexpected_evidence", expectation_array(unexpected)},
        {"missing_evidence_types", missing_types},
        {"member_bound_violations", member_bound_violations},
        {"max_candidate_records", item.max_candidate_records},
        {"max_budgeted_out_records", item.max_budgeted_out_records},
        {"ceiling_violations", ceiling_violations},
        {"freshness_violation", !freshness_ok}
    };
    return result;
}

}  // namespace

int run_federated_eval(const std::vector<std::string>& args) {
    const FederatedEvalRequest request = parse_request(args);
    const FederatedEvalSuite suite = load_suite(request.suite_path);
    Json output;
    bool ok = false;
    {
        FederationSession session(request.config_path);
        if (suite.federation_id != session.id()) {
            throw_invalid_eval("Federated eval federationId does not match the selected federation config.");
        }
        if (suite.required_member_ids != session.member_ids()) {
            throw_invalid_eval("Federated eval requiredMemberIds do not match the selected federation config.");
        }
        validate_member_references(suite, session.member_ids());

        const bool exact_freshness = request.strict || request.require_current;
        Json case_results = Json::array();
        std::set<std::string> snapshot_set_ids;
        int passed = 0;
        for (const FederatedEvalCase& item : suite.cases) {
            Json result = evaluate_case(session, item, exact_freshness);
            if (result.value("passed", false)) {
                ++passed;
            }
            snapshot_set_ids.insert(json_string_value(result, "snapshot_set_id"));
            case_results.push_back(std::move(result));
        }
        const int total = static_cast<int>(case_results.size());
        const int failed = total - passed;
        const bool stable_snapshot_set = snapshot_set_ids.size() == 1;
        ok = total > 0 && failed == 0 && stable_snapshot_set;

        output = make_result_document("shin.federated-eval.v1", "federated-eval", ok);
        output["federation"] = {
            {"id", session.id()},
            {"config", session.config_path().generic_string()},
            {"config_hash", session.config_hash()},
            {"member_ids", session.member_ids()},
            {"snapshot_set_id", stable_snapshot_set ? *snapshot_set_ids.begin() : ""},
            {"stable_snapshot_set", stable_snapshot_set}
        };
        output["suite"] = {
            {"id", suite.id},
            {"path", suite.path.generic_string()},
            {"content_hash", suite.content_hash},
            {"schema_version", "shin.federated-eval-suite.v1"}
        };
        output["strict"] = request.strict;
        output["require_current"] = request.require_current;
        output["summary"] = {{"total", total}, {"passed", passed}, {"failed", failed}};
        output["cases"] = std::move(case_results);
    }
    emit_json_document(std::cout, output);
    return ok ? 0 : 1;
}

}  // namespace shin::context
