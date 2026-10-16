#include "shin/server/oidc.h"
#include "shin/server/config.h"

#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <algorithm>
#include <map>
#include <set>

namespace shin::server {
namespace {
using Seconds = std::chrono::seconds;
constexpr std::size_t token_limit = 8192;
constexpr std::size_t jwks_limit = 65536;
constexpr std::int64_t maximum_date = 4102444800; // 2100; avoid duration overflow.

[[noreturn]] void unauthorized() {
    throw ServiceError(401, "oidc-token-invalid", "A valid access token is required.");
}
[[noreturn]] void invalid_keys() {
    throw ServiceError(503, "oidc-keys-invalid", "Issuer signing keys are unavailable.");
}

struct BoundedTraits : jwt::traits::nlohmann_json {
    static bool parse(Json& value, const std::string& text) {
        value = parse_bounded_json(text, token_limit);
        return true;
    }
};
struct Clock {
    std::chrono::system_clock::time_point value;
    auto now() const { return value; }
};

bool safe_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c >= 127; });
}

std::int64_t timestamp(const Json& claims, const char* name) {
    const auto& item = claims.at(name);
    if (!item.is_number_integer() || item < 0 || item > maximum_date) unauthorized();
    return item.get<std::int64_t>();
}

std::string base64url(std::string_view value) {
    if (value.empty() || value.size() % 4 == 1 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) unauthorized();
    const auto bytes = jwt::base::decode<jwt::alphabet::base64url>(
        jwt::base::pad<jwt::alphabet::base64url>(std::string(value)));
    if (jwt::base::trim<jwt::alphabet::base64url>(
            jwt::base::encode<jwt::alphabet::base64url>(bytes)) != value) unauthorized();
    return bytes;
}

void trust_policy(const OidcTrust& trust) {
    const auto& issuer = trust.issuer;
    const bool secure = issuer.starts_with("https://") && issuer.size() > 8 && issuer[8] != '/';
    if (!secure || !safe_text(issuer, 2048) ||
        issuer.find_first_of("?#@\\") != std::string::npos ||
        !safe_text(trust.audience, 255) || !safe_text(trust.client_id, 255) || trust.audience == trust.client_id)
        throw ServiceError(400, "oidc-trust-invalid", "Explicit issuer, resource audience and distinct client are required.");
}
}

struct OidcVerifier::Impl {
    OidcTrust trust;
    std::chrono::steady_clock::time_point expires;
    std::map<std::string, jwt::algorithm::rs256> keys;
};

OidcVerifier::OidcVerifier(OidcTrust trust, std::string_view jwks, Seconds key_lifetime)
    : impl_(std::make_unique<Impl>()) {
    trust_policy(trust);
    if (key_lifetime < Seconds::zero() || key_lifetime > Seconds(300))
        throw ServiceError(400, "oidc-trust-invalid", "Signing key lifetime is outside the supported bound.");
    impl_->trust = std::move(trust);
    impl_->expires = std::chrono::steady_clock::now() + key_lifetime;
    try {
        const auto document = parse_bounded_json(jwks, jwks_limit);
        const auto& keys = document.at("keys");
        if (!keys.is_array() || keys.empty() || keys.size() > 16) invalid_keys();
        std::set<std::string> seen;
        for (const auto& key : keys) {
            const auto kid = require_text(key, "kid", 128);
            if (!safe_text(kid, 128) || !seen.insert(kid).second) invalid_keys();
            for (const auto* name : {"d", "p", "q", "dp", "dq", "qi", "oth", "k"})
                if (key.contains(name)) invalid_keys();
            if (key.at("kty") != "RSA" || (key.contains("use") && key.at("use") != "sig") ||
                (key.contains("alg") && key.at("alg") != "RS256")) continue;
            if (key.contains("key_ops") && key.at("key_ops") != Json::array({"verify"})) invalid_keys();
            const auto n = require_text(key, "n", 1366), e = require_text(key, "e", 12);
            const auto modulus = base64url(n), exponent = base64url(e);
            if (modulus.size() < 256 || modulus.size() > 1024 || modulus.front() == 0 ||
                (modulus.size() == 256 && static_cast<unsigned char>(modulus.front()) < 128) ||
                exponent.empty() || exponent.size() > 8 || exponent.front() == 0) invalid_keys();
            impl_->keys.emplace(kid, jwt::algorithm::rs256(jwt::helper::create_public_key_from_rsa_components(n, e)));
        }
        if (impl_->keys.empty()) invalid_keys();
    } catch (const std::exception&) { invalid_keys(); }
}

OidcVerifier::~OidcVerifier() = default;

bool OidcVerifier::current() const { return std::chrono::steady_clock::now() < impl_->expires; }

VerifiedOidcIdentity OidcVerifier::verify(std::string_view token, std::chrono::system_clock::time_point now) const {
    if (std::chrono::steady_clock::now() >= impl_->expires)
        throw ServiceError(503, "oidc-keys-expired", "Issuer signing keys require refresh.");
    try {
        if (token.empty() || token.size() > token_limit || std::count(token.begin(), token.end(), '.') != 2) unauthorized();
        const auto first = token.find('.'), second = token.find('.', first + 1);
        // Bound and reject ambiguous JSON before the JWT library parses claims.
        const auto header = parse_bounded_json(base64url(token.substr(0, first)), token_limit);
        const auto claims = parse_bounded_json(base64url(token.substr(first + 1, second - first - 1)), token_limit);
        (void)base64url(token.substr(second + 1));
        if (!header.is_object() || !claims.is_object() || header.at("alg") != "RS256" ||
            (header.at("typ") != "at+jwt" && header.at("typ") != "application/at+jwt")) unauthorized();
        for (const auto* name : {"crit", "b64", "jku", "jwk", "x5u", "x5c"})
            if (header.contains(name)) unauthorized();
        const auto kid = require_text(header, "kid", 128);
        const auto key = impl_->keys.find(kid);
        if (key == impl_->keys.end()) unauthorized();
        const auto issued = timestamp(claims, "iat"), expires = timestamp(claims, "exp");
        const auto seconds = std::chrono::duration_cast<Seconds>(now.time_since_epoch()).count();
        if (seconds < 0 || seconds > maximum_date || expires <= issued || expires - issued > 300 ||
            issued > seconds + 30 || expires <= seconds - 30) unauthorized();
        if (claims.contains("nbf") && (timestamp(claims, "nbf") > seconds + 30 || timestamp(claims, "nbf") >= expires))
            unauthorized();
        const auto subject = require_text(claims, "sub", 255), jti = require_text(claims, "jti", 255);
        if (!safe_text(subject, 255) || !safe_text(jti, 255) || claims.at("client_id") != impl_->trust.client_id)
            unauthorized();
        std::string session;
        if (claims.contains("sid")) {
            session = require_text(claims, "sid", 255);
            if (!safe_text(session, 255)) unauthorized();
        }
        const auto& audience = claims.at("aud");
        if (!audience.is_string()) {
            if (!audience.is_array() || audience.empty() || audience.size() > 16) unauthorized();
            for (const auto& value : audience)
                if (!value.is_string() || !safe_text(value.get<std::string>(), 255)) unauthorized();
        }
        const auto decoded = jwt::decode<BoundedTraits>(std::string(token));
        jwt::verify<Clock, BoundedTraits>(Clock{now})
            .allow_algorithm(key->second).with_issuer(impl_->trust.issuer)
            .with_audience(impl_->trust.audience).leeway(30).verify(decoded);
        return {impl_->trust.issuer, subject, impl_->trust.client_id, jti,
            std::chrono::system_clock::time_point(Seconds(expires)), session};
    } catch (const std::exception&) { unauthorized(); }
}

} // namespace shin::server
