#include "context_internal.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaxBatchInputBytes = 1024U * 1024U;
constexpr std::size_t kMaxBatchRequests = 64;
constexpr std::size_t kMaxBatchRequestIdBytes = 128;
constexpr std::size_t kMaxBatchQueryBytes = 16384;
constexpr std::size_t kMaxBatchTotalQueryBytes = 256U * 1024U;
constexpr std::size_t kMaxBatchTotalGroups = 512;
constexpr std::size_t kMaxBatchTotalExpandedTerms = 4096;
constexpr int kMaxBatchTotalRequestedRecords = 1024;
constexpr std::size_t kMaxBatchOutputBytes = 16U * 1024U * 1024U;
constexpr int kMaxBatchLimit = 64;
constexpr int kMaxBatchPerDocumentLimit = 8;
constexpr int kMaxBatchJobs = 8;

struct BatchRequest {
    std::string request_id;
    std::string command;
    QueryRequest query;
};

struct BatchOptions {
    fs::path input;
    bool stdin_input = false;
    bool require_current = false;
    int jobs = 0;
};

std::string read_bounded_stream(std::istream& input, std::size_t maximum_bytes,
                                const std::string& label) {
    std::string content;
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const std::size_t bytes = static_cast<std::size_t>(count);
            if (content.size() > maximum_bytes || bytes > maximum_bytes - content.size()) {
                throw CliError("batch-input-too-large", "resource",
                               label + " exceeds the 1 MiB batch input limit.", 1, false,
                               "Split the work into smaller batch request documents.");
            }
            content.append(buffer.data(), bytes);
        }
    }
    if (input.bad()) {
        throw CliError("batch-input-read-failed", "filesystem", "Cannot read " + label + ".", 1,
                       true, "Verify the input path and retry.");
    }
    return content;
}

std::string read_batch_input(const BatchOptions& options) {
    if (options.stdin_input) {
        return read_bounded_stream(std::cin, kMaxBatchInputBytes, "standard input");
    }
    std::error_code size_error;
    const std::uintmax_t bytes = fs::file_size(options.input, size_error);
    if (size_error) {
        throw CliError("batch-input-unavailable", "filesystem",
                       "Cannot determine batch input size: " + options.input.string(), 1, false,
                       "Provide a readable regular JSON file or use --input -.");
    }
    if (bytes > kMaxBatchInputBytes) {
        throw CliError("batch-input-too-large", "resource",
                       "Batch input exceeds the 1 MiB limit.", 1, false,
                       "Split the work into smaller batch request documents.");
    }
    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw CliError("batch-input-unavailable", "filesystem",
                       "Cannot read batch input: " + options.input.string(), 1, false,
                       "Provide a readable regular JSON file or use --input -.");
    }
    return read_bounded_stream(input, kMaxBatchInputBytes, options.input.string());
}

BatchOptions parse_batch_options(const std::vector<std::string>& args) {
    BatchOptions options;
    bool input_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--input") {
            if (input_seen || index + 1 >= args.size()) {
                throw_usage_error(input_seen ? "duplicate-option" : "missing-option-value",
                                  input_seen ? "--input may be supplied only once."
                                             : "--input requires a file path or -.");
            }
            const std::string& value = args[++index];
            options.stdin_input = value == "-";
            if (!options.stdin_input) {
                options.input = fs::absolute(value);
            }
            input_seen = true;
        } else if (args[index] == "--require-current") {
            options.require_current = true;
        } else if (args[index] == "--jobs") {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--jobs requires a value.");
            }
            options.jobs = parse_cli_integer(args[++index], "--jobs", 1);
            if (options.jobs > kMaxBatchJobs) {
                throw_usage_error("invalid-option-value", "--jobs must not exceed 8.");
            }
        } else {
            throw_usage_error("unsupported-option",
                              "batch only accepts --input, --jobs, and --require-current.");
        }
    }
    if (!input_seen) {
        throw_usage_error("missing-option", "batch requires --input FILE or --input -.");
    }
    return options;
}

void require_only_fields(const Json& object, const std::set<std::string>& allowed,
                         const std::string& location) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (allowed.find(field.key()) == allowed.end()) {
            throw_usage_error("unknown-batch-field", location + " has unknown field: " + field.key() + ".");
        }
    }
}

int bounded_json_integer(const Json& object, const std::string& field, int fallback,
                         int minimum, int maximum, const std::string& location) {
    const auto value = object.find(field);
    if (value == object.end()) {
        return fallback;
    }
    if (!value->is_number_integer()) {
        throw_usage_error("invalid-batch-field", location + "." + field + " must be an integer.");
    }
    long long parsed = 0;
    try {
        parsed = value->get<long long>();
    } catch (const Json::exception&) {
        throw_usage_error("invalid-batch-field", location + "." + field + " is outside the integer range.");
    }
    if (parsed < minimum || parsed > maximum) {
        throw_usage_error("invalid-batch-field",
                          location + "." + field + " must be between " +
                              std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    }
    return static_cast<int>(parsed);
}

bool request_id_valid(const std::string& value) {
    if (value.empty() || value.size() > kMaxBatchRequestIdBytes) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20U || c == 0x7fU;
    });
}

void require_bounded_query(const Profile& profile, QueryRequest& request,
                           const std::string& location) {
    if (request.query.empty()) {
        throw_usage_error("invalid-batch-query", location + ".query must be non-empty.");
    }
    if (request.query.size() > kMaxBatchQueryBytes) {
        throw CliError("batch-query-too-large", "resource",
                       location + ".query exceeds 16384 bytes.", 1, false,
                       "Split the task into smaller retrieval queries.");
    }
    request.terms = parse_terms(request.query);
    if (request.terms.empty()) {
        throw_usage_error("invalid-batch-query", location + ".query has no meaningful terms.");
    }
    apply_profile_query_expansion(profile, request);
    if (request.term_groups.size() > kMaxQueryGroups) {
        throw CliError("batch-query-too-complex", "resource",
                       location + ".query expands beyond 64 term groups.", 1, false,
                       "Use a narrower retrieval query.");
    }
    std::size_t expanded_terms = 0;
    for (const PreparedTermGroup& group : request.prepared_term_groups) {
        if (group.size() > kMaxExpandedQueryTerms - expanded_terms) {
            throw CliError("batch-query-too-complex", "resource",
                           location + ".query expands beyond 512 terms.", 1, false,
                           "Reduce profile aliases or use a narrower retrieval query.");
        }
        expanded_terms += group.size();
    }
}

std::vector<BatchRequest> parse_batch_requests(const Json& root, const Profile& profile) {
    if (!root.is_object()) {
        throw_usage_error("invalid-batch-document", "Batch input must be a JSON object.");
    }
    require_only_fields(root, {"schema_version", "requests"}, "batch input");
    if (json_string_value(root, "schema_version") != "shin.batch-request.v1") {
        throw_usage_error("unsupported-batch-schema",
                          "Batch schema_version must be shin.batch-request.v1.");
    }
    const Json* requests = find_json_field(root, "requests");
    if (requests == nullptr || !requests->is_array() || requests->empty()) {
        throw_usage_error("invalid-batch-requests", "Batch requests must be a non-empty array.");
    }
    if (requests->size() > kMaxBatchRequests) {
        throw CliError("batch-too-large", "resource", "Batch contains more than 64 requests.", 1,
                       false, "Split the work into smaller batch request documents.");
    }

    std::vector<BatchRequest> parsed;
    parsed.reserve(requests->size());
    std::set<std::string> request_ids;
    std::size_t total_query_bytes = 0;
    std::size_t total_groups = 0;
    std::size_t total_expanded_terms = 0;
    int total_requested_records = 0;
    for (std::size_t index = 0; index < requests->size(); ++index) {
        const Json& object = (*requests)[index];
        const std::string location = "requests[" + std::to_string(index) + "]";
        if (!object.is_object()) {
            throw_usage_error("invalid-batch-request", location + " must be an object.");
        }
        require_only_fields(object, {"request_id", "command", "query", "limit", "per_doc_limit"},
                            location);
        const std::string request_id = json_string_value(object, "request_id");
        const std::string command = json_string_value(object, "command");
        const std::string query = json_string_value(object, "query");
        if (!request_id_valid(request_id)) {
            throw_usage_error("invalid-batch-request-id",
                              location + ".request_id must contain 1 to 128 non-control bytes.");
        }
        if (!request_ids.insert(request_id).second) {
            throw_usage_error("duplicate-batch-request-id",
                              "Batch request_id appears more than once: " + request_id + ".");
        }
        if (command != "search" && command != "context" && command != "brief") {
            throw_usage_error("unsupported-batch-command",
                              location + ".command must be search, context, or brief.");
        }
        if (!object.contains("query") || !object.at("query").is_string()) {
            throw_usage_error("invalid-batch-query", location + ".query must be a string.");
        }
        const int default_limit = command == "search" ? 10 : (command == "context" ? 8 : 6);
        const int default_per_doc = command == "context" ? 3 : (command == "brief" ? 2 : 0);
        QueryRequest request;
        request.query = trim(query);
        request.limit = bounded_json_integer(object, "limit", default_limit, 1, kMaxBatchLimit, location);
        if (command == "search" && object.contains("per_doc_limit")) {
            throw_usage_error("invalid-batch-field",
                              location + ".per_doc_limit is not valid for search.");
        }
        request.per_doc_limit = command == "search"
                                    ? 0
                                    : bounded_json_integer(object, "per_doc_limit", default_per_doc,
                                                           1, kMaxBatchPerDocumentLimit, location);
        require_bounded_query(profile, request, location);
        if (request.query.size() > kMaxBatchTotalQueryBytes - total_query_bytes) {
            throw CliError("batch-query-budget-exceeded", "resource",
                           "Batch query text exceeds the 256 KiB aggregate limit.", 1, false,
                           "Split the work into smaller batch request documents.");
        }
        total_query_bytes += request.query.size();
        total_groups += request.term_groups.size();
        for (const PreparedTermGroup& group : request.prepared_term_groups) {
            total_expanded_terms += group.size();
        }
        total_requested_records += request.limit;
        if (total_groups > kMaxBatchTotalGroups ||
            total_expanded_terms > kMaxBatchTotalExpandedTerms ||
            total_requested_records > kMaxBatchTotalRequestedRecords) {
            throw CliError("batch-aggregate-budget-exceeded", "resource",
                           "Batch exceeds its aggregate group, term, or result budget.", 1, false,
                           "Use at most 512 groups, 4096 expanded terms, and 1024 requested records per batch.");
        }
        parsed.push_back({request_id, command, std::move(request)});
    }
    return parsed;
}

Json execute_batch_request(const RetrievalSession& session, const BatchRequest& request) {
    if (request.command == "search") {
        return session.search(request.query);
    }
    if (request.command == "context") {
        return session.context(request.query);
    }
    return session.brief(request.query);
}

}  // namespace

int run_batch(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const BatchOptions batch_options = parse_batch_options(args);
    const Json input = parse_json_text(read_batch_input(batch_options), "batch input");
    const std::vector<BatchRequest> requests = parse_batch_requests(input, options.profile);
    Json output;
    {
        RetrievalSession session(options, batch_options.require_current);
        const unsigned int automatic_jobs = std::min<unsigned int>(
            static_cast<unsigned int>(kMaxBatchJobs), worker_count_for_items(requests.size(), 1));
        const unsigned int jobs = static_cast<unsigned int>(
            batch_options.jobs > 0 ? batch_options.jobs : std::max(1U, automatic_jobs));
        std::vector<Json> results(requests.size());
        parallel_for_worker_indices(requests.size(), std::min<unsigned int>(jobs, requests.size()),
                                    [&](unsigned int, std::size_t index) {
            results[index] = {
                {"request_id", requests[index].request_id},
                {"result", execute_batch_request(session, requests[index])}
            };
        });

        output = make_result_document("shin.batch.v1", "batch", true);
        output["profile"] = options.profile.id;
        output["root"] = options.root.generic_string();
        output["out_root"] = options.out_root.generic_string();
        output["require_current"] = batch_options.require_current;
        output["snapshot_id"] = session.freshness().snapshot_id;
        output["index_freshness"] = index_freshness_json(session.freshness());
        output["freshness_scope"] = "session-open";
        output["execution"] = {
            {"snapshot_pinned", true},
            {"request_count", requests.size()},
            {"jobs", std::min<unsigned int>(jobs, requests.size())},
            {"result_order", "input-order"}
        };
        output["results"] = std::move(results);
    }
    const std::string serialized = output.dump(2);
    if (serialized.size() > kMaxBatchOutputBytes) {
        throw CliError("batch-output-too-large", "resource",
                       "Batch output exceeds the 16 MiB serialized limit.", 1, false,
                       "Reduce request count or per-request limits.");
    }
    std::cout << serialized << '\n';
    return 0;
}

}  // namespace shin::context
