#pragma once

#include "observer.h"
#include <chrono>
#include <future>

namespace shin::observer {
struct DeliveryConfig {
    Json destination;
    std::string token;
    std::uint64_t timeout_ms = 2000;
};

struct DeliveryResult {
    Json receipt;
    std::string code;
    bool retryable = false;
    double duration_ms = 0;
};

DeliveryConfig load_delivery_config(const Config& observer, const fs::path& path);
DeliveryResult send_observation(const DeliveryConfig& config, const Json& submission, const Json& last_receipt);
Json reconcile_remote(const DeliveryConfig& config, const Json& request);
Json reconcile_outbox(const Config& config, const DeliveryConfig& delivery);
void bind_delivery(Json& outbox, const DeliveryConfig& config);

// Only the state-owning thread calls tick/finish; the worker owns a copied packet.
class DeliverySession {
public:
    DeliverySession(const Config& config, DeliveryConfig delivery);
    bool tick(Json& state, StateStore& store, bool start = true);
    void finish(Json& state, StateStore& store);
    bool failed() const { return failed_; }
    std::uint64_t completed() const { return completed_; }
private:
    void complete(Json& state, StateStore& store);
    const Config& config_;
    DeliveryConfig delivery_;
    std::future<DeliveryResult> pending_;
    std::chrono::steady_clock::time_point retry_after_{};
    std::uint64_t failures_ = 0, completed_ = 0;
    bool failed_ = false, blocked_ = false;
};

Json deliver_outbox(const Config& config, const DeliveryConfig& delivery, std::uint64_t maximum,
                    const std::function<bool()>& stopped);
} // namespace shin::observer
