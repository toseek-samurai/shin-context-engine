#include "context_internal.h"
#include "execution_evidence.h"
#include "execution_io.h"

namespace shin::context {
namespace {

bool beneath(const std::string& path, const std::string& parent) {
    return path == parent || (path.starts_with(parent) && path.size() > parent.size() && path[parent.size()] == '/');
}

void require_input_path(const std::string& path) {
    require_session_safe_text(path);
    if (path.size() > 1024 || !profile_relative_path_valid(path) || fs::path(path).generic_string() != path ||
        std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        throw_usage_error("invalid-execution-selector", "Execution selectors require canonical repository-relative paths.");
    }
    for (const auto& component : fs::path(path)) {
        const std::string name = to_lower(component.generic_string());
        if (name == ".git" || name == ".ssh" || name == ".gnupg") {
            throw_usage_error("unsafe-execution-selector", "Execution selectors cannot include private host or Git state.");
        }
    }
}

std::string coverage_of(const Json& input) {
    const std::string coverage = session_string(input, "coverage", 32);
    if (coverage != "profile" && coverage != "declared-complete" && coverage != "partial") {
        throw_usage_error("invalid-execution-coverage", "Expected profile, declared-complete or partial input coverage.");
    }
    return coverage;
}

Json parse_selectors(const Json& input) {
    const std::string coverage = coverage_of(input);
    const auto selectors = input.find("selectors");
    if (selectors == input.end() || !selectors->is_array() || selectors->size() > kMaxExecutionSelectors ||
        (coverage == "profile" ? !selectors->empty() : selectors->empty())) {
        throw_usage_error("invalid-execution-selectors", "Profile coverage has no selectors; other coverage requires 1-32 selectors.");
    }
    Json result = Json::array();
    for (const Json& selector : *selectors) {
        require_session_fields(selector, {"path", "kind"});
        const std::string path = session_string(selector, "path", 1024);
        require_input_path(path);
        const std::string kind = session_string(selector, "kind", 16);
        if (kind != "file" && kind != "tree") {
            throw_usage_error("invalid-execution-selector", "Execution selector kind must be file or tree.");
        }
        for (const Json& existing : result) {
            const std::string prior = existing.at("path").get<std::string>();
            if (path == prior || (existing.at("kind") == "tree" && beneath(path, prior)) ||
                (kind == "tree" && beneath(prior, path))) {
                throw_usage_error("overlapping-execution-selectors", "Execution selectors must be distinct and non-overlapping.");
            }
        }
        result.push_back({{"path", path}, {"kind", kind}});
    }
    std::sort(result.begin(), result.end(), [](const Json& a, const Json& b) { return a.at("path") < b.at("path"); });
    return result;
}

struct ScanBudget {
    std::size_t entries = 0;
    std::uintmax_t bytes = 0;
};

Json observe_selector(const RuntimeOptions& options, const Json& selector, ScanBudget& budget) {
    const std::string path = selector.at("path").get<std::string>();
    const std::string kind = selector.at("kind").get<std::string>();
    if (profile_path_contains_symlink(options.root, path)) {
        throw_usage_error("unsafe-execution-selector", "Execution selectors cannot traverse symlinks.");
    }
    std::error_code error;
    const auto status = fs::symlink_status(options.root / path, error);
    Json result = {{"path", path}, {"kind", kind}, {"observation", "missing"},
                   {"content_hash", nullptr}, {"files", 0}, {"directories", 0}, {"bytes", 0}};
    if (status.type() == fs::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory)) return result;
    if (error || (kind == "file" ? !fs::is_regular_file(status) : !fs::is_directory(status))) {
        throw_usage_error("unsafe-execution-selector", "Execution selector has an inaccessible or unexpected filesystem type.");
    }
    std::vector<std::pair<std::string, bool>> entries;
    entries.emplace_back(path, kind == "tree");
    if (kind == "tree") {
        fs::recursive_directory_iterator iterator(options.root / path, fs::directory_options::none, error), end;
        if (error) throw_usage_error("execution-input-unavailable", "Cannot enumerate execution input tree.");
        while (iterator != end) {
            if (iterator.depth() >= 32 || entries.size() + budget.entries >= kMaxExecutionFiles) {
                throw_usage_error("execution-input-budget", "Execution input tree exceeds its entry or depth budget.");
            }
            const std::string relative = iterator->path().lexically_relative(options.root).generic_string();
            require_input_path(relative);
            const auto child = iterator->symlink_status(error);
            if (error || fs::is_symlink(child) || (!fs::is_directory(child) && !fs::is_regular_file(child))) {
                throw_usage_error("unsafe-execution-selector", "Execution trees cannot include symlinks or special files.");
            }
            entries.emplace_back(relative, fs::is_directory(child));
            iterator.increment(error);
            if (error) throw_usage_error("execution-input-unavailable", "Cannot enumerate execution input tree.");
        }
    }
    if (entries.size() > kMaxExecutionFiles - budget.entries) {
        throw_usage_error("execution-input-budget", "Execution inputs exceed the entry budget.");
    }
    budget.entries += entries.size();
    std::sort(entries.begin(), entries.end());
    Json identities = Json::array();
    std::uintmax_t bytes = 0;
    std::size_t files = 0, directories = 0;
    for (const auto& [relative, directory] : entries) {
        if (directory) {
            ++directories;
            identities.push_back({relative, "directory"});
            continue;
        }
        const std::string content = read_execution_file(options.root, relative,
            static_cast<std::size_t>(std::min(kMaxExecutionFileBytes, kMaxExecutionTotalBytes - budget.bytes)));
        budget.bytes += content.size();
        bytes += content.size();
        ++files;
        identities.push_back({relative, "file", fnv1a64(content), content.size()});
    }
    result["observation"] = "observed";
    result["content_hash"] = fnv1a64(identities.dump());
    result["files"] = files;
    result["directories"] = directories;
    result["bytes"] = bytes;
    return result;
}

Json observe_selectors(const RuntimeOptions& options, const Json& selectors) {
    ScanBudget budget;
    Json result = Json::array();
    for (const Json& selector : selectors) result.push_back(observe_selector(options, selector, budget));
    return result;
}

Json stable_observations(const RuntimeOptions& options, const Json& selectors) {
    const auto root_metadata = file_metadata(options.root);
    Json first = observe_selectors(options, selectors);
    if (observe_selectors(options, selectors) != first || file_metadata(options.root) != root_metadata) {
        throw_file_changed_during_verification(options.root);
    }
    return first;
}

}  // namespace

Json capture_execution_inputs(const RuntimeOptions& options, const Json& request,
                              const Json& binding, std::int64_t captured_at) {
    require_session_fields(request, {"schema_version", "coverage", "selectors"});
    if (session_string(request, "schema_version", 128) != "shin.execution-input-request.v1") {
        throw_usage_error("unsupported-execution-input-request", "Expected shin.execution-input-request.v1.");
    }
    const Json selectors = parse_selectors(request);
    Json result = {{"schema_version", "shin.execution-inputs.v1"}, {"captured_at", captured_at},
                   {"coverage", coverage_of(request)}, {"binding", binding}, {"selectors", selectors},
                   {"observations", stable_observations(options, selectors)}};
    result["inputs_id"] = fnv1a64(result.dump());
    return result;
}

Json validate_execution_inputs(const Json& input) {
    require_session_fields(input, {"schema_version", "inputs_id", "captured_at", "coverage", "binding", "selectors", "observations"});
    if (session_string(input, "schema_version", 128) != "shin.execution-inputs.v1") {
        throw_usage_error("unsupported-execution-inputs", "Expected shin.execution-inputs.v1.");
    }
    session_integer(input, "captured_at", 0, kMaxSessionTimestamp);
    if (!input.contains("binding")) throw_usage_error("invalid-execution-binding", "Captured inputs require a binding.");
    parse_session_binding(input.at("binding"), true);
    const Json selectors = parse_selectors(input);
    if (selectors != input.at("selectors") || !input.contains("observations") || !input.at("observations").is_array() ||
        input.at("observations").size() != selectors.size()) {
        throw_usage_error("invalid-execution-observations", "Input observations must match the canonical selector list.");
    }
    std::uintmax_t bytes = 0, entries = 0;
    for (std::size_t i = 0; i < selectors.size(); ++i) {
        const Json& row = input.at("observations").at(i);
        require_session_fields(row, {"path", "kind", "observation", "content_hash", "files", "directories", "bytes"});
        if (!row.contains("path") || !row.contains("kind") || !row.contains("content_hash") ||
            row.at("path") != selectors.at(i).at("path") || row.at("kind") != selectors.at(i).at("kind")) {
            throw_usage_error("invalid-execution-observations", "Input observation does not match its selector.");
        }
        const std::string state = session_string(row, "observation", 32);
        const auto files = session_integer(row, "files", 0, kMaxExecutionFiles);
        const auto directories = session_integer(row, "directories", 0, kMaxExecutionFiles);
        const auto count = session_integer(row, "bytes", 0, kMaxExecutionTotalBytes);
        if (row.at("kind") == "file" && static_cast<std::uintmax_t>(count) > kMaxExecutionFileBytes) {
            throw_usage_error("execution-input-budget", "File input observation exceeds its byte budget.");
        }
        entries += static_cast<std::uintmax_t>(files + directories);
        bytes += static_cast<std::uintmax_t>(count);
        const bool missing = state == "missing";
        if (missing ? (!row.at("content_hash").is_null() || files != 0 || directories != 0 || count != 0)
                    : (state != "observed" || (row.at("kind") == "file" ? files != 1 || directories != 0 : directories < 1))) {
            throw_usage_error("invalid-execution-observations", "Input observation counts and state are inconsistent.");
        }
        if (!missing) require_session_digest(session_string(row, "content_hash", 80));
    }
    if (bytes > kMaxExecutionTotalBytes || entries > kMaxExecutionFiles) {
        throw_usage_error("execution-input-budget", "Input manifest exceeds its aggregate budget.");
    }
    Json payload = input;
    payload.erase("inputs_id");
    if (session_string(input, "inputs_id", 80) != fnv1a64(payload.dump())) {
        throw_usage_error("execution-input-integrity", "Captured execution input identity does not match its payload.");
    }
    return input;
}

Json assess_execution_inputs(const RuntimeOptions& options, const Json& input) {
    const Json manifest = validate_execution_inputs(input);
    const Json current = stable_observations(options, manifest.at("selectors"));
    Json changed = Json::array();
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current.at(i) != manifest.at("observations").at(i)) changed.push_back(current.at(i).at("path"));
    }
    return {{"coverage", manifest.at("coverage")}, {"current", changed.empty()},
            {"changed_selectors", std::move(changed)}, {"complete", true}};
}

}  // namespace shin::context
