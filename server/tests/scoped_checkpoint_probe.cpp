#include "shin/server/continuity_service.h"
#include <chrono>
#include <array>
#include <future>
#include <iostream>
#include <thread>

namespace {
using namespace shin::server;
unsigned checks = 0;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class F> void denied(F operation, int status, const char* code) {
    try { operation(); }
    catch (const ServiceError& error) { check(error.status == status && error.code == code, "unexpected checkpoint denial"); return; }
    throw std::runtime_error("checkpoint unexpectedly admitted");
}
CheckpointMutation mutation(const std::string& marker, const std::string& slot, std::uint64_t revision = 0) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto hash = digest_hex(sha256(marker + ":" + slot + ":" + std::to_string(revision)));
    return {"task-" + marker, "thread-" + marker, hash.substr(0, 32), hash, revision, now + 120};
}
Json draft(const std::string& marker) {
    auto value = Json::parse(R"({"schema_version":"shin.session-draft.v1",
      "task_request":{"schema_version":"shin.task-request.v1","request_id":"scoped-checkpoint",
        "task":{"objective":"Preserve route policy","must":[{"id":"policy","query":"project identity profile fingerprint"}]},
        "budget":{"max_evidence_bytes":8192,"max_records":6,"per_query_records":6,"per_document_records":2}},
      "state":{"current_slice":"","assertions":[],"changed_paths":[],"identifiers":[],"next_actions":[]},
      "receipts":[],"vcs":null,"ttl_seconds":3600})");
    value["state"]["current_slice"] = marker;
    return value;
}
Json write_request(const std::string& marker, const std::string& slot, const Json& selector, std::uint64_t revision = 0) {
    const auto request = mutation(marker, slot, revision);
    return {{"schema_version", "shin.task-checkpoint-write.v1"}, {"selector", selector},
        {"task_id", request.task_id}, {"thread_id", request.thread_id}, {"request_id", request.request_id},
        {"request_deadline", request.request_deadline}, {"expected_revision", revision},
        {"require_current", true}, {"draft", draft(marker)}};
}
void run(const std::filesystem::path& path) {
    const auto config = load_config(path);
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "dedicated runtime stand required");
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    check(admin.admin, "maintenance identity missing");
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}};
    Store store(config.database);
    RouteCatalog catalog(config);
    RetrievalService retrieval(config, catalog);
    ContinuityService continuity(store, retrieval);
    const WorkspaceReadRequest own{"scope-a", "primary"}, shared{"scope-b", "primary"};
    const auto seed = [&](const Principal& actor, const std::string& project, const WorkspaceReadRequest& scope,
                          const Json& selector, const std::string& slot, const std::string& marker) {
        const auto input = write_request(marker, slot, selector);
        const auto saved = continuity.save(actor, project, slot, input, scope);
        check(saved.at("replayed") == false, "scoped save did not commit");
        check(continuity.save(actor, project, slot, input, scope).at("replayed") == true, "scoped save replay failed");
        auto changed = input;
        changed["draft"]["state"]["current_slice"] = "different body";
        denied([&] { continuity.save(actor, project, slot, changed, scope); }, 409, "checkpoint-request-conflict");
        return store.scoped_checkpoint_record(actor, project, slot, scope);
    };
    const Json visible_route{{"kind", "route"}, {"id", "visible-retrieval"}};
    const Json other_route{{"kind", "route"}, {"id", "other-retrieval"}};
    const auto a = seed(alice, "visible", own, visible_route, "a-private", "private-a");
    seed(alice, "visible", own, visible_route, "z-private", "private-z");
    // A cleared legacy slot retaining another workspace's declared provenance
    // must not enter this workspace's inventory or consume its page budget.
    auto writer = alice;
    writer.project_ids = {"visible"};
    auto old_provenance = a.at("provenance");
    old_provenance["workspace_id"] = "secondary";
    const auto hidden = store.save_checkpoint(writer, "visible", "b-hidden", mutation("private-a", "b-hidden"), old_provenance,
                                               a.at("checkpoint_text").get<std::string>());
    store.clear_checkpoint(writer, "visible", "b-hidden", mutation("private-a", "b-hidden", hidden.at("revision").get<std::uint64_t>()));
    auto record = store.scoped_checkpoint_record(alice, "visible", "a-private", own);
    check(record.at("owner_id") == alice.id && record.at("checkpoint_id") == a.at("checkpoint_id"), "own checkpoint differs");
    check(store.scoped_checkpoint_record(alice, "visible", "b-hidden", own).is_null(), "foreign-workspace tombstone leaked");
    denied([&] { store.clear_checkpoint(alice, "visible", "b-hidden", mutation("private-a", "b-hidden", hidden.at("revision").get<std::uint64_t>()), own); },
           404, "checkpoint-not-found");
    check(store.scoped_checkpoint_record(alice, "visible", "missing", own).is_null(), "missing checkpoint enumerated");
    const auto first = store.scoped_checkpoints(alice, "visible", "", 1, own);
    check(first.at("entries").size() == 1 && first.at("entries")[0].at("slot_id") == "a-private" && first.at("next_cursor") == "a-private",
          "checkpoint first page differs");
    const auto second = store.scoped_checkpoints(alice, "visible", "a-private", 1, own);
    check(second.at("entries").size() == 1 && second.at("entries")[0].at("slot_id") == "z-private" && second.at("next_cursor").is_null(),
          "hidden checkpoint displaced visible continuation");
    for (const auto* field : {"checkpoint_text", "checkpoint_sha256", "receipt_origins", "last_request_hash"})
        check(!first.at("entries")[0].contains(field), "checkpoint inventory leaked private payload");
    seed(alice, "visible", own, visible_route, "b-hidden", "private-a");
    check(store.checkpoint_record(writer, "visible", "b-hidden").at("state") == "cleared",
          "new scoped slot replaced a legacy tombstone");
    check(store.checkpoint_record(writer, "visible", "a-private").is_null(), "legacy read selected scoped payload");

    // Namespace/storage fixture: the second workspace has declared provenance,
    // not a newly qualified source artifact or a claim of secondary Resume.
    const WorkspaceReadRequest second_scope{"scope-a", "checkpoint-other"};
    store.register_workspace(admin, "visible", {"checkpoint-other", "Same checkout", config.database.source_host_id,
        "", "", true}, "checkpoint-other", std::string(64, 'a'));
    store.identity_bind_workspace(admin, "scope-a", "visible", {{"workspace_id", "checkpoint-other"}, {"owner_id", "alice"}});
    auto second_provenance = a.at("provenance");
    second_provenance["workspace_id"] = second_scope.workspace_id;
    const auto save_slot = [&](const WorkspaceReadRequest& target, const CheckpointMutation& request) {
        return store.save_checkpoint(alice, "visible", "namespace-slot", request,
            target.workspace_id == own.workspace_id ? a.at("provenance") : second_provenance,
            a.at("checkpoint_text"), target, store.scoped_workspace(alice, "visible", target));
    };
    const auto same_request = mutation("private-a", "namespace-slot");
    const auto legacy_slot = store.save_checkpoint(writer, "visible", "namespace-slot", same_request,
                                                   a.at("provenance"), a.at("checkpoint_text"));
    std::array<std::future<Json>, 4> pending_saves;
    for (std::size_t i = 0; i < pending_saves.size(); ++i)
        pending_saves[i] = std::async(std::launch::async, [&, i] { return save_slot(i % 2 ? second_scope : own, same_request); });
    unsigned created = 0;
    for (auto& saved : pending_saves) if (saved.get().at("replayed") == false) ++created;
    check(created == 2, "same-name workspace deliveries collided or duplicated");
    Store reopened(config.database);
    const auto first_slot = reopened.scoped_checkpoint_record(alice, "visible", "namespace-slot", own);
    const auto second_slot = reopened.scoped_checkpoint_record(alice, "visible", "namespace-slot", second_scope);
    check(first_slot.at("revision") != second_slot.at("revision") && first_slot.at("revision") != legacy_slot.at("revision"),
          "independent slot namespace reused revision");
    auto changed_request = same_request;
    changed_request.request_hash = std::string(64, 'b');
    denied([&] { save_slot(second_scope, changed_request); }, 409, "checkpoint-request-conflict");
    const auto second_update = mutation("private-a", "namespace-slot", second_slot.at("revision").get<std::uint64_t>());
    const auto updated_second = save_slot(second_scope, second_update);
    check(reopened.scoped_checkpoint_record(alice, "visible", "namespace-slot", own) == first_slot,
          "scoped update changed another workspace");
    check(reopened.checkpoint_record(writer, "visible", "namespace-slot").at("revision") == legacy_slot.at("revision"),
          "scoped update changed legacy slot");
    denied([&] { reopened.checkpoint_replay(bob, "visible", "namespace-slot", same_request, "save", second_scope); },
           404, "identity-not-found");
    const auto second_clear = mutation("private-a", "namespace-slot", updated_second.at("revision").get<std::uint64_t>());
    check(reopened.clear_checkpoint(alice, "visible", "namespace-slot", second_clear, second_scope).at("replayed") == false,
          "second workspace clear failed");
    check(reopened.clear_checkpoint(alice, "visible", "namespace-slot", second_clear, second_scope).at("replayed") == true,
          "second workspace clear replay failed");
    check(reopened.scoped_checkpoint_record(alice, "visible", "namespace-slot", own) == first_slot,
          "scoped clear crossed workspace");
    const auto second_page = reopened.scoped_checkpoints(alice, "visible", "", 1, second_scope);
    check(second_page.at("entries").size() == 1 && second_page.at("entries")[0].at("slot_id") == "namespace-slot" &&
          second_page.at("next_cursor").is_null(), "foreign slots consumed workspace page");
    check(reopened.scoped_checkpoints(alice, "visible", "namespace-slot", 1, second_scope).at("entries").empty(),
          "scoped end cursor crossed namespace");
    seed(alice, "visible", own, visible_route, "shared-slot", "alice-state");
    seed(bob, "other", shared, other_route, "shared-slot", "bob-state");
    seed(bob, "other", shared, other_route, "bob-only", "bob-private");
    check(store.scoped_checkpoint_record(alice, "visible", "shared-slot", own).at("task_id") == "task-alice-state", "same slot crossed owners");
    check(store.scoped_checkpoint_record(bob, "other", "shared-slot", shared).at("task_id") == "task-bob-state", "same slot crossed owners");
    denied([&] { store.scoped_checkpoint_record(alice, "other", "bob-only", shared); }, 404, "identity-not-found");
    check(store.scoped_checkpoint_record(admin, "other", "bob-only", shared).is_null(), "maintenance implicitly impersonated checkpoint owner");
    const auto resume = [&](const Principal& actor, const std::string& project, const std::string& slot,
                            const WorkspaceReadRequest& scope, const std::string& marker, bool impact = false) {
        return continuity.resume_workspace(actor, project, slot, scope,
            {{"schema_version", impact ? "shin.task-impact-request.v1" : "shin.task-resume-request.v1"},
             {"task_id", "task-" + marker}, {"thread_id", "thread-" + marker}, {"require_current", true}}, impact);
    };
    const auto resumed = resume(alice, "visible", "shared-slot", own, "alice-state");
    check(resumed.at("read_only") == true && resumed.at("tenant_id") == "scope-a" &&
          resumed.at("assessment").at("state").at("current_slice") == "alice-state", "native scoped resume failed");
    check(resume(alice, "visible", "shared-slot", own, "alice-state", true).at("schema_version") == "shin.task-impact.v1", "native scoped impact failed");
    for (const auto* field : {"\"root\":", "\"out_root\":", "\"checkpoint_text\":", "\"last_request_hash\":", "\"argv\":"})
        check(resumed.dump().find(field) == std::string::npos, "resume leaked host-private fields");
    denied([&] { resume(alice, "visible", "shared-slot", own, "bob-state"); }, 404, "checkpoint-not-found");
    denied([&] { resume(alice, "visible", "bob-only", own, "bob-private"); }, 404, "checkpoint-not-found");
    denied([&] { resume(bob, "visible", "a-private", own, "private-a"); }, 404, "identity-not-found");
    denied([&] { store.scoped_checkpoints(alice, "visible", "", 1, {"scope-b", "primary"}); }, 404, "identity-not-found");
    for (const auto limit : {0U, 33U}) denied([&] { store.scoped_checkpoints(alice, "visible", "", limit, own); }, 400, "invalid-limit");
    denied([&] { store.scoped_checkpoints(alice, "visible", "prune", 1, own); }, 400, "invalid-cursor");
    const auto admitted = store.scoped_workspace(alice, "visible", own);
    const auto pending = mutation("alice-state", "pending");
    const auto generated = retrieval.evaluate_continuity(store, alice, "visible", visible_route, "checkpoint",
        pending.task_id, pending.thread_id, draft("alice-state").dump(), true, Json(), Json::array(), own);
    const auto commit_pending = [&](const Json& admission) {
        return store.save_checkpoint(alice, "visible", "pending", pending, generated.at("provenance"), generated.at("value").dump(), own, admission);
    };
    store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", 3}});
    denied([&] { store.scoped_checkpoint_record(alice, "visible", "shared-slot", own); }, 404, "identity-not-found");
    denied([&] { resume(alice, "visible", "shared-slot", own, "alice-state"); }, 404, "identity-not-found");
    denied([&] { commit_pending(admitted); }, 404, "identity-not-found");
    denied([&] { store.checkpoint_replay(alice, "visible", "pending", pending, "save", own); }, 404, "identity-not-found");
    denied([&] { store.clear_checkpoint(alice, "visible", "shared-slot", pending, own); }, 404, "identity-not-found");
    denied([&] { store.prune_checkpoints(alice, "visible", own); }, 404, "identity-not-found");
    auto producer = alice;
    producer.receipt_producer = true;
    denied([&] { store.import_checkpoint_receipts(producer, "visible", "shared-slot", pending,
        Json::array({{{"producer", alice.id}}}), own); }, 404, "identity-not-found");
    store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", 4}});
    denied([&] { commit_pending(admitted); }, 409, "workspace-write-scope-changed");
    check(store.scoped_checkpoint_record(alice, "visible", "pending", own).is_null(), "denied save persisted a slot");
    const auto new_admission = store.scoped_workspace(alice, "visible", own);
    const auto committed = commit_pending(new_admission);
    check(committed.at("replayed") == false && commit_pending(new_admission).at("replayed") == true, "post-regrant save failed");
    denied([&] { store.save_checkpoint(alice, "visible", "pending", pending, generated.at("provenance"), generated.at("value").dump(), own); },
           400, "invalid-request");
    auto malformed = generated.at("provenance");
    malformed["preparation_id"] = nullptr;
    denied([&] { store.save_checkpoint(alice, "visible", "pending", pending, malformed, generated.at("value").dump(), own, new_admission); },
           400, "invalid-request");
    malformed = generated.at("provenance");
    malformed["route_id"] = "foreign-route";
    denied([&] { store.save_checkpoint(alice, "visible", "pending", pending, malformed, generated.at("value").dump(), own, new_admission); },
           400, "invalid-request");
    check(resume(alice, "visible", "shared-slot", own, "alice-state").at("read_only") == true, "checkpoint restore grant failed");
    const auto update = write_request("alice-state", "pending", visible_route, committed.at("revision").get<std::uint64_t>());
    check(continuity.save(alice, "visible", "pending", update, own).at("revision") > committed.at("revision"), "scoped CAS update failed");
    auto stale = update;
    stale["request_id"] = std::string(32, 'a');
    denied([&] { continuity.save(alice, "visible", "pending", stale, own); }, 409, "checkpoint-revision-conflict");
    const auto receipt_base = retrieval.evaluate_continuity(store, alice, "visible", visible_route, "checkpoint",
        "task-receipt", "thread-receipt", draft("receipt").dump(), true, Json(), Json::array(), own).at("value");
    auto receipt_scope = receipt_base.at("binding");
    receipt_scope["snapshot_id"] = receipt_base.at("baseline").at("snapshot_id");
    receipt_scope["worktree_id"] = receipt_base.at("baseline").at("worktree_id");
    const Json receipt{{"schema_version", "shin.execution-receipt.v1"}, {"receipt_id", "scoped-test"},
        {"kind", "test"}, {"producer", alice.id}, {"observed_at", receipt_base.at("captured_at")},
        {"ttl_seconds", 3600}, {"result", "pass"}, {"scope", receipt_scope}, {"command_fingerprint", "fnv1a64:0000000000000000"}};
    auto receipt_write = write_request("receipt", "receipt-slot", visible_route);
    receipt_write["draft"]["receipts"] = Json::array({receipt});
    const auto receipt_saved = continuity.save(alice, "visible", "receipt-slot", receipt_write, own);
    const auto receipt_record = store.scoped_checkpoint_record(alice, "visible", "receipt-slot", own);
    const auto legacy_receipt = store.save_checkpoint(writer, "visible", "receipt-slot", mutation("receipt", "legacy-import"),
        receipt_record.at("provenance"), receipt_record.at("checkpoint_text"));
    const auto import = mutation("receipt", "receipt-import", receipt_saved.at("revision").get<std::uint64_t>());
    const Json import_input{{"schema_version", "shin.task-receipt-import.v1"}, {"task_id", import.task_id},
        {"thread_id", import.thread_id}, {"expected_revision", import.expected_revision}, {"request_id", import.request_id},
        {"request_deadline", import.request_deadline}, {"receipts", Json::array({receipt})}};
    denied([&] { continuity.import_receipts(alice, "visible", "receipt-slot", import_input, own); }, 403, "receipt-import-forbidden");
    check(continuity.import_receipts(producer, "visible", "receipt-slot", import_input, own).at("replayed") == false, "scoped receipt import failed");
    check(continuity.import_receipts(producer, "visible", "receipt-slot", import_input, own).at("replayed") == true, "scoped receipt replay failed");
    check(store.scoped_checkpoint_record(alice, "visible", "receipt-slot", own).at("receipt_origins").size() == 1, "receipt origin was lost");
    check(store.checkpoint_record(writer, "visible", "receipt-slot").at("receipt_origins").empty() &&
          store.checkpoint_record(writer, "visible", "receipt-slot").at("revision") == legacy_receipt.at("revision"),
          "scoped receipt import changed legacy origin or revision");
    check(resume(producer, "visible", "receipt-slot", own, "receipt").at("read_only") == true, "receipt checkpoint resume failed");
    const auto clear = mutation("private-a", "a-private", a.at("revision").get<std::uint64_t>());
    check(store.clear_checkpoint(alice, "visible", "a-private", clear, own).at("replayed") == false, "scoped clear failed");
    check(store.clear_checkpoint(alice, "visible", "a-private", clear, own).at("replayed") == true, "scoped clear replay failed");
    check(store.prune_checkpoints(alice, "visible", own).at("removed") == 0, "prune removed protected tombstone");
    const auto short_deadline = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3;
    for (const auto* slot : {"prune-own", "prune-hidden"}) {
        auto request = mutation("private-a", slot);
        request.request_deadline = short_deadline;
        const bool hidden_slot = std::string_view(slot) == "prune-hidden";
        // The foreign-workspace row is declared legacy metadata, not a newly
        // qualified secondary source. Only that negative fixture uses grants.
        const auto saved = hidden_slot ? store.save_checkpoint(writer, "visible", slot, request, old_provenance, a.at("checkpoint_text")) :
            store.save_checkpoint(alice, "visible", slot, request, a.at("provenance"), a.at("checkpoint_text"), own,
                                  store.scoped_workspace(alice, "visible", own));
        auto clear_expired = mutation("private-a", slot, saved.at("revision").get<std::uint64_t>());
        clear_expired.request_deadline = short_deadline;
        if (hidden_slot) store.clear_checkpoint(writer, "visible", slot, clear_expired);
        else store.clear_checkpoint(alice, "visible", slot, clear_expired, own);
    }
    const auto wait_end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto other_expiring = mutation("private-a", "prune-own");
    other_expiring.request_deadline = short_deadline;
    const auto other_expiring_saved = store.save_checkpoint(alice, "visible", "prune-own", other_expiring,
        second_provenance, a.at("checkpoint_text"), second_scope, store.scoped_workspace(alice, "visible", second_scope));
    auto other_expiring_clear = mutation("private-a", "prune-own", other_expiring_saved.at("revision").get<std::uint64_t>());
    other_expiring_clear.request_deadline = short_deadline;
    store.clear_checkpoint(alice, "visible", "prune-own", other_expiring_clear, second_scope);
    while (true) {
        const auto pruned = store.prune_checkpoints(alice, "visible", own).at("removed").get<unsigned>();
        if (pruned != 0) { check(pruned == 1, "scoped prune crossed workspace"); break; }
        if (std::chrono::steady_clock::now() >= wait_end) throw std::runtime_error("scoped expiry did not advance");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(store.scoped_checkpoint_record(alice, "visible", "prune-own", own).is_null(), "expired own tombstone retained");
    check(!store.checkpoint_record(writer, "visible", "prune-hidden").is_null(), "prune removed foreign-workspace tombstone");
    check(!store.scoped_checkpoint_record(alice, "visible", "prune-own", second_scope).is_null(),
          "prune removed same-name slot in another workspace");
    check(store.prune_checkpoints(writer, "visible").at("removed") == 1, "legacy prune did not retain scoped tombstones");
    check(store.prune_checkpoints(alice, "visible", second_scope).at("removed") == 1, "second workspace prune failed");
    denied([&] { resume(alice, "visible", "a-private", own, "private-a"); }, 410, "checkpoint-cleared");
    const auto published = seed(alice, "visible", own, {{"kind", "publication"}, {"id", "scoped-publication"}}, "publication", "publication-state");
    for (const bool impact : {false, true})
        denied([&] { continuity.resume_workspace(alice, "visible", "publication", own,
            {{"schema_version", impact ? "shin.task-impact-request.v1" : "shin.task-resume-request.v1"},
             {"task_id", "task-publication-state"}, {"thread_id", "thread-publication-state"}, {"require_current", true}}, impact, true); },
            403, "publication-deferred");
    check(resume(alice, "visible", "publication", own, "publication-state").at("read_only") == true, "publication checkpoint resume failed");
    store.withdraw_publication(admin, "visible", "scoped-publication", published.at("provenance").at("revision").get<std::uint64_t>());
    denied([&] { resume(alice, "visible", "publication", own, "publication-state"); }, 404, "identity-not-found");
    denied([&] { store.save_checkpoint(alice, "visible", "withdrawn-write", mutation("publication-state", "withdrawn-write"),
        published.at("provenance"), published.at("checkpoint_text").get<std::string>(), own, store.scoped_workspace(alice, "visible", own)); },
        409, "checkpoint-source-scope-changed");
    check(store.scoped_checkpoint_record(alice, "visible", "withdrawn-write", own).is_null(), "withdrawn source persisted");
    denied([&] { store.identity_request_admission(alice, false); }, 403, "identity-data-plane-not-enabled");
    std::cout << Json{{"ok", true}, {"checks", checks}, {"scoped_checkpoints", true}, {"scoped_mutations", true}}.dump() << '\n';
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { run(argv[1]); }
    catch (const shin::server::ServiceError& error) { std::cerr << error.code << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
