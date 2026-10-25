#include "local_sessions.h"
#include "context_internal.h"
#include "session_contract.h"
#include "shin/observer/local_rpc.h"
#include <set>

namespace shin::observer {
namespace core = shin::context;
void local_session_actions(Json& assessment) {
    const auto actions = assessment.find("next_actions");
    if (actions == assessment.end() || !actions->is_array()) return;
    for (auto& action : *actions) {
        if (!action.is_object()) continue;
        const auto command = action.value("next_command",Json());
        if (action.value("code",Json()) == "rebuild-index" ||
            (command.is_object() && command.value("index_mutation",Json()) == true))
            action = {{"code","wait-for-background-qualification"},{"source","shin-local-policy"},{"automatic_execution",false},
                {"text","The local service owns index publication. Preserve its binding and let normal background qualification finish. Continue direct authorized source work; use one useful current-code query when ready. Do not rebuild the managed index or change grants."}};
    }
}
namespace {
bool opaque(const std::string& value) { return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos; }
void require(bool condition, const char* code, const char* message) { if (!condition) throw Error(code,message); }
}
Json local_session_payload(const LocalProjectRequest& request) {
    Json value{{"task_id",request.task_id},{"task",request.session_task}};
    if (request.command == "checkpoint") value["draft"] = request.session_draft;
    if (!request.expected_revision.empty()) value["expected_revision"] = request.expected_revision;
    return value;
}
void local_session_fields(const Json& payload, LocalProjectRequest& request) {
    request.session_draft = nullptr; request.session_task.clear(); request.expected_revision.clear(); request.task_id.clear();
    request.session_id = payload.value("session_id",std::string());
    if (request.command != "checkpoint" && request.command != "resume") return;
    require(opaque(request.session_id) && payload.contains("session") && payload.at("session").is_object(),
        "invalid-session-identity","Session recovery requires an actual caller session and task binding.");
    const auto& value = payload.at("session");
    for (const auto& [key,entry] : value.items()) {
        (void)entry;
        require(key == "task_id" || key == "task" || (request.command == "checkpoint" && (key == "draft" || key == "expected_revision")),
            "invalid-arguments","Unexpected session argument.");
    }
    request.task_id = local_rpc::string_field(value,"task_id",64);
    request.session_task = local_rpc::string_field(value,"task",1024);
    require(opaque(request.task_id) && !request.session_task.empty(),"invalid-session-identity","A bounded task binding is required.");
    if (request.command == "checkpoint") {
        require(value.contains("draft") && value.at("draft").is_object() && value.at("draft").dump().size() <= 4096,
            "session-input-too-large","Keep the compact task draft within 4 KiB; omit transcripts and raw tool output.");
        request.session_draft = value.at("draft");
        if (value.contains("expected_revision")) {
            request.expected_revision = local_rpc::string_field(value,"expected_revision",64);
            require(opaque(request.expected_revision),"invalid-arguments","Use the exact revision returned by resume/checkpoint.");
        }
    }
}
Json local_session_operation(const core::RuntimeOptions& options, const fs::path& workspace,
    const LocalProjectRequest& request, const std::function<void()>& guard) {
    const auto root_identity = directory_identity(options.root);
    const auto workspace_identity = directory_identity(workspace,true);
    const Json binding{{"root_identity",root_identity},{"workspace_identity",workspace_identity},
        {"profile",options.profile.id},{"session_id",request.session_id},{"task_id",request.task_id}};
    require(opaque(request.session_id) && opaque(request.task_id),"invalid-session-identity","Session and task identities are required.");
    const auto parent = request.home / "sessions";
    const auto slot = parent / digest(binding.dump());
    core::SessionEvaluationOptions command;
    command.task_id = request.task_id; command.thread_id = request.session_id;
    if (request.command == "resume") command.current_task = request.session_task;
    command.jobs = 1;
    guard();
    if (request.command == "resume") {
        require(fs::exists(fs::symlink_status(slot)),"session-checkpoint-not-found","No checkpoint is saved for this project, session and task. Query current code normally.");
        StateStore saved(slot,false);
        const auto envelope = saved.load();
        if (!envelope) throw Error("session-state-invalid","Saved session state is absent; existing data is preserved.");
        const auto& document = *envelope;
        require(document.value("schema_version",Json()) == "shin.local-session-state.v1" && document.value("binding",Json()) == binding &&
            document.contains("checkpoint") && document.value("revision",Json()) == digest(document.at("checkpoint").dump()),
            "session-state-invalid","Saved session state could not be validated; existing data is preserved.");
        auto session = core::evaluate_resume(options,command,document.at("checkpoint").dump());
        local_session_actions(session);
        guard();
        require(session.dump().size() <= 48*1024,"session-response-too-large","Session assessment exceeds its response bound; the checkpoint is preserved.");
        return {{"ok",session.value("ok",false)},{"session",session},{"revision",document.at("revision")},
            {"ready_without_review",session.value("continuation_ready",false) && !session.value("review_required",true)},
            {"snapshot_id",session.value("snapshot_id",std::string())},{"session_state_authority","caller-asserted"}};
    }
    const auto checkpoint = core::evaluate_checkpoint(options,command,request.session_draft.dump());
    require(checkpoint.dump().size() <= 16*1024,"session-checkpoint-too-large","Keep fewer paths and shorter notes; the previous checkpoint is preserved.");
    guard();
    require(directory_identity(options.root) == root_identity && directory_identity(workspace,true) == workspace_identity,
        "binding-identity-changed","Project identity changed during checkpoint assessment.");
    ensure_private_directory(parent);
    StateStore allocation(parent,true);
    if (!fs::exists(fs::symlink_status(slot))) {
        require(request.expected_revision.empty(),"session-checkpoint-conflict","No checkpoint exists at the supplied revision; resume before retrying.");
        std::size_t slots = 0;
        for (const auto& entry : fs::directory_iterator(parent)) {
            if (entry.path().filename() == "observer.lock") continue;
            require(opaque(entry.path().filename().string()) && ++slots <= 1024,"session-store-unavailable","Session storage requires operator review; project reads remain available.");
            (void)directory_identity(entry.path(),true);
        }
        require(slots < 1024,"session-store-full","Session storage reached its bounded capacity; existing checkpoints and project reads are preserved.");
        ensure_private_directory(slot);
    }
    StateStore saved(slot,true);
    saved.recover_pending();
    const auto previous = saved.load();
    require(!previous || (previous->value("schema_version",Json()) == "shin.local-session-state.v1" && previous->value("binding",Json()) == binding &&
        previous->contains("checkpoint") && previous->value("revision",Json()) == digest(previous->at("checkpoint").dump())),
        "session-state-invalid","The previous checkpoint binding is invalid; it is preserved.");
    require(previous ? previous->value("revision",Json()) == request.expected_revision : request.expected_revision.empty(),
        "session-checkpoint-conflict","Resume this task to obtain its current revision before replacing the checkpoint.");
    const auto revision = digest(checkpoint.dump());
    guard();
    saved.save({{"schema_version","shin.local-session-state.v1"},{"binding",binding},{"checkpoint",checkpoint},{"revision",revision}});
    return {{"ok",true},{"revision",revision},{"checkpoint_id",checkpoint.at("checkpoint_id")},
        {"snapshot_id",checkpoint.at("baseline").at("snapshot_id")},{"baseline",checkpoint.at("baseline")},
        {"session_state_authority","caller-asserted"},{"saved",true}};
}
}
