#include "context_internal.h"
#include "federation.h"

namespace shin::context {

namespace {

constexpr std::size_t kMaxFederationQueryBytes = 16 * 1024;
constexpr int kMaxFederationLimit = 64;
constexpr int kMaxFederationPerDoc = 8;

struct FederatedBriefRequest {
    fs::path config_path;
    FederationQueryOptions query;
};

FederatedBriefRequest parse_federated_brief_request(const std::vector<std::string>& args) {
    FederatedBriefRequest request;
    std::vector<std::string> query_parts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--config requires a value.");
            }
            if (!request.config_path.empty()) {
                throw_usage_error("duplicate-option", "--config may be provided only once.");
            }
            request.config_path = fs::absolute(args[++i]).lexically_normal();
        } else if (args[i] == "--limit") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--limit requires a value.");
            }
            request.query.limit = parse_cli_integer(args[++i], "--limit", 1);
        } else if (args[i] == "--per-doc") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--per-doc requires a value.");
            }
            request.query.per_doc_limit = parse_cli_integer(args[++i], "--per-doc", 1);
        } else if (args[i] == "--require-current") {
            request.query.require_current = true;
            request.query.verify_current = true;
        } else if (starts_with(args[i], "--")) {
            throw_usage_error("unsupported-option",
                              "Unsupported federated-brief option: " + args[i]);
        } else {
            query_parts.push_back(args[i]);
        }
    }
    if (request.config_path.empty()) {
        throw_usage_error("missing-option-value", "federated-brief requires --config FILE.");
    }
    if (request.query.limit > kMaxFederationLimit) {
        throw_usage_error("invalid-option-value", "--limit must not exceed 64.");
    }
    if (request.query.per_doc_limit > kMaxFederationPerDoc) {
        throw_usage_error("invalid-option-value", "--per-doc must not exceed 8.");
    }
    for (const std::string& part : query_parts) {
        if (!request.query.query.empty()) {
            request.query.query.push_back(' ');
        }
        request.query.query += part;
    }
    request.query.query = trim(std::move(request.query.query));
    if (request.query.query.empty()) {
        throw_usage_error("missing-query", "A non-empty federated query is required.");
    }
    if (request.query.query.size() > kMaxFederationQueryBytes) {
        throw_usage_error("invalid-option-value", "Federated query must not exceed 16 KiB.");
    }
    return request;
}

}  // namespace

int run_federated_brief(const std::vector<std::string>& args) {
    const FederatedBriefRequest request = parse_federated_brief_request(args);
    FederationSession session(request.config_path);
    emit_json_document(std::cout, session.brief(request.query));
    return 0;
}

}  // namespace shin::context
