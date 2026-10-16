#include "shin/server/oidc_provider.h"
#include "shin/server/config.h"
#include "oidc_test_key.h"
#include <algorithm>
#include <iostream>

namespace {
using namespace shin::server;
Json read_line() {
    std::string line;
    char c = 0;
    while (std::cin.get(c) && c != '\n') {
        if (line.size() >= 131072) throw std::runtime_error("probe input bound");
        line.push_back(c);
    }
    return parse_bounded_json(line, 131072);
}
void output(const Json& value) { std::cout << value.dump() << '\n'; std::cout.flush(); }
}

int main(int argc, char** argv) {
    try {
        const auto input = read_line();
        if (argc == 2 && std::string_view(argv[1]) == "fixtures") {
            const oidc_fixture::Key a("key-a"), b("key-b");
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const Json claims{{"iss",input.at("issuer")},{"aud","shin-server"},{"client_id","shin-ce"},
                {"sub","fixture-user"},{"jti","fixture-token"},{"iat",now},{"exp",now+300}};
            const auto token = [&](const oidc_fixture::Key& key) {
                return key.sign(Json{{"typ","at+jwt"},{"alg","RS256"},{"kid",key.jwk.at("kid")}}.dump(), claims.dump());
            };
            if (input.contains("sessions")) {
                Json tokens = Json::array();
                for (const auto& session : input.at("sessions")) {
                    auto body = claims;
                    body.update(session);
                    tokens.push_back(a.sign(Json{{"typ","at+jwt"},{"alg","RS256"},{"kid",a.jwk.at("kid")}}.dump(), body.dump()));
                }
                output({{"keys",Json{{"keys",Json::array({a.jwk})}}},{"tokens",tokens}});
                return 0;
            }
            output({{"a",Json{{"keys",Json::array({a.jwk})}}}, {"b",Json{{"keys",Json::array({b.jwk})}}},
                    {"both",Json{{"keys",Json::array({a.jwk,b.jwk})}}}, {"token_a",token(a)}, {"token_b",token(b)}});
            return 0;
        }
        if (argc != 1) return 2;
        OidcProviderOptions options;
        options.trust = {input.at("issuer"), "shin-server", "shin-ce"};
        options.ca_file = input.value("ca_file", "");
        options.refresh_timeout = std::chrono::milliseconds(input.value("timeout_ms", 500));
        options.refresh_interval = std::chrono::seconds(input.value("interval_seconds", 1));
        options.key_lifetime = std::chrono::seconds(input.value("lifetime_seconds", 3));
        auto provider = std::make_unique<OidcProvider>(options);
        output({{"started",true}});
        while (std::cin.peek() != EOF) {
            const auto command = read_line();
            if (command.at("command") == "quit") {
                const auto start = std::chrono::steady_clock::now();
                provider.reset();
                output({{"ok",true},{"shutdown_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()}});
                return 0;
            }
            try {
                if (command.at("command") == "status") output(provider->status());
                else if (command.at("command") == "wait") {
                    const auto ready = provider->wait_ready(std::chrono::milliseconds(command.value("timeout_ms", 2000)));
                    output({{"ready",ready},{"status",provider->status()}});
                } else if (command.at("command") == "verify") {
                    const auto token = command.at("token").get<std::string>();
                    const auto count = command.value("count", 1);
                    if (count < 1 || count > 500) return 2;
                    std::vector<double> times;
                    times.reserve(static_cast<std::size_t>(count));
                    for (int i = 0; i < count; ++i) {
                        const auto start = std::chrono::steady_clock::now();
                        if (provider->verify(token).subject.empty()) return 2;
                        times.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
                    }
                    std::sort(times.begin(),times.end());
                    output({{"ok",true},{"samples",count},{"p95_us",times[static_cast<std::size_t>((count-1)*95/100)]}});
                } else return 2;
            } catch (const ServiceError& e) { output({{"ok",false},{"code",e.code}}); }
        }
        return 0;
    } catch (const ServiceError& e) {
        try { output({{"ok",false},{"code",e.code}}); }
        catch (...) { std::cerr << "OIDC provider fixture failed\n"; }
        return 1;
    }
    catch (const std::exception&) { std::cerr << "OIDC provider fixture failed\n"; return 2; }
}
