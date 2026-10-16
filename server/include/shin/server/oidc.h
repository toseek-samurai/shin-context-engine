#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace shin::server {

struct OidcTrust {
    std::string issuer;
    std::string audience;
    std::string client_id;
};

// Authentication evidence only. Directory/device admission must resolve current
// authority before creating a Principal; neither email nor roles grant access.
struct VerifiedOidcIdentity {
    std::string issuer;
    std::string subject;
    std::string client_id;
    std::string token_id;
    std::chrono::system_clock::time_point expires_at;
    std::string session_id;
};

// Immutable, concurrently readable issuer-owned key snapshot. It never follows
// token URLs or performs network I/O. The caller must obtain JWKS over a trusted
// transport and replace expired snapshots; an unknown kid fails closed.
class OidcVerifier {
public:
    OidcVerifier(OidcTrust trust, std::string_view jwks,
                 std::chrono::seconds key_lifetime = std::chrono::seconds(300));
    ~OidcVerifier();
    bool current() const;
    OidcVerifier(const OidcVerifier&) = delete;
    OidcVerifier& operator=(const OidcVerifier&) = delete;
    VerifiedOidcIdentity verify(std::string_view token,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shin::server
