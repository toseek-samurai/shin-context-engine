#include "shin/observer/delivery.h"
#include "shin/observer/outbox.h"
#include <algorithm>
#include <limits>

namespace shin::observer {
namespace {
void increment(Json& value) {
    const auto counter = value.get<std::uint64_t>();
    if (counter >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1))
        throw Error("sequence-exhausted", "Delivery counter capacity was reached.");
    value = counter + 1;
}
} // namespace

DeliverySession::DeliverySession(const Config& config, DeliveryConfig delivery)
    : config_(config), delivery_(std::move(delivery)) {}

void DeliverySession::complete(Json& state, StateStore& store) {
    const auto result = pending_.get();
    Json next = state;
    auto& metrics = next.at("outbox").at("delivery");
    metrics["last_result_at_ms"] = now_ms();
    metrics["last_duration_ms"] = result.duration_ms;
    metrics["last_outcome"] = result.code;
    failed_ = result.receipt.is_null();
    if (!failed_) {
        acknowledge_outbox(config_, next, result.receipt);
        failures_ = 0;
    } else {
        increment(metrics.at("failures"));
        blocked_ = !result.retryable;
        failures_ = std::min<std::uint64_t>(failures_ + 1, 8);
        const auto delay = std::min<std::uint64_t>(30000, 250ULL << (failures_ - 1));
        retry_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    }
    verify_outbox(config_, next);
    store.save(next);
    state = std::move(next);
    ++completed_;
}

bool DeliverySession::tick(Json& state, StateStore& store, bool start) {
    if (pending_.valid()) {
        if (pending_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return true;
        complete(state, store);
    }
    if (!start || blocked_ || state.at("outbox").at("entries").empty() || std::chrono::steady_clock::now() < retry_after_) return false;
    Json next = state;
    bind_delivery(next.at("outbox"), delivery_);
    auto& metrics = next.at("outbox").at("delivery");
    increment(metrics.at("attempts"));
    metrics["last_attempt_at_ms"] = now_ms();
    metrics["last_outcome"] = "in-flight";
    verify_outbox(config_, next);
    store.save(next);
    state = std::move(next);
    const auto submission = state.at("outbox").at("entries").front();
    // std::async retains one bounded request; destruction joins before the writer fence is released.
    pending_ = std::async(std::launch::async, send_observation, delivery_, submission, state.at("outbox").at("last_receipt"));
    return true;
}

void DeliverySession::finish(Json& state, StateStore& store) {
    if (pending_.valid()) complete(state, store);
}
} // namespace shin::observer
