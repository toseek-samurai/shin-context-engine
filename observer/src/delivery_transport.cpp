#include "shin/observer/delivery.h"
#include "shin/observer_protocol.h"
#include <httplib.h>
#include <algorithm>
#include <set>

namespace shin::observer {
namespace {
using Clock = std::chrono::steady_clock;
[[noreturn]] void protocol() { throw Error("protocol-invalid", "Server response was not a supported bounded envelope."); }
Json result_body(const httplib::Result& reply, int expected) {
    if (!reply) {
        if (reply.error() == httplib::Error::ExceedMaxPayloadSize) protocol();
        throw Error("transport-unavailable", "Delivery response is unavailable.");
    }
    if (reply->status == 401 || reply->status == 403) throw Error("authorization-denied", "Delivery is not authorized.");
    if (reply->status == 410) throw Error("history-retired", "Server history requires explicit reconciliation.");
    if (reply->status == 408 || reply->status == 429 || reply->status >= 500)
        throw Error("server-unavailable", "Server could not accept this attempt.");
    if (reply->status != expected && !(expected == 201 && reply->status == 200))
        throw Error("server-rejected", "Server rejected the delivery operation.");
    const auto type = reply->get_header_value("Content-Type");
    if ((type != "application/json" && !type.starts_with("application/json;")) || reply->body.size() > 16384) protocol();
    Json envelope;
    try { envelope = parse_json(reply->body); }
    catch (const Error&) { protocol(); }
    const std::set<std::string> keys{"schema_version", "tool", "version", "stability", "ok", "result"};
    if (!envelope.is_object() || envelope.size() != keys.size() ||
        !std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return envelope.contains(key); }) ||
        envelope.at("schema_version") != "shin.server-response.v1" || envelope.at("tool") != "shin-server" ||
        envelope.at("stability") != "development" || envelope.at("ok") != true || !envelope.at("version").is_string()) protocol();
    return envelope.at("result");
}

void deadline(httplib::Client& client, Clock::time_point end) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now());
    if (remaining.count() <= 0) throw Error("transport-unavailable", "Delivery deadline expired.");
    client.set_max_timeout(remaining);
    client.set_connection_timeout(remaining);
    client.set_read_timeout(remaining);
    client.set_write_timeout(remaining);
}

class Transport {
public:
    explicit Transport(const DeliveryConfig& config)
        : config_(config), client_("127.0.0.1", config.destination.at("port").get<int>()),
          end_(Clock::now() + std::chrono::milliseconds(config.timeout_ms)) {
        client_.set_hostname_addr_map({{"127.0.0.1", "127.0.0.1"}});
        client_.set_follow_location(false);
        client_.set_proxy("", -1);
        client_.set_keep_alive(false);
        client_.set_decompress(false);
        client_.set_payload_max_length(16384);
        client_.set_bearer_token_auth(config.token);
    }
    void handshake() {
        deadline(client_, end_);
        const auto identity = result_body(client_.Get("/api/v1/observer-identity", request_headers()), 200);
        const Json expected{{"schema_version", observer_protocol::kIdentitySchema},
            {"server_id", config_.destination.at("server_id")}, {"principal_id", config_.destination.at("principal_id")},
            {"submission_schema", "shin.observer-submission.v1"}, {"acknowledgement_schema", "shin.observer-ack.v1"},
            {"confirmation_schema", "shin.observer-confirmation.v1"}, {"reconciliation_schema", "shin.observer-reconcile-result.v1"},
            {"request_identity_header", observer_protocol::kRequestIdentityHeader},
            {"verification_level", "service-observed"}, {"source_verified", false}, {"shared_publication_mutated", false}};
        if (identity != expected) throw Error("identity-mismatch", "Configured Server identity or protocol did not match.");
    }
    Json post(const std::string& path, const Json& body, int status) {
        deadline(client_, end_);
        return result_body(client_.Post(path, request_headers(), body.dump(), "application/json"), status);
    }
private:
    httplib::Headers request_headers() const {
        return {{observer_protocol::kRequestIdentityHeader, config_.destination.at("server_id").get<std::string>()}};
    }

    const DeliveryConfig& config_;
    httplib::Client client_;
    Clock::time_point end_;
};
} // namespace

DeliveryResult send_observation(const DeliveryConfig& config, const Json& submission, const Json& last_receipt) {
    const auto started = Clock::now();
    DeliveryResult result;
    try {
        observer_protocol::verify_submission(submission);
        const auto path = "/api/v1/observer-streams/" + submission.at("stream_id").get<std::string>();
        if (last_receipt.is_null()) {
            if (submission.at("sequence") != 1) protocol();
        } else {
            observer_protocol::verify_acknowledgement_shape(last_receipt);
            if (last_receipt.at("sequence").get<std::int64_t>() + 1 != submission.at("sequence") ||
                last_receipt.at("server_id") != config.destination.at("server_id") ||
                last_receipt.at("stream_id") != submission.at("stream_id") ||
                last_receipt.at("project_id") != submission.at("payload").at("project_id") ||
                last_receipt.at("workspace_id") != submission.at("payload").at("workspace_id")) protocol();
        }
        Transport transport(config);
        transport.handshake();
        if (!last_receipt.is_null()) {
            const auto confirmed = transport.post(path + "/confirmations", {{"receipt", last_receipt}}, 200);
            observer_protocol::verify_confirmation(confirmed, last_receipt);
        }
        const auto response = transport.post(path + "/submissions", submission, 201);
        if (!response.is_object() || response.size() != 2 || !response.contains("receipt") ||
            !response.contains("replayed") || !response.at("replayed").is_boolean()) protocol();
        observer_protocol::verify_acknowledgement(response.at("receipt"), submission, config.destination.at("server_id").get<std::string>());
        result.receipt = response.at("receipt");
        result.code = "acknowledged";
    } catch (const Error& error) {
        result.code = error.code;
        result.retryable = error.code == "transport-unavailable" || error.code == "server-unavailable";
    } catch (const observer_protocol::ValidationError&) {
        result.code = "protocol-invalid";
    } catch (...) {
        result.code = "worker-failed";
    }
    result.duration_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return result;
}

Json reconcile_remote(const DeliveryConfig& config, const Json& request) {
    try {
        observer_protocol::verify_reconciliation_request(request);
        Transport transport(config);
        transport.handshake();
        auto result = transport.post("/api/v1/observer-streams/" + request.at("stream_id").get<std::string>() +
                                     "/reconciliation", request, 200);
        observer_protocol::verify_reconciliation_result(result, request);
        return result;
    } catch (const observer_protocol::ValidationError&) {
        throw Error("protocol-invalid", "Reconciliation response did not match its declared boundary.");
    }
}
} // namespace shin::observer
