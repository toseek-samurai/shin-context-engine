#pragma once

#include "agent_context_pack.h"
#include "worktree_state.h"

namespace shin::context {

inline constexpr std::size_t kMaxSessionInputBytes = 256U * 1024U;
inline constexpr std::size_t kMaxSessionOutputBytes = 512U * 1024U;
inline constexpr std::size_t kMaxSessionTrackedPaths = 256;
inline constexpr std::int64_t kMaxSessionTimestamp = 253402300799LL;
inline constexpr std::int64_t kMaxSessionTtl = 604800;

struct SessionEvaluationOptions {
    std::string task_id;
    std::string thread_id;
    std::string current_task;
    std::int64_t observed_at = 0;
    bool explicit_clock = false;
    bool require_current = false;
    int jobs = 0;
    std::set<std::string> trusted_producers;
    // Native host policy only; never populated from a checkpoint or CLI input.
    std::set<std::string> trusted_receipt_documents;
};

struct SessionCommandOptions : SessionEvaluationOptions {
    fs::path input;
    bool stdin_input = false;
};

struct SessionDraft {
    TaskRequest request;
    Json state;
    Json receipts;
    Json vcs;
    std::int64_t ttl_seconds = 86400;
};

SessionCommandOptions parse_session_options(const std::vector<std::string>& args, bool resume,
                                            std::string_view input_override = {});
void validate_session_evaluation_options(const SessionEvaluationOptions& options, bool resume);
std::int64_t session_observed_at(const SessionEvaluationOptions& command);
void require_session_safe_text(const std::string& text);
RuntimeOptions canonical_session_options(const RuntimeOptions& options);
Json read_session_input(const SessionCommandOptions& command, const fs::path& root);
Json parse_session_document(std::string_view text);
std::unique_ptr<RetrievalSession> open_session_evaluation(const RuntimeOptions& options);
Json evaluate_checkpoint(const RuntimeOptions& options, const SessionEvaluationOptions& command,
                         std::string_view input);
Json evaluate_resume(const RuntimeOptions& options, const SessionEvaluationOptions& command,
                     std::string_view input);
void require_session_output_bounds(const Json& output);
void require_session_fields(const Json& object, const std::set<std::string>& allowed);
std::string session_string(const Json& object, const std::string& field, std::size_t maximum);
std::int64_t session_integer(const Json& object, const std::string& field,
                             std::int64_t minimum, std::int64_t maximum);
void require_session_digest(const std::string& value);
Json parse_session_binding(const Json& value, bool receipt);
SessionDraft parse_session_draft(const Json& input, const Profile& profile);
SessionDraft validate_session_checkpoint(const Json& input, const Profile& profile);
Json session_binding(const RuntimeOptions& options, const SessionEvaluationOptions& command);
Json session_source_state(const RuntimeOptions& options, const WorktreeInventory& inventory,
                          const std::string& path);
Json session_tracked_sources(const RuntimeOptions& options, const WorktreeInventory& inventory,
                             const Json& state, const AgentContextPack* pack);
std::string session_age(std::int64_t observed_at, std::int64_t ttl, std::int64_t now);
Json session_policy();
void emit_session_document(const Json& output);

}  // namespace shin::context
