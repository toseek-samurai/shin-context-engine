#pragma once
#include "shin/observer/local_project.h"
#include <string_view>

namespace shin::observer {
inline std::string_view mcp_session_guidance() {
    return " Optional handoff: use shin_checkpoint only when useful and permitted by the host. "
        "Keep the exact project, real session_id, stable task_key, objective and returned revision in host handoff state; "
        "never persist chat identity or notes in shared project instructions or MCP configuration. "
        "After reconnect resolve the saved exact root through the current catalog when advertised; do not select by name alone. "
        "After context loss, use shin_resume only when advertised and that binding is known; otherwise ask one useful current-code query. "
        "An ok assessment is not clearance to continue: ready_without_review requires continuation_ready and no review_required. "
        "Reconcile review flags and current evidence; never change identities to bypass a missing or mismatched note. "
        "Replacing notes requires the exact expected_revision after review. No per-turn checkpoint ritual, automatic compaction hook or call quota. "
        "Host history model labels never identify the model of an individual MCP call.";
}
inline bool session_tool(std::string_view method) { return method == "checkpoint" || method == "resume"; }
inline void mcp_session_schema(std::string_view method, Json& properties, Json& required) {
    properties["task_key"] = {{"type","string"},{"minLength",1},{"maxLength",128},
        {"description","Caller-chosen stable task key within this real chat. Keep it across context recovery; use a new key for a different task. Optional for ordinary reads."}};
    properties["intent"] = {{"type","string"},{"enum",Json::array({"explore","context-recovery","follow-up"})},
        {"description","Optional caller-reported purpose. Context recovery does not assert a host compaction event."}};
    if (!session_tool(method)) return;
    properties.erase("intent");
    required.push_back("session_id"); required.push_back("task_key"); required.push_back("task");
    properties["task"] = {{"type","string"},{"minLength",1},{"maxLength",1024},
        {"description","Current objective in your own words. Resume compares it with the saved objective; it never silently changes tasks."}};
    if (method == "resume") return;
    properties["progress"] = {{"type","string"},{"minLength",1},{"maxLength",1024}};
    properties["ttl_seconds"] = {{"type","integer"},{"minimum",1},{"maximum",604800},{"default",86400}};
    properties["expected_revision"] = {{"type","string"},{"pattern","^[0-9a-f]{64}$"},
        {"description","Exact revision returned by resume/checkpoint when replacing existing notes. Omit only for the first save."}};
    for (const auto* key : {"queries","paths","next_steps"})
        properties[key] = {{"type","array"},{"maxItems",std::string_view(key) == "queries" ? 4 : 8},
            {"items",{{"type","string"},{"minLength",1},{"maxLength",256}}}};
    properties["queries"]["minItems"] = 1;
    required.push_back("progress"); required.push_back("queries");
}
inline Json mcp_session_draft(const Json& args, const std::string& task, const std::string& progress) {
    const auto strings = [&](const char* key, std::size_t maximum, bool required = false) {
        auto list = args.value(key,Json::array());
        if (!list.is_array() || list.size() > maximum || (required && list.empty()))
            throw Error("invalid-arguments","Provide a bounded list of compact session items.");
        for (const auto& value : list) if (!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > 256)
            throw Error("invalid-arguments","Session list items must be nonempty strings within 256 bytes.");
        return list;
    };
    Json requirements = Json::array(), actions = Json::array();
    for (const auto& query : strings("queries",4,true)) requirements.push_back({{"id","context-"+std::to_string(requirements.size()+1)},{"query",query}});
    for (const auto& step : strings("next_steps",8)) actions.push_back({{"id","step-"+std::to_string(actions.size()+1)},{"text",step}});
    const auto ttl = args.value("ttl_seconds",Json(86400));
    if (!ttl.is_number_integer() || ttl.get<std::int64_t>() < 1 || ttl.get<std::int64_t>() > 604800)
        throw Error("invalid-arguments","Session TTL must be between one second and seven days.");
    Json draft{{"schema_version","shin.session-draft.v1"},
        {"task_request",{{"schema_version","shin.task-request.v1"},{"request_id","session-context"},
            {"task",{{"objective",task},{"must",requirements}}},
            {"budget",{{"max_evidence_bytes",4096},{"max_records",4},{"per_query_records",2},{"per_document_records",1}}}}},
        {"state",{{"current_slice",progress},{"assertions",Json::array()},{"changed_paths",strings("paths",8)},
            {"identifiers",Json::array()},{"next_actions",actions}}},{"receipts",Json::array()},{"vcs",nullptr},{"ttl_seconds",ttl}};
    if (draft.dump().size() > 4096) throw Error("session-input-too-large","Keep compact task notes and locators within 4 KiB; omit transcripts and raw tool output.");
    return draft;
}
}
