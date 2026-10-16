#include "shin/server/config.h"
#include "shin/server/oidc.h"
#include "shin/server/store.h"
#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    using namespace shin::server;
    try {
        if (argc != 2) return 2;
        const auto config = load_config(argv[1]);
        if (config.database.expected_database != "shin_server_dev" || config.database.runtime_role != "shin_runtime") return 2;
        std::string input;
        char c = 0;
        while (std::cin.get(c)) { if (input.size() >= 4096) return 2; input.push_back(c); }
        const auto request = parse_bounded_json(input, 4096);
        // Synthetic trusted evidence tests Store only, not token verification or HTTP login.
        const VerifiedOidcIdentity identity{request.at("issuer"),request.at("subject"),request.value("client_id","shin-ce"),
            request.value("jti","fixture-token"),std::chrono::system_clock::now() + std::chrono::seconds(request.value("expiry",60)),request.at("sid")};
        Store store(config.database);
        Json result;
        if (request.at("command") == "register") result = store.identity_register_device(identity, request.at("input"));
        else if (request.at("command") == "resolve") {
            std::vector<double> times;
            const auto count = request.value("samples",1);
            if (count < 1 || count > 30) return 2;
            for (int i = 0; i < count; ++i) {
                const auto start = std::chrono::steady_clock::now();
                const auto actor = store.identity_resolve_device(identity);
                if (actor.admin || actor.receipt_producer || !actor.project_ids.empty()) return 1;
                result["principal_id"] = actor.id;
                times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            }
            std::sort(times.begin(),times.end());
            result["lookup_ms_p95"] = times[static_cast<std::size_t>((count-1)*95/100)];
        } else return 2;
        std::cout << Json{{"ok",true},{"result",result}}.dump() << '\n';
    } catch (const ServiceError& e) {
        try { std::cout << Json{{"ok",false},{"code",e.code},{"status",e.status}}.dump() << '\n'; }
        catch (...) { return 2; }
    } catch (const std::exception&) { std::cerr << "device fixture failed\n"; return 2; }
}
