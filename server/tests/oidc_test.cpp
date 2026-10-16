#include "shin/server/oidc.h"
#include "shin/server/model.h"
#include "oidc_test_key.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using namespace shin::server;
using oidc_fixture::Key;
using Seconds = std::chrono::seconds;
const auto now = std::chrono::system_clock::time_point(Seconds(2000000000));
const OidcTrust trust{"https://identity.example.test/realm", "shin-server", "shin-ce"};
int checks = 0;
void require(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("OIDC assertion " + std::to_string(checks));
}
template<class F> void rejects(F operation, const std::string& code = "oidc-token-invalid") {
    try { operation(); } catch (const ServiceError& e) { require(e.code == code); return; }
    throw std::runtime_error("Unexpected OIDC admission at assertion " + std::to_string(checks + 1));
}
}

int main() {
    try {
        const Key key("key-1"), rotated("key-2"), weak("weak", 1024);
        const Json header{{"typ", "at+jwt"}, {"alg", "RS256"}, {"kid", "key-1"}};
        const Json claims{{"iss", trust.issuer}, {"aud", trust.audience}, {"client_id", trust.client_id},
            {"sub", "opaque-subject"}, {"jti", "token-1"}, {"iat", 2000000000}, {"exp", 2000000300}};
        const Json jwks{{"keys", Json::array({key.jwk})}};
        OidcVerifier verifier(trust, jwks.dump());
        const auto token = key.sign(header.dump(), claims.dump());
        const auto identity = verifier.verify(token, now);
        require(identity.subject == "opaque-subject" && identity.issuer == trust.issuer && identity.client_id == trust.client_id);
        require(identity.session_id.empty());
        auto session_claims = claims; session_claims["sid"] = "session-A";
        require(verifier.verify(key.sign(header.dump(), session_claims.dump()), now).session_id == "session-A");
        for (const auto& bad : {Json(""),Json(42),Json(nullptr),Json("bad\nsid"),Json(std::string(256,'x'))}) {
            session_claims["sid"] = bad;
            rejects([&] { verifier.verify(key.sign(header.dump(), session_claims.dump()), now); });
        }
        for (const auto* field : {"iss", "aud", "client_id", "sub", "jti", "iat", "exp"}) {
            auto changed = claims; changed.erase(field);
            rejects([&] { verifier.verify(key.sign(header.dump(), changed.dump()), now); });
        }
        for (const auto* field : {"iss", "aud", "client_id", "sub", "jti"}) {
            auto changed = claims; changed[field] = "";
            rejects([&] { verifier.verify(key.sign(header.dump(), changed.dump()), now); });
            changed[field] = Json::object();
            rejects([&] { verifier.verify(key.sign(header.dump(), changed.dump()), now); });
        }
        for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
                {"iss", "https://attacker.example.test"}, {"aud", "other-server"}, {"client_id", "other-client"},
                {"iat", 2000000031}, {"iat", -1}, {"exp", 1999999969}, {"exp", 2000000301},
                {"exp", "2000000300"}, {"exp", 2000000300.0}, {"exp", UINT64_MAX},
                {"nbf", 2000000031}, {"nbf", "invalid"}, {"nbf", 4102444801LL},
                {"aud", Json::array({trust.audience, 42})}, {"aud", Json::array()}, {"sub", "bad\nsubject"}}) {
            auto changed = claims; changed[field] = value;
            rejects([&] { verifier.verify(key.sign(header.dump(), changed.dump()), now); });
        }
        for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
                {"typ", "JWT"}, {"typ", "logout+jwt"}, {"alg", "none"}, {"alg", "HS256"},
                {"kid", "unknown"}, {"kid", Json::array()}, {"crit", Json::array()}, {"b64", false},
                {"jku", "https://attacker.example.test/jwks"}, {"jwk", key.jwk}, {"x5u", "file:///key"},
                {"x5c", Json::array()}}) {
            auto changed = header; changed[field] = value;
            rejects([&] { verifier.verify(key.sign(changed.dump(), claims.dump()), now); });
        }
        rejects([&] { verifier.verify(rotated.sign(header.dump(), claims.dump()), now); });
        auto duplicate_claims = claims.dump(); duplicate_claims.insert(1, "\"sub\":\"attacker\",");
        auto duplicate_header = header.dump(); duplicate_header.insert(1, "\"alg\":\"none\",");
        rejects([&] { verifier.verify(key.sign(header.dump(), duplicate_claims), now); });
        rejects([&] { verifier.verify(key.sign(duplicate_header, claims.dump()), now); });
        auto nested = claims;
        Json depth = 0;
        for (int i = 0; i < 20; ++i) depth = Json::array({depth});
        nested["extra"] = depth;
        rejects([&] { verifier.verify(key.sign(header.dump(), nested.dump()), now); });
        rejects([&] { verifier.verify(std::string(8193, 'a'), now); });
        rejects([&] { verifier.verify(token + ".extra", now); });
        rejects([&] { verifier.verify(token + "=", now); });
        rejects([&] { verifier.verify(token.substr(0, token.rfind('.') + 1), now); });
        rejects([&] { verifier.verify(token, now + Seconds(330)); });
        require(verifier.verify(token, now + Seconds(329)).subject == identity.subject);
        auto plural = claims; plural["aud"] = Json::array({"other", trust.audience});
        require(verifier.verify(key.sign(header.dump(), plural.dump()), now).subject == identity.subject);
        auto media = header; media["typ"] = "application/at+jwt";
        require(verifier.verify(key.sign(media.dump(), claims.dump()), now).subject == identity.subject);
        auto ignored = claims; ignored["email"] = "admin@example.test"; ignored["roles"] = {"admin"};
        require(verifier.verify(key.sign(header.dump(), ignored.dump()), now).subject == identity.subject);
        auto duplicate_keys = jwks.dump(); duplicate_keys.insert(1, "\"keys\":[],");
        rejects([&] { OidcVerifier bad(trust, duplicate_keys); }, "oidc-keys-invalid");
        for (const auto& keys : {Json::array(), Json::array({key.jwk, key.jwk}), Json::array({weak.jwk})})
            rejects([&] { OidcVerifier bad(trust, Json{{"keys", keys}}.dump()); }, "oidc-keys-invalid");
        for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
                {"d", "secret"}, {"k", "secret"}, {"key_ops", Json::array({"sign"})},
                {"n", "AA"}, {"e", "bad="}, {"kid", ""}}) {
            auto changed = key.jwk; changed[field] = value;
            rejects([&] { OidcVerifier bad(trust, Json{{"keys", Json::array({changed})}}.dump()); }, "oidc-keys-invalid");
        }
        OidcVerifier expired(trust, jwks.dump(), Seconds::zero());
        rejects([&] { expired.verify(token, now); }, "oidc-keys-expired");
        rejects([&] { OidcVerifier bad(trust, jwks.dump(), Seconds(301)); }, "oidc-trust-invalid");
        for (const auto& issuer : {"http://identity.example.test", "https://a@b", "https://host/?x=1", "https:///"}) {
            auto bad = trust; bad.issuer = issuer;
            rejects([&] { OidcVerifier refused(bad, jwks.dump()); }, "oidc-trust-invalid");
        }
        auto local = trust; local.issuer = "http://127.0.0.1:12345/realms/test";
        rejects([&] { OidcVerifier bad(local, jwks.dump()); }, "oidc-trust-invalid");
        auto rotated_header = header; rotated_header["kid"] = "key-2";
        const auto new_token = rotated.sign(rotated_header.dump(), claims.dump());
        rejects([&] { verifier.verify(new_token, now); });
        OidcVerifier overlap(trust, Json{{"keys", Json::array({key.jwk, rotated.jwk})}}.dump());
        require(overlap.verify(token, now).subject == overlap.verify(new_token, now).subject);
        OidcVerifier replacement(trust, Json{{"keys", Json::array({rotated.jwk})}}.dump());
        rejects([&] { replacement.verify(token, now); });
        std::atomic<int> verified{0};
        {
            std::vector<std::jthread> workers;
            workers.reserve(4);
            for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
                for (int n = 0; n < 100; ++n) {
                    if (verifier.verify(token, now).subject == identity.subject) ++verified;
                }
            });
        }
        require(verified == 400);
        std::cout << "OIDC verifier: " << checks << " assertions; 400 concurrent verifications passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
