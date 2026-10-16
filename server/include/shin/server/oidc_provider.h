#pragma once

#include "oidc.h"
#include "model.h"
#include <filesystem>

namespace shin::server {

struct OidcProviderOptions {
    OidcTrust trust;
    std::filesystem::path ca_file; // Empty: platform/libcurl trust. Never skip TLS verification.
    std::chrono::milliseconds refresh_timeout{2000};
    std::chrono::seconds refresh_interval{60};
    std::chrono::seconds key_lifetime{300};
};

// One provider per configured issuer, not per user/request. Only the owned
// background worker performs network I/O; verify never refreshes keys inline.
class OidcProvider {
public:
    explicit OidcProvider(OidcProviderOptions options);
    ~OidcProvider();
    OidcProvider(const OidcProvider&) = delete;
    OidcProvider& operator=(const OidcProvider&) = delete;
    VerifiedOidcIdentity verify(std::string_view token) const;
    Json status() const;
    bool wait_ready(std::chrono::milliseconds timeout) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shin::server
