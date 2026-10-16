#include "context_internal.h"
#include "session_contract.h"
#include "generated_snapshot.h"

namespace shin::context {
namespace {

std::string read_bounded_session(std::istream& stream) {
    std::string text;
    std::array<char, 8192> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count > kMaxSessionInputBytes - text.size()) {
            throw CliError("session-input-too-large", "resource",
                           "Session input exceeds 256 KiB.", 1, false,
                           "Keep only bounded task state and references.");
        }
        text.append(buffer.data(), count);
    }
    if (stream.bad()) {
        throw CliError("session-input-unavailable", "filesystem",
                       "Cannot read session input.", 1, true, "Check the external input file.");
    }
    return text;
}

Json parse_session_json(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 32) {
            throw_usage_error("session-input-too-deep", "Session JSON nesting exceeds 32 levels.");
        }
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == Json::parse_event_t::key &&
                   !keys.back().insert(parsed.get<std::string>()).second) {
            throw_usage_error("duplicate-session-field", "Session JSON contains a duplicate field.");
        }
        return true;
    };
    try {
        return Json::parse(text, callback);
    } catch (const Json::exception&) {
        throw_usage_error("invalid-session-json", "Session input is not valid JSON.");
    }
}

void reject_session_secrets(const Json& value) {
    if (value.is_string()) {
        require_session_safe_text(value.get<std::string>());
    } else if (value.is_structured()) {
        for (auto child = value.begin(); child != value.end(); ++child) {
            if (value.is_object()) {
                require_session_safe_text(child.key());
            }
            reject_session_secrets(child.value());
        }
    }
}

void require_external_file(const fs::path& input, const fs::path& root) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(input, error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw_usage_error("invalid-session-file", "Session input must be a regular non-symlink file.");
    }
    const fs::path canonical = fs::canonical(input, error);
    if (error) {
        throw_usage_error("invalid-session-file", "Cannot canonicalize session input.");
    }
    const std::string relative = canonical.lexically_relative(root).generic_string();
    if (relative == "." || profile_relative_path_valid(relative)) {
        throw_usage_error("session-file-inside-project",
                          "Session state must be stored outside the consumer project.");
    }
    if (fs::file_size(canonical, error) > kMaxSessionInputBytes || error) {
        throw_usage_error("session-input-too-large", "Session input exceeds 256 KiB.");
    }
}

std::int64_t parse_session_time(const std::string& value) {
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < 0 || parsed > kMaxSessionTimestamp) {
        throw_usage_error("invalid-session-time", "--observed-at requires Unix seconds within years 1970-9999.");
    }
    return parsed;
}

void require_cli_identifier(const std::string& value) {
    require_session_safe_text(value);
    if (value.empty() || value.size() > 128 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x21 || c == 0x7f; })) {
        throw_usage_error("invalid-session-identity", "Session identifiers require 1-128 non-space, non-control bytes.");
    }
}

}  // namespace

SessionCommandOptions parse_session_options(const std::vector<std::string>& args, bool resume,
                                            std::string_view input_override) {
    SessionCommandOptions options;
    std::set<std::string> seen;
    const std::string input_option = input_override.empty() ? (resume ? "--checkpoint" : "--input") : std::string(input_override);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& option = args[index];
        if (option != "--trusted-producer" && !seen.insert(option).second) {
            throw_usage_error("duplicate-option", "Session options may appear only once.");
        }
        if (option == "--require-current") {
            options.require_current = true;
            continue;
        }
        const bool supported = option == input_option || option == "--task-id" ||
                               option == "--thread-id" || option == "--observed-at" ||
                               option == "--jobs" || (resume && (option == "--task" ||
                               option == "--trusted-producer"));
        if (!supported) {
            throw_usage_error("unsupported-option", "Unsupported session option.");
        }
        if (++index == args.size()) {
            throw_usage_error("missing-option-value", "Session option requires a value.");
        }
        const std::string& value = args[index];
        if (option == input_option) {
            options.stdin_input = value == "-" && input_option == "--input";
            options.input = options.stdin_input ? fs::path() : fs::absolute(value);
        } else if (option == "--task-id" || option == "--thread-id") {
            require_cli_identifier(value);
            (option == "--task-id" ? options.task_id : options.thread_id) = value;
        } else if (option == "--observed-at") {
            options.observed_at = parse_session_time(value);
            options.explicit_clock = true;
        } else if (option == "--jobs") {
            options.jobs = parse_cli_integer(value, option, 1);
            if (options.jobs > kMaxPackJobs) {
                throw_usage_error("invalid-option-value", "--jobs must not exceed 8.");
            }
        } else if (option == "--task") {
            options.current_task = session_string(Json{{"task", value}}, "task", 4096);
        } else {
            require_cli_identifier(value);
            if (!options.trusted_producers.insert(value).second || options.trusted_producers.size() > 16) {
                throw_usage_error("invalid-trusted-producer", "Use at most 16 distinct explicitly trusted producers.");
            }
        }
    }
    if (!seen.contains(input_option) || options.task_id.empty() || options.thread_id.empty()) {
        throw_usage_error("missing-option", "Session commands require input, --task-id, and --thread-id.");
    }
    validate_session_evaluation_options(options, resume);
    return options;
}

void validate_session_evaluation_options(const SessionEvaluationOptions& options, bool resume) {
    require_cli_identifier(options.task_id);
    require_cli_identifier(options.thread_id);
    if (options.jobs < 0 || options.jobs > kMaxPackJobs) {
        throw_usage_error("invalid-option-value", "Session jobs must be between 0 and 8.");
    }
    if (options.explicit_clock && (options.observed_at < 0 || options.observed_at > kMaxSessionTimestamp)) {
        throw_usage_error("invalid-session-time", "Session observation time is outside the supported range.");
    }
    if (!resume && (!options.current_task.empty() || !options.trusted_producers.empty() ||
                    !options.trusted_receipt_documents.empty())) {
        throw_usage_error("unsupported-option", "Checkpoint does not accept resume-only options.");
    }
    if (!options.current_task.empty()) {
        (void)session_string(Json{{"task", options.current_task}}, "task", 4096);
    }
    if (options.trusted_producers.size() > 16) {
        throw_usage_error("invalid-trusted-producer", "Use at most 16 distinct explicitly trusted producers.");
    }
    for (const auto& producer : options.trusted_producers) require_cli_identifier(producer);
    std::size_t trusted_bytes = 0;
    if (options.trusted_receipt_documents.size() > 32) {
        throw_usage_error("invalid-trusted-receipts", "Use at most 32 exact host-trusted receipts.");
    }
    for (const auto& receipt : options.trusted_receipt_documents) {
        if (receipt.size() > kMaxSessionInputBytes - trusted_bytes) {
            throw_usage_error("invalid-trusted-receipts", "Host receipt policy exceeds the session input bound.");
        }
        trusted_bytes += receipt.size();
    }
}

std::int64_t session_observed_at(const SessionEvaluationOptions& command) {
    if (command.explicit_clock) {
        return command.observed_at;
    }
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now < 0 || now > kMaxSessionTimestamp) {
        throw_usage_error("invalid-session-time", "System clock is outside the supported range.");
    }
    return now;
}

void require_session_safe_text(const std::string& text) {
    if (detect_secret_like_text(text, false).found) {
        throw_usage_error("secret-like-session-content",
                          "Session input contains potential credential material. Value omitted.",
                          "Remove credentials and provider payloads before creating a checkpoint.");
    }
}

RuntimeOptions canonical_session_options(const RuntimeOptions& options) {
    RuntimeOptions canonical = options;
    std::error_code error;
    canonical.root = fs::canonical(options.root, error);
    if (error || !fs::is_directory(canonical.root)) {
        throw_usage_error("invalid-session-root", "Session root must be an existing directory.");
    }
    canonical.out_root = fs::canonical(options.out_root, error);
    if (error || !fs::is_directory(canonical.out_root)) {
        throw_usage_error("invalid-session-index", "Session commands require an existing published index.");
    }
    if (canonical.generated_snapshot) {
        const auto& loaded = canonical.generated_snapshot->options();
        if (canonical.root != loaded.root || canonical.out_root != loaded.out_root ||
            canonical.profile.id != loaded.profile.id ||
            profile_config_hash(canonical.profile) != profile_config_hash(loaded.profile)) {
            throw_usage_error("session-snapshot-binding-mismatch",
                              "Loaded snapshot does not match the requested session scope.");
        }
        // The immutable owner, not a mutable copy of its Profile, defines the route.
        canonical = loaded;
        canonical.generated_snapshot = options.generated_snapshot;
    }
    return canonical;
}

std::unique_ptr<RetrievalSession> open_session_evaluation(const RuntimeOptions& input) {
    const RuntimeOptions options = canonical_session_options(input);
    if (options.force_source_scan) {
        throw_usage_error("unsupported-option", "Session evaluation requires a published snapshot.");
    }
    if (options.generated_snapshot) {
        return std::make_unique<RetrievalSession>(options.generated_snapshot, RetrievalSessionOpenMode::verified_snapshot);
    }
    return std::make_unique<RetrievalSession>(options, RetrievalSessionOpenMode::verified_snapshot);
}

Json parse_session_document(std::string_view text) {
    if (text.size() > kMaxSessionInputBytes) {
        throw_usage_error("session-input-too-large", "Session input exceeds 256 KiB.");
    }
    Json document = parse_session_json(text);
    reject_session_secrets(document);
    return document;
}

Json read_session_input(const SessionCommandOptions& command, const fs::path& root) {
    std::string text;
    if (command.stdin_input) {
        text = read_bounded_session(std::cin);
    } else {
        require_external_file(command.input, root);
        const FileMetadata before = file_metadata(command.input);
        std::ifstream stream(command.input, std::ios::binary);
        if (!stream) {
            throw_usage_error("invalid-session-file", "Cannot open session input.");
        }
        text = read_bounded_session(stream);
        require_external_file(command.input, root);
        if (file_metadata(command.input) != before) {
            throw_file_changed_during_verification(command.input);
        }
    }
    return parse_session_document(text);
}

void require_session_output_bounds(const Json& output) {
    if (output.dump().size() > kMaxSessionOutputBytes) {
        throw CliError("session-output-too-large", "resource", "Session output exceeds 512 KiB.",
                       1, false, "Reduce task requirements and checkpoint state.");
    }
}

void emit_session_document(const Json& output) {
    require_session_output_bounds(output);
    std::cout << output.dump() << '\n';
}

}  // namespace shin::context
