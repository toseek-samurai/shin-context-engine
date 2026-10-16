#include "shin/server/oidc_provider.h"
#include "shin/server/config.h"

#include <curl/curl.h>
#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <tuple>

namespace shin::server {
namespace {
using Steady = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
constexpr std::size_t body_limit = 65536, header_limit = 16384;

[[noreturn]] void unavailable(const char* code) {
    throw ServiceError(503, code, "OIDC provider metadata is unavailable.");
}
[[noreturn]] void invalid_options() {
    throw ServiceError(400, "oidc-provider-config-invalid", "OIDC provider configuration is invalid.");
}

void initialize_curl() {
    // Process lifetime: other native components may also use libcurl. Do not
    // globally tear it down while another service owns a transfer.
    static const bool initialized = [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) unavailable("oidc-transport-unavailable");
        const auto* version = curl_version_info(CURLVERSION_NOW);
        if (!version || !(version->features & CURL_VERSION_SSL) || !(version->features & CURL_VERSION_ASYNCHDNS))
            unavailable("oidc-transport-unsupported");
        return true;
    }();
    (void)initialized;
}

using Origin = std::tuple<std::string,std::string,std::string>;
Origin https_origin(const std::string& text, bool issuer) {
    if (text.empty() || text.size() > 2048 || !text.starts_with("https://") ||
        text.find_first_of("#\\") != std::string::npos || (issuer && text.find('?') != std::string::npos) ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) invalid_options();
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, text.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) invalid_options();
    const auto part = [&](CURLUPart name, unsigned flags = 0) {
        char* raw = nullptr;
        const auto result = curl_url_get(url.get(), name, &raw, flags);
        const std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
        if (result != CURLUE_OK || !raw) invalid_options();
        return std::string(raw);
    };
    const auto scheme = part(CURLUPART_SCHEME);
    auto host = part(CURLUPART_HOST);
    const auto port = part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
    if (scheme != "https" || host.empty() || port == "0" || host.find('%') != std::string::npos) invalid_options();
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return {scheme, host, port};
}

struct Transfer {
    std::string body;
    std::size_t header_bytes = 0;
    bool over_limit = false;
    std::stop_token stop;
};

std::size_t receive_body(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (size != 0 && count > body_limit / size) { transfer.over_limit = true; return 0; }
    const auto bytes = size * count;
    if (bytes > body_limit - transfer.body.size()) { transfer.over_limit = true; return 0; }
    try { transfer.body.append(data, bytes); return bytes; }
    catch (...) { return 0; }
}
std::size_t receive_header(char*, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (size != 0 && count > header_limit / size) { transfer.over_limit = true; return 0; }
    const auto bytes = size * count;
    if (bytes > header_limit - transfer.header_bytes) { transfer.over_limit = true; return 0; }
    transfer.header_bytes += bytes;
    return bytes;
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<Transfer*>(opaque)->stop.stop_requested() ? 1 : 0;
}

Json get_json(const std::string& url, const OidcProviderOptions& options,
              Steady::time_point deadline, std::stop_token stop) {
    if (stop.stop_requested()) unavailable("oidc-refresh-stopped");
    const auto remaining = std::chrono::duration_cast<Milliseconds>(deadline - Steady::now()).count();
    if (remaining <= 0) unavailable("oidc-refresh-timeout");
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) unavailable("oidc-transport-unavailable");
    Transfer transfer{{}, 0, false, stop};
    const auto option = [&](CURLoption name, auto value) {
        if (curl_easy_setopt(curl.get(), name, value) != CURLE_OK) unavailable("oidc-transport-unavailable");
    };
    option(CURLOPT_URL, url.c_str());
    option(CURLOPT_PROTOCOLS_STR, "https");
    option(CURLOPT_FOLLOWLOCATION, 0L);
    option(CURLOPT_PROXY, "");
    option(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
    option(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    option(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(remaining));
    option(CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
    option(CURLOPT_WRITEFUNCTION, &receive_body);
    option(CURLOPT_WRITEDATA, &transfer);
    option(CURLOPT_HEADERFUNCTION, &receive_header);
    option(CURLOPT_HEADERDATA, &transfer);
    option(CURLOPT_NOPROGRESS, 0L);
    option(CURLOPT_XFERINFOFUNCTION, &progress);
    option(CURLOPT_XFERINFODATA, &transfer);
    const auto ca = options.ca_file.string();
    if (!ca.empty()) option(CURLOPT_CAINFO, ca.c_str());
    const auto result = curl_easy_perform(curl.get());
    if (stop.stop_requested()) unavailable("oidc-refresh-stopped");
    if (transfer.over_limit) unavailable("oidc-response-too-large");
    if (result == CURLE_OPERATION_TIMEDOUT) unavailable("oidc-refresh-timeout");
    if (result != CURLE_OK) unavailable("oidc-transport-failed");
    long status = 0;
    if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200)
        unavailable("oidc-http-failed");
    try { return parse_bounded_json(transfer.body, body_limit); }
    catch (const ServiceError&) { unavailable("oidc-metadata-invalid"); }
}
}

struct OidcProvider::Impl {
    OidcProviderOptions options;
    Origin origin;
    std::shared_ptr<const OidcVerifier> snapshot;
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::uint64_t attempts = 0, successes = 0, failures = 0;
    std::string last_error;
    double last_refresh_ms = 0;
    std::jthread worker;

    explicit Impl(OidcProviderOptions input) : options(std::move(input)), origin(https_origin(options.trust.issuer, true)) {
        if (options.refresh_timeout < Milliseconds(50) || options.refresh_timeout > Milliseconds(2000) ||
            options.refresh_interval < Seconds(1) || options.refresh_interval > Seconds(120) ||
            options.key_lifetime <= options.refresh_interval || options.key_lifetime > Seconds(300) ||
            (!options.ca_file.empty() && (!options.ca_file.is_absolute() || !std::filesystem::is_regular_file(options.ca_file))))
            invalid_options();
        const auto printable = [](const std::string& value) {
            return !value.empty() && value.size() <= 255 &&
                std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; });
        };
        if (!printable(options.trust.audience) || !printable(options.trust.client_id) ||
            options.trust.audience == options.trust.client_id) invalid_options();
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~Impl() {
        worker.request_stop();
        changed.notify_all();
        worker.join();
    }

    std::shared_ptr<const OidcVerifier> refresh(std::stop_token stop) const {
        const auto deadline = Steady::now() + options.refresh_timeout;
        auto issuer = options.trust.issuer;
        if (issuer.ends_with('/')) issuer.pop_back();
        const auto metadata = get_json(issuer + "/.well-known/openid-configuration", options, deadline, stop);
        if (!metadata.is_object() || !metadata.contains("issuer") || metadata.at("issuer") != options.trust.issuer)
            unavailable("oidc-issuer-mismatch");
        std::string jwks_uri;
        try {
            jwks_uri = require_text(metadata, "jwks_uri", 2048);
            if (https_origin(jwks_uri, false) != origin) unavailable("oidc-jwks-origin-denied");
        } catch (const ServiceError& error) {
            if (error.code == "oidc-jwks-origin-denied") throw;
            unavailable("oidc-jwks-uri-invalid");
        }
        const auto jwks = get_json(jwks_uri, options, deadline, stop);
        auto next = std::make_shared<OidcVerifier>(options.trust, jwks.dump(), options.key_lifetime);
        if (Steady::now() >= deadline) unavailable("oidc-refresh-timeout");
        return next;
    }

    void run(std::stop_token stop) {
        // NOSIGNAL leaves SIGPIPE policy to the application. Confine this to our
        // network thread; do not change process-wide handlers owned by Server.
        sigset_t signals;
        if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGPIPE) != 0 ||
            pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
            {
                const std::lock_guard lock(mutex);
                attempts = failures = 1;
                last_error = "oidc-signal-setup-failed";
            }
            changed.notify_all();
            return;
        }
        Seconds retry(1);
        while (!stop.stop_requested()) {
            const auto start = Steady::now();
            std::string error;
            std::shared_ptr<const OidcVerifier> next;
            try { next = refresh(stop); }
            catch (const ServiceError& failure) { error = failure.code; }
            catch (...) { error = "oidc-refresh-failed"; }
            if (stop.stop_requested()) break;
            const auto delay = next ? options.refresh_interval : retry;
            {
                const std::lock_guard lock(mutex);
                ++attempts;
                if (next) { snapshot = std::move(next); ++successes; retry = Seconds(1); }
                else { ++failures; retry = std::min(retry * 2, Seconds(30)); }
                last_error = std::move(error);
                last_refresh_ms = std::chrono::duration<double,std::milli>(Steady::now() - start).count();
            }
            changed.notify_all();
            std::unique_lock lock(mutex);
            changed.wait_for(lock, stop, delay, [] { return false; });
        }
    }
};

OidcProvider::OidcProvider(OidcProviderOptions options) {
    initialize_curl();
    impl_ = std::make_unique<Impl>(std::move(options));
}
OidcProvider::~OidcProvider() = default;

VerifiedOidcIdentity OidcProvider::verify(std::string_view token) const {
    std::shared_ptr<const OidcVerifier> current;
    {
        const std::lock_guard lock(impl_->mutex);
        current = impl_->snapshot;
    }
    if (!current) unavailable("oidc-keys-unavailable");
    return current->verify(token);
}
Json OidcProvider::status() const {
    const std::lock_guard lock(impl_->mutex);
    const auto& current = impl_->snapshot;
    return {{"ready", current && current->current()}, {"attempts",impl_->attempts},
        {"successes",impl_->successes}, {"failures",impl_->failures}, {"last_error",impl_->last_error},
        {"last_refresh_ms",impl_->last_refresh_ms}, {"curl_version",curl_version_info(CURLVERSION_NOW)->version}};
}
bool OidcProvider::wait_ready(Milliseconds timeout) const {
    if (timeout < Milliseconds::zero() || timeout > Milliseconds(10000)) invalid_options();
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] {
        const auto& current = impl_->snapshot;
        return current && current->current();
    });
}

} // namespace shin::server
