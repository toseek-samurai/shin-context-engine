#include "shin/server/identity_directory.h"
#include "legacy_project_access.h"
#include "shin/server/config.h"
#include "shin/server/store.h"

#include <pqxx/pqxx>
#include <limits>

namespace shin::server {
namespace {
[[noreturn]] void denied() { throw ServiceError(404, "identity-not-found", "Identity resource was not found."); }
[[noreturn]] void conflict() { throw ServiceError(409, "identity-conflict", "Identity state changed or conflicts with the request."); }
void identifier(const std::string& value) {
    if (!valid_identifier(value)) throw ServiceError(400, "invalid-request", "Invalid identity request.");
}
void ops(const Principal& actor) {
    if (!actor.admin) throw ServiceError(403, "forbidden", "Directory administration is required.");
}
std::string text(const Json& input, const char* key) { return require_text(input, key, 120); }
std::uint64_t revision(const Json& input) {
    const auto& value = input.at("expected_revision");
    if (!value.is_number_unsigned() && !value.is_number_integer())
        throw ServiceError(400, "invalid-request", "Invalid expected revision.");
    if ((value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
        value.get<std::uint64_t>() >= static_cast<std::uint64_t>(INT64_MAX))
        throw ServiceError(400, "invalid-request", "Invalid expected revision.");
    return value.get<std::uint64_t>();
}
bool enabled(const Json& input) {
    if (!input.at("enabled").is_boolean()) throw ServiceError(400, "invalid-request", "Invalid enabled state.");
    return input.at("enabled").get<bool>();
}
void write_lock(pqxx::transaction_base& tx) {
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.identity_directory.v1'))");
}
void audit(pqxx::transaction_base& tx, const Principal& actor, const std::string& tenant,
           const char* action, const std::string& target, std::uint64_t rev) {
    tx.exec("INSERT INTO shin_server.identity_audit(actor_id,tenant_id,action,target_id,revision) "
            "VALUES($1,NULLIF($2,''),$3,$4,$5)", pqxx::params{tx, actor.id, tenant, action, target, rev});
}
template<class Row> Json account_json(const Row& row) {
    return {{"id", row["id"].template as<std::string>()}, {"principal_id", row["principal_id"].template as<std::string>()},
            {"display_name", row["display_name"].template as<std::string>()}, {"enabled", row["enabled"].template as<bool>()},
            {"revision", row["revision"].template as<std::uint64_t>()}};
}
template<class Row> Json tenant_json(const Row& row) {
    return {{"id", row["id"].template as<std::string>()}, {"display_name", row["display_name"].template as<std::string>()},
            {"owner_id", row["owner_id"].template as<std::string>()}, {"content_access", "scoped-routes-only"}};
}
void page(const std::string& after, std::size_t limit) {
    if ((!after.empty() && !valid_identifier(after)) || limit == 0 || limit > 100)
        throw ServiceError(400, "invalid-request", "Invalid page boundary.");
}
}

bool identity_role_allows(std::string_view role, IdentityOperation operation) {
    switch (operation) {
    case IdentityOperation::tenant_read: return role == "owner";
    case IdentityOperation::membership_write: return false;
    }
    return false;
}

void Store::identity_request_admission(const Principal& actor, bool directory_request) {
    identifier(actor.id);
    legacy_access::validate_principal(actor);
    if (actor.admin) return;
    std::string legacy_projects;
    for (const auto& project : actor.project_ids) {
        if (!legacy_projects.empty()) legacy_projects += ',';
        legacy_projects += project;
    }
    (void)run_transaction([&](auto& tx) {
        tx.exec("SELECT set_config('shin.identity_principal',$1,true), set_config('shin.identity_ops','0',true), "
                "set_config('shin.identity_account','',true), set_config('shin.identity_tenant','',true), "
                "set_config('shin.identity_manage','',true), set_config('shin.identity_legacy_projects',$2,true)",
                pqxx::params{tx, actor.id, legacy_projects});
        const auto account = tx.exec("SELECT enabled FROM shin_server.identity_accounts WHERE principal_id=$1", pqxx::params{tx, actor.id});
        if (!account.empty()) {
            if (!account[0]["enabled"].template as<bool>())
                throw ServiceError(401, "account-disabled", "Account access is disabled.");
            // Personal handlers use live workspace admission; legacy routes remain fenced.
            if (!directory_request)
                throw ServiceError(403, "identity-data-plane-not-enabled", "Use the personal workspace routes; legacy data access is unavailable.");
        }
        // A bound project must not remain reachable through an unenrolled
        // static-grant principal through legacy project-wide APIs.
        if (!directory_request && !tx.exec("SELECT 1 FROM shin_server.identity_project_owners WHERE "
                "project_id=ANY(string_to_array(current_setting('shin.identity_legacy_projects'),',')) LIMIT 1").empty())
            throw ServiceError(403, "identity-data-plane-not-enabled", "Bound projects require personal workspace routes.");
        return Json{{"admitted", true}};
    });
}

void identity_scope(pqxx::transaction_base& tx, const Principal& actor) {
    identifier(actor.id);
    tx.exec("SELECT set_config('shin.identity_principal',$1,true), set_config('shin.identity_ops',$2,true), "
            "set_config('shin.identity_account','',true), set_config('shin.identity_tenant','',true), "
            "set_config('shin.identity_manage','',true), set_config('shin.identity_legacy_projects','',true), "
            "set_config('shin.content_project','',true), set_config('shin.content_workspace','',true), "
            "set_config('shin.content_route','',true), "
            "set_config('shin.preparation_sources','',true), set_config('shin.preparation_legacy','0',true)",
            pqxx::params{tx, actor.id, actor.admin ? "1" : "0"});
    if (actor.admin) return; // Explicit legacy maintenance authority, not a tenant role.
    const auto user = tx.exec("SELECT id,enabled FROM shin_server.identity_accounts WHERE principal_id=$1",
                              pqxx::params{tx, actor.id});
    if (user.empty() || !user[0]["enabled"].as<bool>()) denied();
    tx.exec("SELECT set_config('shin.identity_account',$1,true)", pqxx::params{tx, user[0]["id"].as<std::string>()});
}

void authorize_identity_tenant(pqxx::transaction_base& tx, const Principal& actor,
                               const std::string& tenant, IdentityOperation operation) {
    identifier(tenant);
    identity_scope(tx, actor);
    if (!actor.admin) {
        if (tx.exec("SELECT id FROM shin_server.identity_tenants WHERE id=$1 AND owner_id=current_setting('shin.identity_account')",
                    pqxx::params{tx, tenant}).empty()) denied();
        const auto membership = tx.exec("SELECT role FROM shin_server.identity_memberships "
            "WHERE tenant_id=$1 AND account_id=current_setting('shin.identity_account') AND enabled",
            pqxx::params{tx, tenant});
        if (membership.empty()) denied();
        if (!identity_role_allows(membership[0]["role"].as<std::string>(), operation))
            throw ServiceError(403, "forbidden", "Tenant operation is not permitted.");
    }
    if (tx.exec("SELECT id FROM shin_server.identity_tenants WHERE id=$1", pqxx::params{tx, tenant}).empty()) denied();
    tx.exec("SELECT set_config('shin.identity_tenant',$1,true), set_config('shin.identity_manage',$2,true)",
            pqxx::params{tx, tenant, operation == IdentityOperation::membership_write ? "1" : "0"});
}

Json Store::identity_register_account(const Principal& actor, const Json& input) {
    ops(actor);
    require_fields(input, {"id", "principal_id", "display_name"});
    const auto id = text(input, "id"), principal_id = text(input, "principal_id"), name = text(input, "display_name");
    identifier(id); identifier(principal_id);
    if (principal_id == actor.id) throw ServiceError(400, "invalid-request", "Maintenance identity cannot enroll itself.");
    return run_transaction([&](auto& tx) {
        write_lock(tx); identity_scope(tx, actor);
        const auto existing = tx.exec("SELECT * FROM shin_server.identity_accounts WHERE id=$1 OR principal_id=$2",
                                      pqxx::params{tx, id, principal_id});
        if (!existing.empty()) {
            if (existing.size() != 1 || existing[0]["id"].template as<std::string>() != id ||
                existing[0]["principal_id"].template as<std::string>() != principal_id ||
                existing[0]["display_name"].template as<std::string>() != name) conflict();
            return Json{{"account", account_json(existing[0])}, {"replayed", true}};
        }
        const auto row = tx.exec("INSERT INTO shin_server.identity_accounts(id,principal_id,display_name) VALUES($1,$2,$3) RETURNING *",
                                  pqxx::params{tx, id, principal_id, name});
        audit(tx, actor, "", "account.created", id, 1);
        return Json{{"account", account_json(row[0])}, {"replayed", false}};
    });
}

Json Store::identity_account(const Principal& actor) {
    return run_transaction([&](auto& tx) {
        identity_scope(tx, actor);
        const auto rows = tx.exec("SELECT * FROM shin_server.identity_accounts WHERE principal_id=$1", pqxx::params{tx, actor.id});
        if (rows.empty()) denied();
        return account_json(rows[0]);
    });
}

Json Store::identity_accounts(const Principal& actor, const std::string& after, std::size_t limit) {
    ops(actor); page(after, limit);
    return run_transaction([&](auto& tx) {
        identity_scope(tx, actor);
        const auto rows = tx.exec("SELECT * FROM shin_server.identity_accounts WHERE id>$1 ORDER BY id LIMIT $2",
                                  pqxx::params{tx, after, limit + 1});
        const auto count = static_cast<std::size_t>(rows.size());
        Json items = Json::array();
        for (std::size_t i = 0; i < count && i < limit; ++i) items.push_back(account_json(rows[i]));
        return Json{{"items", items}, {"next_cursor", count > limit ? items.back()["id"] : Json(nullptr)}};
    });
}

Json Store::identity_account_state(const Principal& actor, const std::string& id, const Json& input) {
    ops(actor); identifier(id); require_fields(input, {"enabled", "expected_revision"});
    const auto state = enabled(input); const auto expected = revision(input);
    return run_transaction([&](auto& tx) {
        write_lock(tx); identity_scope(tx, actor);
        const auto rows = tx.exec("UPDATE shin_server.identity_accounts SET enabled=$2,revision=revision+1 "
            "WHERE id=$1 AND revision=$3 RETURNING *", pqxx::params{tx, id, state, expected});
        if (rows.empty()) conflict();
        audit(tx, actor, "", "account.state", id, expected + 1);
        return account_json(rows[0]);
    });
}

Json Store::identity_register_tenant(const Principal& actor, const Json& input) {
    ops(actor); require_fields(input, {"id", "display_name", "owner_id"});
    const auto id = text(input, "id"), name = text(input, "display_name"), owner = text(input, "owner_id");
    identifier(id); identifier(owner);
    return run_transaction([&](auto& tx) {
        write_lock(tx); identity_scope(tx, actor);
        const auto existing = tx.exec("SELECT * FROM shin_server.identity_tenants WHERE id=$1", pqxx::params{tx, id});
        if (!existing.empty()) {
            if (existing[0]["display_name"].template as<std::string>() != name || existing[0]["owner_id"].template as<std::string>() != owner) conflict();
            return Json{{"tenant", tenant_json(existing[0])}, {"replayed", true}};
        }
        if (tx.exec("SELECT id FROM shin_server.identity_accounts WHERE id=$1 AND enabled", pqxx::params{tx, owner}).empty()) denied();
        const auto rows = tx.exec("INSERT INTO shin_server.identity_tenants(id,display_name,owner_id) VALUES($1,$2,$3) RETURNING *",
                                  pqxx::params{tx, id, name, owner});
        tx.exec("INSERT INTO shin_server.identity_memberships VALUES($1,$2,'owner',true,1)", pqxx::params{tx, id, owner});
        audit(tx, actor, id, "tenant.created", id, 1);
        return Json{{"tenant", tenant_json(rows[0])}, {"replayed", false}};
    });
}

Json Store::identity_tenants(const Principal& actor, const std::string& after, std::size_t limit) {
    page(after, limit);
    return run_transaction([&](auto& tx) {
        identity_scope(tx, actor);
        const auto rows = tx.exec("SELECT * FROM shin_server.identity_tenants WHERE id>$1 ORDER BY id LIMIT $2",
                                  pqxx::params{tx, after, limit + 1});
        const auto count = static_cast<std::size_t>(rows.size());
        Json items = Json::array();
        for (std::size_t i = 0; i < count && i < limit; ++i) items.push_back(tenant_json(rows[i]));
        return Json{{"items", items}, {"next_cursor", count > limit ? items.back()["id"] : Json(nullptr)}};
    });
}

Json Store::identity_tenant(const Principal& actor, const std::string& tenant) {
    return run_transaction([&](auto& tx) {
        authorize_identity_tenant(tx, actor, tenant, IdentityOperation::tenant_read);
        return tenant_json(tx.exec("SELECT * FROM shin_server.identity_tenants WHERE id=$1", pqxx::params{tx, tenant})[0]);
    });
}

Json Store::identity_membership(const Principal& actor, const std::string& tenant, const Json& input) {
    (void)actor; (void)tenant; (void)input;
    throw ServiceError(403, "sharing-disabled", "This Server release supports personal spaces only.");
}
} // namespace shin::server
