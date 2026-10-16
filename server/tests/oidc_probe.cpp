#include "shin/server/oidc.h"
#include "shin/server/oidc_provider.h"
#include "shin/server/config.h"
#include <algorithm>
#include <iostream>

int main() {
    using namespace shin::server;
    // Test-only transport: secrets arrive on a bounded pipe, never command arguments.
    try {
        std::string input;
        char c = 0;
        while (std::cin.get(c)) {
            if (input.size() >= 131072) return 2;
            input.push_back(c);
        }
        const auto request = parse_bounded_json(input, 131072);
        OidcTrust trust{request.at("issuer"), request.at("audience"), request.at("client_id")};
        std::unique_ptr<OidcVerifier> verifier;
        std::unique_ptr<OidcProvider> provider;
        if (request.contains("provider")) {
            OidcProviderOptions options;
            options.trust = trust;
            options.ca_file = request.at("provider").at("ca_file").get<std::string>();
            provider = std::make_unique<OidcProvider>(std::move(options));
            if (!provider->wait_ready(std::chrono::milliseconds(3000)))
                throw ServiceError(503, "oidc-keys-unavailable", "Provider did not become ready.");
        } else verifier = std::make_unique<OidcVerifier>(trust, request.at("jwks").dump());
        const auto verify = [&](const std::string& value) {
            return provider ? provider->verify(value) : verifier->verify(value);
        };
        std::vector<double> times;
        times.reserve(500);
        const auto token = request.at("token").get<std::string>();
        auto identity = verify(token);
        for (int i = 0; i < 500; ++i) {
            const auto start = std::chrono::steady_clock::now();
            identity = verify(token);
            times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        std::cout << Json{{"ok", true}, {"subject", identity.subject}, {"issuer", identity.issuer},
            {"session_id", identity.session_id},
            {"samples", times.size()}, {"verify_us_p50", times[249]}, {"verify_us_p95", times[474]},
            {"provider", provider ? provider->status() : Json(nullptr)}}.dump() << '\n';
        return 0;
    } catch (const ServiceError& error) {
        try { std::cout << Json{{"ok", false}, {"code", error.code}}.dump() << '\n'; }
        catch (...) { std::cout << "{\"ok\":false,\"code\":\"probe-error\"}\n"; }
        return 1;
    } catch (const std::exception&) {
        std::cout << "{\"ok\":false,\"code\":\"probe-invalid-input\"}\n";
        return 2;
    }
}
