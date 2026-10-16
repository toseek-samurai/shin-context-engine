#include "shin/server/config.h"
#include "shin/server/oidc.h"
#include "shin/server/store.h"
#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    using namespace shin::server;
    try {
        if (argc != 5) return 2;
        const auto config = load_config(argv[1]);
        if (config.database.expected_database != "shin_server_dev" || config.database.runtime_role != "shin_runtime") return 2;
        Store store(config.database);
        // Store-only fixture. Cryptographic verification has independent coverage;
        // this deliberately does not pretend to be an HTTP OIDC login.
        const VerifiedOidcIdentity identity{argv[2], argv[3], "shin-ce", "fixture-token",
            std::chrono::system_clock::now() + std::chrono::seconds(60), {}};
        const std::string expected = argv[4];
        if (expected == "denied") {
            try { (void)store.identity_resolve_external(identity); }
            catch (const ServiceError& e) {
                if (e.status != 401 || e.code != "external-identity-unavailable") throw;
                std::cout << "{\"ok\":true,\"denied\":true}\n";
                return 0;
            }
            return 1;
        }
        std::vector<double> times;
        times.reserve(30);
        for (int i = 0; i < 32; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto actor = store.identity_resolve_external(identity);
            const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
            if (actor.id != expected || actor.admin || actor.receipt_producer || !actor.project_ids.empty()) return 1;
            if (i >= 2) times.push_back(elapsed);
            if (store.identity_account(actor).at("principal_id") != expected) return 1;
            try { (void)store.identity_accounts(actor, "", 10); return 1; }
            catch (const ServiceError& e) { if (e.status != 403) throw; }
        }
        std::sort(times.begin(), times.end());
        auto expired = identity;
        expired.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(31);
        try { (void)store.identity_resolve_external(expired); return 1; }
        catch (const ServiceError& e) { if (e.status != 401) throw; }
        std::cout << Json{{"ok",true},{"samples",30},{"lookup_ms_p50",times[14]},{"lookup_ms_p95",times[28]}}.dump() << '\n';
        return 0;
    } catch (const ServiceError& e) { std::cerr << e.code << '\n'; return 1; }
    catch (const std::exception&) { std::cerr << "external identity fixture failed\n"; return 1; }
}
