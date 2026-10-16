#include "shin/server/store.h"
#include "shin/server/config.h"
#include "shin/server/identity_directory.h"
#include "shin/server/oidc.h"
#include <pqxx/pqxx>
#include <algorithm>

namespace shin::server {
namespace {
void maintenance(const Principal& actor) {
    if (!actor.admin) throw ServiceError(403, "forbidden", "Directory administration is required.");
}
void account_id(const std::string& account) {
    if (!valid_identifier(account)) throw ServiceError(400, "invalid-request", "Invalid account identifier.");
}
void external_key(const std::string& issuer, const std::string& subject) {
    const auto ascii = [](const auto& value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; });
    };
    if (issuer.size() < 9 || issuer.size() > 2048 || !issuer.starts_with("https://") || issuer[8] == '/' ||
        issuer.find_first_of("?#@\\") != std::string::npos || !ascii(issuer) ||
        subject.empty() || subject.size() > 255 || !ascii(subject))
        throw ServiceError(400, "invalid-request", "Invalid external identity key.");
}
void write_scope(pqxx::transaction_base& tx, const Principal& actor) {
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.identity_directory.v1'))");
    identity_scope(tx, actor);
}
[[noreturn]] void not_found() { throw ServiceError(404, "identity-not-found", "Identity resource was not found."); }
[[noreturn]] void conflict() { throw ServiceError(409, "identity-conflict", "Identity state changed or conflicts with the request."); }
void audit(pqxx::transaction_base& tx, const Principal& actor, const std::string& account,
           const char* action, std::uint64_t revision) {
    tx.exec("INSERT INTO shin_server.identity_audit(actor_id,action,target_id,revision) VALUES($1,$2,$3,$4)",
            pqxx::params{tx, actor.id, action, account, revision});
}
template<class Row> Json binding(const Row& row) {
    return {{"account_id", row["account_id"].template as<std::string>()},
        {"issuer", row["issuer"].template as<std::string>()}, {"subject", row["subject"].template as<std::string>()},
        {"enabled", row["enabled"].template as<bool>()}, {"revision", row["revision"].template as<std::uint64_t>()}};
}
}

Json Store::identity_bind_external(const Principal& actor, const std::string& account, const Json& input) {
    maintenance(actor); account_id(account); require_fields(input, {"issuer", "subject"});
    const auto issuer = require_text(input, "issuer", 2048), subject = require_text(input, "subject", 255);
    external_key(issuer, subject);
    return run_transaction([&](auto& tx) {
        write_scope(tx, actor);
        if (tx.exec("SELECT id FROM shin_server.identity_accounts WHERE id=$1", pqxx::params{tx, account}).empty()) not_found();
        const auto existing = tx.exec("SELECT * FROM shin_server.identity_external_bindings "
            "WHERE account_id=$1 OR (issuer=$2 AND subject=$3)", pqxx::params{tx, account, issuer, subject});
        if (!existing.empty()) {
            if (existing.size() != 1 || existing[0]["account_id"].template as<std::string>() != account ||
                existing[0]["issuer"].template as<std::string>() != issuer || existing[0]["subject"].template as<std::string>() != subject)
                conflict();
            return Json{{"binding", binding(existing[0])}, {"replayed", true}};
        }
        const auto row = tx.exec("INSERT INTO shin_server.identity_external_bindings(account_id,issuer,subject) "
            "VALUES($1,$2,$3) RETURNING *", pqxx::params{tx, account, issuer, subject});
        audit(tx, actor, account, "external.bound", 1);
        return Json{{"binding", binding(row[0])}, {"replayed", false}};
    });
}

Json Store::identity_external(const Principal& actor, const std::string& account) {
    account_id(account);
    return run_transaction([&](auto& tx) {
        identity_scope(tx, actor);
        const auto rows = tx.exec("SELECT * FROM shin_server.identity_external_bindings WHERE account_id=$1",
                                  pqxx::params{tx, account});
        if (rows.empty()) not_found();
        return binding(rows[0]);
    });
}

Json Store::identity_external_state(const Principal& actor, const std::string& account, const Json& input) {
    maintenance(actor); account_id(account); require_fields(input, {"enabled", "expected_revision"});
    const auto& revision = input.at("expected_revision");
    if (!input.at("enabled").is_boolean() || !revision.is_number_integer() || revision < 1 || revision >= INT64_MAX)
        throw ServiceError(400, "invalid-request", "Invalid binding state or revision.");
    const auto enabled = input.at("enabled").get<bool>();
    const auto expected = revision.get<std::uint64_t>();
    return run_transaction([&](auto& tx) {
        write_scope(tx, actor);
        const auto row = tx.exec("UPDATE shin_server.identity_external_bindings SET enabled=$2,revision=revision+1 "
            "WHERE account_id=$1 AND revision=$3 RETURNING *", pqxx::params{tx, account, enabled, expected});
        if (row.empty()) conflict();
        audit(tx, actor, account, "external.state", expected + 1);
        return binding(row[0]);
    });
}

Principal Store::identity_resolve_external(const VerifiedOidcIdentity& identity) {
    external_key(identity.issuer, identity.subject);
    const auto result = run_transaction([&](auto& tx) {
        if (identity.expires_at <= std::chrono::system_clock::now() - std::chrono::seconds(30))
            throw ServiceError(401, "external-identity-unavailable", "External account access is unavailable.");
        // Fixed internal authentication lookup only, never user SQL/content.
        // The transaction ends before the resolved personal Principal is used.
        identity_scope(tx, Principal{"identity-resolver", true, {}});
        const auto row = tx.exec("SELECT a.principal_id FROM shin_server.identity_external_bindings b "
            "JOIN shin_server.identity_accounts a ON a.id=b.account_id "
            "WHERE b.issuer=$1 AND b.subject=$2 AND b.enabled AND a.enabled",
            pqxx::params{tx, identity.issuer, identity.subject});
        if (row.size() != 1)
            throw ServiceError(401, "external-identity-unavailable", "External account access is unavailable.");
        return Json{{"principal_id", row[0]["principal_id"].template as<std::string>()}};
    });
    return Principal{result.at("principal_id").get<std::string>(), false, {}, false};
}
} // namespace shin::server
