#include "shin/observer/local_mcp_scope.h"
#include "shin/observer/validation_diagnostic.h"
#include <algorithm>
#include <set>

namespace shin::observer {
namespace {
Json safe_report_fields() {
    return Json::array({"error code", "bounded diagnostic IDs and reasons", "tool and version",
        "timestamp", "opaque project/request/snapshot IDs when available"});
}
Json with_diagnostic(Json result, const Json& input) {
    const auto diagnostic = bounded_failure_diagnostic(input);
    if (!diagnostic.is_null()) {
        result["diagnostic"] = diagnostic;
        const auto reason = validation_failure_reason(diagnostic);
        if (!reason.empty()) result["failure_reason"] = reason;
    }
    return result;
}

Json route_review() {
    return {{"kind", "operator-review"}, {"responsible_party", "project-owner"},
        {"approval_required", true}, {"next_action", "review-project-changes"},
        {"same_project_required", true}, {"fresh_snapshot_required", true},
        {"message", "Project profile, marker or route configuration differs from its approved revision. "
            "This is not a transient refresh: repeated queries or reconnecting MCP do not approve changes. "
            "Continue useful work with direct sources and report this blocker. For a profile change, "
            "review it in Shin Monitor (View changes), or use the read-only shin-ce profile-review with "
            "this project's verified root and configured home. If the review is unavailable, report that "
            "limitation for operator review; do not guess an approval command. An owner must accept the "
            "exact reviewed change; an agent may do that only with explicit authorization covering it. "
            "Editing a profile is not approval. After acceptance, wait for background qualification, then "
            "make a fresh shin_query for the same project and use its new snapshot. Approval alone does "
            "not establish readiness. Do not re-enroll, release ownership, weaken checks or rebuild the "
            "managed index manually."}};
}
}

Json local_mcp_recovery(const std::string& code, const Json& diagnostic, std::string_view method) {
    if (code == "session-checkpoint-conflict")
        return {{"kind","request-correction"},{"responsible_party","requester"},
            {"next_action","resume-same-project-session-task"},{"retry_same_request",false},
            {"message","Resume the same project, real chat and task key to review its saved revision. Replace notes only with that exact expected_revision. Existing notes and normal context reads are preserved."}};
    if (code == "session-checkpoint-not-found")
        return {{"kind","request-correction"},{"responsible_party","requester"},
            {"next_action","query-current-code-without-checkpoint"},{"retry_same_request",false},
            {"message","No checkpoint is available under this binding. Use a useful current-code query; do not substitute another session or task identity. Checkpoints are optional."}};
    if (code.starts_with("session-state-") || code.starts_with("session-store-") || code == "session-response-too-large" || code == "session-checkpoint-too-large")
        return {{"kind","operator-review"},{"responsible_party","operator"},
            {"next_action","preserve-session-notes-and-query-current-code"},{"retry_same_request",false},
            {"message","This optional task-note operation could not be confirmed. Preserve existing notes and report the bounded error code. Continue normal context queries; do not delete saved state or repair project indexes."}};
    if (code == "route-revision-conflict") return with_diagnostic(route_review(), diagnostic);
    if (code == "binding-identity-changed" || code == "mcp-project-changed" ||
        code == "project-detached" || code == "binding-changed" || code == "source-binding-changed" ||
        code == "refresh-authority-changed" || code == "refresh-grant-required" ||
        code == "refresh-grant-incomplete" || code == "service-project-denied" || code == "project-unavailable") {
        return with_diagnostic({{"kind", "operator-review"}, {"responsible_party", "operator"},
            {"next_action", "review-project-binding-and-lifecycle"},
            {"message", "The project binding, lifecycle or authorized availability cannot be used by this connection. "
                "Ask its owner/operator to review the project state and existing setup; reconnect only when "
                "the reviewed lifecycle or identity change requires a new connection. "
                "Do not re-enroll or change route authority from this response."}}, diagnostic);
    }
    if (code == "service-version-mismatch")
        return {{"kind", "environment-action"}, {"responsible_party", "operator"},
            {"next_action", "reconnect-client-with-current-kit"},
            {"message", "The MCP client and service belong to different kit versions. After a kit update, reconnect "
                "the MCP integration or restart its client application to load the installed adapter. Verify that "
                "both components use the current installation. A current running service can keep serving other "
                "clients. Preserve project bindings and data; do not re-enroll projects or launch another service."}};
    if (code == "service-unavailable" || code == "service-disconnected" || code == "service-executable-changed")
        return {{"kind", "environment-action"}, {"responsible_party", "operator"},
            {"next_action", "review-service-and-matched-kit"},
            {"message", "Ask the operator to check that the configured local service is available and its "
                "components belong to the same installed kit. This alone does not prove damaged project data "
                "or an engine defect. Continue authorized source work. Do not launch a second service, re-enroll "
                "projects or change client settings to bypass the refusal. If the matched service still fails, "
                "report the code to Shin maintainers."}};
    if (code == "refresh-engine-changed")
        return {{"kind", "operator-review"}, {"responsible_party", "operator"},
            {"approval_required", true}, {"next_action", "review-kit-and-index-migration"},
            {"message", "The engine or index binding needs explicit migration review. Ask the operator to "
                "review the matched kit and applicable migration while preserving project data and history. "
                "Do not run init, re-enroll, rebuild a managed index or migrate it from this response alone."}};
    if (code == "service-request-cancelled")
        return {{"kind", "cancelled"}, {"responsible_party", "requester"},
            {"next_action", "continue-current-task"},
            {"message", "The request was cancelled. No repair is indicated; make another request only if it "
                "is still useful for the current task."}};
    if (code == "refresh-source-budget" || code == "refresh-output-budget")
        return {{"kind", "operator-review"}, {"responsible_party", "project-owner"},
            {"next_action", "review-project-coverage-and-budget"}, {"approval_required", true},
            {"message", "The project exceeds its configured refresh bounds. Ask its owner to review intended "
                "coverage and resource requirements. Do not raise limits, delete project files or narrow the "
                "profile automatically. Continue authorized source work while the policy is reviewed."}};
    if (code == "invalid-arguments")
        return {{"kind", "request-correction"}, {"responsible_party", "requester"},
            {"next_action", "correct-request-arguments"},
            {"message", "Correct the tool arguments using the documented bounds, then retry the corrected request."}};
    if (code == "mcp-snapshot-scope-mismatch" || code == "source-snapshot-mismatch" || code == "source-read-snapshot-mismatch" || code == "mcp-snapshot-invalid")
        return {{"kind", "request-correction"}, {"responsible_party", "requester"},
            {"next_action", "query-same-project-for-fresh-snapshot"}, {"same_project_required", true},
            {"fresh_snapshot_required", true},
            {"message", "The snapshot cannot be used for this project and current request context. Make a fresh query "
                "for the same project and use only the returned snapshot for follow-up reads."}};
    if (code == "source-read-document-only")
        return {{"kind", "request-correction"}, {"responsible_party", "requester"},
            {"next_action", "read-authorized-document-directly"}, {"retry_same_request", false},
            {"message", "This published path is document evidence, not a SOURCE file. Use the returned document excerpts or read the file with your ordinary local reader within the task's existing access. Do not retry shin_read for this document, reindex or change the profile."}};
    if (code == "source-read-unknown-file")
        return {{"kind", "request-correction"}, {"responsible_party", "requester"},
            {"next_action", "check-source-locators-or-read-authorized-file-directly"},
            {"file_existence_assessed", false}, {"retry_same_request", false},
            {"message", "The path has no scanned source binding in this snapshot; this does not prove it is missing on disk. Check the project-relative path and SOURCE locators from the query. For a known authorized file outside source coverage, use your ordinary local reader. Do not re-enroll, reindex or widen the profile to read it."}};
    if (code == "mcp-project-denied")
        return {{"kind", "request-correction"}, {"responsible_party", "requester"},
            {"next_action", "discover-allowed-project-catalog"},
            {"message", "This selector is not allowed on the current MCP connection. Discover the allowed "
                "project catalog and use a returned selector; do not infer grants or substitute a project ID."}};
    if (code == "storage-full" || code == "storage-read-only" || code == "access-denied" || code == "source-io-error" || code == "resource-exhausted")
        return with_diagnostic({{"kind", "environment-action"}, {"responsible_party", "operator"},
            {"next_action", "review-storage-or-access-condition"},
            {"message", "The local environment reported a source I/O, storage, access or resource condition. Ask the operator to "
                "review the reported condition before continuing. Do not delete project files or change permissions automatically."}}, diagnostic);
    if (code == "refresh-validation-failed" || code == "project-check-failed") {
        Json result{{"kind", "project-check"}, {"responsible_party", "project-owner"},
            {"next_action", "inspect-authorized-source-evidence"},
            {"message", "Project validation did not qualify the current candidate. Inspect authorized source "
                "and documentation evidence together with the bounded diagnostic; a reported missing-path "
                "reason alone does not establish that a file was deleted. Repair an actual source or "
                "documentation issue only within this task's authority. Intentional policy or profile changes "
                "need owner review. If the evidence exists but was missed or remains unclear, report it to Shin "
                "maintainers. Do not relax profiles or repair indexes."}};
        return with_diagnostic(std::move(result), diagnostic);
    }
    if (code == "service-busy")
        return with_diagnostic({{"kind","transient"},{"responsible_party","requester"},
            {"retry_after_ms",method == "status" ? 0 : 1000},{"max_attempts_per_checkpoint",1},
            {"next_tool","shin_query"},{"same_project_required",true},{"fresh_snapshot_required",true},
            {"next_action",method == "status" ? "query-once-with-bounded-readiness-wait" : "continue-work-then-query-once-at-checkpoint"},
            {"message","Read admission is occupied or its bounded waiting capacity is full; this does not establish a broken project or failed validation. A useful query can wait briefly behind current work. Continue authorized direct-source work; if context is useful after the current calls finish, make one query for the same project and use its new snapshot. Do not poll status, restart a healthy service or change grants to bypass capacity."}},diagnostic);
    if (code == "project-updating" && method == "status")
        return with_diagnostic({{"kind", "transient"}, {"responsible_party", "requester"},
            {"retry_after_ms", 0}, {"max_attempts_per_checkpoint", 1}, {"next_tool", "shin_query"},
            {"same_project_required", true}, {"fresh_snapshot_required", true},
            {"next_action", "query-once-with-bounded-readiness-wait"},
            {"message", "This status probe did not wait for background reader preparation. If context is useful now, "
                "make one shin_query for the same project; it waits automatically up to 20 seconds for readiness. Use its "
                "fresh snapshot for follow-up reads. If that query is also refused, continue useful work with "
                "direct sources and follow its recovery guidance. Do not poll status or retry in a loop."}}, diagnostic);
    if (code == "project-updating" || code == "refresh-timeout" ||
        code == "refresh-suspended" ||
        code == "file-changed-during-verification" || code == "query-source-changed" ||
        code == "source-read-hash-mismatch" || code == "refresh-superseded" ||
        code == "query-snapshot-changed")
        return with_diagnostic({{"kind", "transient"}, {"responsible_party", "requester"},
            {"retry_after_ms", 5000},
            {"max_attempts_per_checkpoint", 1}, {"next_tool", "shin_query"},
            {"same_project_required", true}, {"fresh_snapshot_required", true},
            {"next_action", "continue-work-then-query-once-at-checkpoint"},
            {"message", "Shin reported a transient update or source race. Continue useful work with direct "
                "sources. If context is still useful at a later task checkpoint, after edits settle, make one "
                "fresh query for the same project after the suggested delay and use only its new snapshot for "
                "follow-up reads. Do not poll or retry in a loop. If it fails again, report the repeated code "
                "to Shin maintainers at the next useful checkpoint."}}, diagnostic);

    return with_diagnostic({{"kind", "maintainer-diagnosis"}, {"responsible_party", "shin-maintainers"},
        {"cause_status", "unconfirmed"}, {"next_action", "continue-authorized-source-work-and-report"},
        {"help_tool", "shin_help"}, {"safe_report_fields", safe_report_fields()},
        {"report_privacy", "Review before sharing; omit credentials, source bodies and absolute paths. Nothing is uploaded automatically."},
        {"message", "The reason is not enough to blame a source or assert an engine defect. Continue direct "
            "authorized source work and report the code with bounded diagnostic evidence. Do not retry on a "
            "timer, repair indexes or change profiles."}}, diagnostic);
}

Json local_mcp_help(const std::string& code) {
    if (!code.empty() && validation_identifier(code).empty())
        throw Error("invalid-arguments", "Help code must be a bounded identifier.");
    Json result{{"ok", true}, {"schema_version", "shin.mcp-help.v1"},
        {"scope", "static-guidance"}, {"readiness_assessed", false},
        {"message", "Shin MCP is read-only context access. Use a returned project selector when available and a follow-up "
            "snapshot. Correct request mistakes; review binding, lifecycle or route-authority changes with "
            "the owner; ask an operator to address environment conditions; investigate project-check evidence "
            "within task authority; and report unclear or unclassified failures to Shin maintainers. Guidance "
            "does not repair state, grant access or approve changes."},
        {"categories", Json::array({"request-correction", "operator-review", "environment-action",
            "project-check", "transient", "cancelled", "maintainer-diagnosis"})},
        {"safe_report_fields", safe_report_fields()},
        {"exclude_from_report", Json::array({"credentials", "source bodies", "absolute paths"})},
        {"automatic_upload", false}};
    if (!code.empty()) result["recovery"] = local_mcp_recovery(code);
    return result;
}

LocalMcpScope::LocalMcpScope(const fs::path& home, const std::vector<fs::path>& roots)
    : home_(home), home_identity_(directory_identity(home, true)) {
    if (roots.empty() || roots.size() > kLocalServiceProjectLimit)
        throw Error("invalid-arguments", "Allow between one and sixteen explicit project roots.");
    std::set<fs::path> unique;
    for (const auto& root : roots) {
        if (root.empty()) throw Error("invalid-arguments", "Project root must not be empty.");
        Entry value;
        value.request.root = fs::absolute(root).lexically_normal();
        value.request.home = home_;
        try {
            value.request.root = fs::canonical(root);
            value.root_identity = directory_identity(value.request.root);
            const auto workspace = home_ / "projects" / digest(value.request.root.string());
            value.workspace_identity = directory_identity(workspace, true);
            value.lifecycle = local_project_lifecycle(workspace);
        } catch (const Error& error) { value.startup_error = error.code; }
        catch (const std::exception&) { value.startup_error = "project-unavailable"; }
        if (!unique.insert(value.request.root).second)
            throw Error("invalid-arguments", "Project roots must be distinct after canonicalization.");
        // Length-framed canonical JSON avoids delimiter/path ambiguity. This is
        // an opaque selector, not a credential or inferred Server project ID.
        value.handle = "p" + digest(Json::array({home_identity_, value.request.root.string(),
            value.root_identity, value.workspace_identity, value.lifecycle.revision, value.startup_error}).dump());
        entries_.push_back(std::move(value));
    }
}

const LocalMcpScope::Entry& LocalMcpScope::entry(const std::string& project) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& value) { return value.handle == project; });
    if (found == entries_.end()) throw Error("mcp-project-denied", "Project is outside this MCP connection's allowed set.");
    return *found;
}

void LocalMcpScope::verify(const Entry& value) const {
    if (!value.startup_error.empty())
        throw Error(value.startup_error, "Configured project was unavailable at startup; reconnect after setup.");
    const auto workspace = home_ / "projects" / digest(value.request.root.string());
    if (directory_identity(home_, true) != home_identity_ ||
        directory_identity(value.request.root) != value.root_identity ||
        directory_identity(workspace, true) != value.workspace_identity)
        throw Error("binding-identity-changed", "Project identity changed; reconnect after review.");
    if (require_local_project_active(workspace) != value.lifecycle)
        throw Error("mcp-project-changed", "Project lifecycle changed; reconnect to obtain a new selector.");
}

void LocalMcpScope::verify(const std::string& project) const { verify(entry(project)); }
LocalProjectRequest LocalMcpScope::select(const std::string& project) const {
    const auto& value = entry(project);
    verify(value);
    return value.request;
}

Json LocalMcpScope::catalog() const {
    Json projects = Json::array();
    for (const auto& value : entries_) {
        Json item{{"project", value.handle}, {"name", value.request.root.filename().string()},
                  {"root", value.request.root.string()}, {"binding_active", false}};
        try { verify(value); item["binding_active"] = true; }
        catch (const Error& error) { item["error_code"] = error.code; }
        catch (const std::exception&) { item["error_code"] = "project-unavailable"; }
        projects.push_back(std::move(item));
    }
    Json result{{"ok", true}, {"schema_version", "shin.mcp-projects.v1"}, {"projects", projects},
                {"readiness_assessed", false}, {"scope", "explicit-startup-roots"}};
    if (result.dump().size() > 32768)
        throw Error("mcp-catalog-too-large", "Allowed project metadata exceeds the response bound.");
    return result;
}

std::string LocalMcpScope::snapshot(const std::string& project, const std::string& native) const {
    (void)entry(project);
    if (native.empty() || native.size() > 32 || native.find('/') != std::string::npos)
        throw Error("mcp-snapshot-invalid", "Native snapshot identity is invalid.");
    return project + "/" + native;
}
std::string LocalMcpScope::native_snapshot(const std::string& project, const std::string& scoped) const {
    (void)entry(project);
    const auto prefix = project + "/";
    if (!scoped.starts_with(prefix))
        throw Error("mcp-snapshot-scope-mismatch", "Use this project's returned mcp_snapshot for follow-up reads.");
    const auto native = scoped.substr(prefix.size());
    (void)snapshot(project, native); // Validate the bounded native component.
    return native;
}
} // namespace shin::observer
