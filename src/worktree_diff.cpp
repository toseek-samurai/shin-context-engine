#include "context_internal.h"
#include "snapshot_verification.h"
#include "worktree_state.h"

namespace shin::context {

int run_worktree_diff(const RuntimeOptions& options, const std::vector<std::string>& args) {
    int limit = 100;
    bool strict = false;
    std::set<std::string> seen;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (!seen.insert(args[index]).second) {
            throw_usage_error("duplicate-option", "worktree-diff options may appear only once.");
        }
        if (args[index] == "--strict") {
            strict = true;
        } else if (args[index] == "--limit") {
            if (++index == args.size()) {
                throw_usage_error("missing-option-value", "--limit requires a value.");
            }
            limit = parse_cli_integer(args[index], "--limit", 1);
            if (limit > 1000) {
                throw_usage_error("invalid-option-value", "worktree-diff --limit must not exceed 1000.");
            }
        } else {
            throw_usage_error("unsupported-option", "worktree-diff only accepts --limit and --strict.");
        }
    }
    const VerifiedSnapshot snapshot = load_verified_snapshot(options.out_root);
    if (snapshot.profile != options.profile.id) {
        throw CliError("worktree-diff-profile-mismatch", "configuration",
                       "Generated snapshot profile does not match the active profile.", 1, false,
                       "Select the profile that owns the published index or rebuild it.");
    }
    const WorktreeInventory inventory = observe_worktree(options);
    Json output = compare_worktree(options, snapshot.manifest, inventory, limit);
    const bool current = output.at("current").get<bool>();
    output.update(make_result_document("shin.worktree-diff.v1", "worktree-diff", current));
    output["strict"] = strict;
    output["snapshot"] = verified_snapshot_description(snapshot);
    emit_json_document(std::cout, output);
    return strict && !current ? 1 : 0;
}

}  // namespace shin::context
