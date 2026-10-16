#include "shin/server/store.h"
#include "shin/server/config.h"
#include "shin/server/identity_directory.h"
#include "shin/server/oidc.h"
#include <pqxx/pqxx>
#include <algorithm>

namespace shin::server {
namespace {
[[noreturn]] void unavailable() {
    throw ServiceError(401, "device-session-unavailable", "Device session access is unavailable.");
}
[[noreturn]] void missing() {
    throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
}
void identifier(const std::string& value) {
    if (!valid_identifier(value)) throw ServiceError(400, "invalid-request", "Invalid identity identifier.");
}
void write_lock(pqxx::transaction_base& tx) {
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.identity_directory.v1'))");
}
void token(const VerifiedOidcIdentity& identity) {
    const auto text = [](const auto& value, std::size_t max) {
        return !value.empty() && value.size() <= max &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; });
    };
    if (!text(identity.issuer, 2048) || !text(identity.subject, 255) ||
        !text(identity.client_id, 255) || !text(identity.session_id, 255) ||
        identity.expires_at <= std::chrono::system_clock::now() - std::chrono::seconds(30)) unavailable();
}
pqxx::row account(pqxx::transaction_base& tx, const VerifiedOidcIdentity& identity) {
    token(identity);
    // Fixed authentication lookup; this internal scope never leaves this transaction.
    identity_scope(tx, Principal{"device-resolver", true, {}});
    const auto rows = tx.exec("SELECT a.id,a.principal_id FROM shin_server.identity_accounts a "
        "JOIN shin_server.identity_external_bindings b ON b.account_id=a.id "
        "WHERE a.enabled AND b.enabled AND b.issuer=$1 AND b.subject=$2",
        pqxx::params{tx, identity.issuer, identity.subject});
    if (rows.size() != 1) unavailable();
    return pqxx::row(rows[0]);
}
void own_account(pqxx::transaction_base& tx, const Principal& actor, const std::string& id) {
    identity_scope(tx, actor);
    if (tx.exec("SELECT id FROM shin_server.identity_accounts WHERE id=$1", pqxx::params{tx, id}).empty()) missing();
}
template<class Row> Json metadata(const Row& row) {
    return {{"id",row["id"].template as<std::string>()}, {"account_id",row["account_id"].template as<std::string>()},
        {"display_name",row["display_name"].template as<std::string>()}, {"revoked",row["revoked"].template as<bool>()},
        {"revision",row["revision"].template as<std::uint64_t>()}, {"created_at",row["created_at"].template as<std::string>()}};
}
void audit(pqxx::transaction_base& tx, const std::string& actor, const char* action,
           const std::string& device, std::uint64_t revision) {
    // Authorization/mutation already succeeded. Only this fixed metadata insert
    // uses internal audit authority; the transaction ends before returning.
    tx.exec("SELECT set_config('shin.identity_principal',$1,true), set_config('shin.identity_ops','1',true)",
            pqxx::params{tx, actor});
    tx.exec("INSERT INTO shin_server.identity_audit(actor_id,action,target_id,revision) VALUES($1,$2,$3,$4)",
        pqxx::params{tx, actor, action, device, revision});
}
}

Json Store::identity_register_device(const VerifiedOidcIdentity& identity, const Json& input) {
    require_fields(input, {"display_name"});
    const auto name = require_text(input, "display_name", 120);
    if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw ServiceError(400, "invalid-request", "Invalid device label.");
    return run_transaction([&](auto& tx) {
        write_lock(tx);
        const auto owner = account(tx, identity);
        const auto id = owner["id"].template as<std::string>();
        const auto existing = tx.exec("SELECT * FROM shin_server.identity_devices "
            "WHERE account_id=$1 AND client_id=$2 AND session_id=$3", pqxx::params{tx, id, identity.client_id, identity.session_id});
        if (!existing.empty()) {
            if (existing[0]["revoked"].template as<bool>()) unavailable();
            return Json{{"device",metadata(existing[0])},{"replayed",true}};
        }
        if (tx.exec("SELECT count(*) FROM shin_server.identity_devices WHERE account_id=$1 AND NOT revoked",
                    pqxx::params{tx, id})[0][0].template as<unsigned>() >= 32)
            throw ServiceError(409, "device-limit-reached", "Revoke an unused device session before enrolling another.");
        const auto rows = tx.exec("INSERT INTO shin_server.identity_devices(account_id,client_id,session_id,display_name) "
            "VALUES($1,$2,$3,$4) RETURNING *", pqxx::params{tx, id, identity.client_id, identity.session_id, name});
        const auto result = metadata(rows[0]);
        audit(tx, owner["principal_id"].template as<std::string>(), "device.registered", result.at("id").template get<std::string>(), 1);
        return Json{{"device",result},{"replayed",false}};
    });
}

Principal Store::identity_resolve_device(const VerifiedOidcIdentity& identity) {
    const auto result = run_transaction([&](auto& tx) {
        token(identity);
        identity_scope(tx, Principal{"device-resolver", true, {}});
        const auto rows = tx.exec("SELECT a.principal_id FROM shin_server.identity_devices d "
            "JOIN shin_server.identity_external_bindings b ON b.account_id=d.account_id "
            "JOIN shin_server.identity_accounts a ON a.id=b.account_id "
            "WHERE b.issuer=$1 AND b.subject=$2 AND d.client_id=$3 AND d.session_id=$4 "
            "AND a.enabled AND b.enabled AND NOT d.revoked",
            pqxx::params{tx, identity.issuer, identity.subject, identity.client_id, identity.session_id});
        if (rows.size() != 1) unavailable();
        return Json{{"principal_id",rows[0]["principal_id"].template as<std::string>()}};
    });
    return Principal{result.at("principal_id").get<std::string>(), false, {}, false};
}

Json Store::identity_devices(const Principal& actor, const std::string& id, const std::string& after, std::size_t limit) {
    identifier(id);
    if (!after.empty()) identifier(after);
    if (!limit || limit > 100) throw ServiceError(400, "invalid-request", "Invalid page boundary.");
    return run_transaction([&](auto& tx) {
        own_account(tx, actor, id);
        const auto rows = tx.exec("SELECT * FROM shin_server.identity_devices WHERE account_id=$1 AND id>$2 ORDER BY id LIMIT $3",
                                 pqxx::params{tx, id, after, limit + 1});
        Json items = Json::array();
        for (std::size_t i = 0; i < static_cast<std::size_t>(rows.size()) && i < limit; ++i) items.push_back(metadata(rows[i]));
        return Json{{"items",items},{"next_cursor",static_cast<std::size_t>(rows.size()) > limit ? items.back()["id"] : Json(nullptr)}};
    });
}

Json Store::identity_revoke_device(const Principal& actor, const std::string& id, const std::string& device, const Json& input) {
    identifier(id); identifier(device); require_fields(input, {"expected_revision"});
    const auto& revision = input.at("expected_revision");
    if (!revision.is_number_integer() || revision < 1 || revision >= INT64_MAX)
        throw ServiceError(400, "invalid-request", "Invalid expected revision.");
    const auto expected = revision.get<std::uint64_t>();
    return run_transaction([&](auto& tx) {
        write_lock(tx); own_account(tx, actor, id);
        const auto rows = tx.exec("SELECT * FROM shin_server.identity_devices WHERE account_id=$1 AND id=$2", pqxx::params{tx, id, device});
        if (rows.empty()) missing();
        const auto current = rows[0]["revision"].template as<std::uint64_t>();
        if (rows[0]["revoked"].template as<bool>() && (expected == current || expected + 1 == current))
            return Json{{"device",metadata(rows[0])},{"replayed",true}};
        if (current != expected) throw ServiceError(409, "identity-conflict", "Device state changed.");
        const auto changed = tx.exec("UPDATE shin_server.identity_devices SET revoked=true,revision=revision+1 "
            "WHERE account_id=$1 AND id=$2 RETURNING *", pqxx::params{tx, id, device});
        audit(tx, actor.id, "device.revoked", device, current + 1);
        return Json{{"device",metadata(changed[0])},{"replayed",false}};
    });
}

Json Store::identity_revoke_devices(const Principal& actor, const std::string& id) {
    identifier(id);
    return run_transaction([&](auto& tx) {
        write_lock(tx); own_account(tx, actor, id);
        const auto rows = tx.exec("UPDATE shin_server.identity_devices SET revoked=true,revision=revision+1 "
            "WHERE account_id=$1 AND NOT revoked RETURNING id,revision", pqxx::params{tx, id});
        for (const auto& row : rows) audit(tx, actor.id, "device.revoked", row["id"].template as<std::string>(), row["revision"].template as<std::uint64_t>());
        return Json{{"revoked_count",rows.size()}};
    });
}
} // namespace shin::server
