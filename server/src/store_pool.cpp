#include "store_pool.h"
#include "store_session.h"
#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

namespace shin::server {
namespace {
constexpr auto kConnectedLeaseTimeout = std::chrono::seconds{3};
}

Store::Impl::Impl(StoreConfig config) : config_(std::move(config)) {
    validate_store_config(config_);
    slots_.reserve(config_.pool_size);
    for (std::size_t index = 0; index < config_.pool_size; ++index) {
        auto slot = std::make_unique<Slot>();
        slot->index = index;
        slots_.push_back(std::move(slot));
    }
    deadlines_ = std::make_unique<StoreDeadlines>(config_.pool_size);
}

Store::Impl::Lease Store::Impl::acquire() {
    Slot* slot = nullptr;
    {
        std::unique_lock lock{mutex_};
        const auto has_available_slot = [this] {
            return std::any_of(slots_.begin(), slots_.end(), [](const auto& candidate) { return !candidate->in_use; });
        };
        if (!available_.wait_for(lock, std::chrono::milliseconds{500}, has_available_slot))
            throw ServiceError{503, "store-busy", "The server is busy. Try again shortly."};
        const auto iterator = std::find_if(slots_.begin(), slots_.end(), [](const auto& candidate) { return !candidate->in_use; });
        slot = iterator->get();
        slot->in_use = true;
    }

    try {
        if (!slot->connection || !slot->connection->is_open()) make_runtime_connection(*slot);
        else deadlines_->arm(slot->index, slot->connection->sock(), kConnectedLeaseTimeout);
        verify_runtime_connection(*slot->connection, config_);
        return Lease{*this, *slot};
    } catch (const ServiceError&) {
        release(*slot, true);
        throw;
    } catch (const pqxx::failure&) {
        release(*slot, true);
        throw_database_unavailable();
    } catch (const std::system_error&) {
        release(*slot, true);
        throw_database_unavailable();
    } catch (...) {
        release(*slot, true);
        throw;
    }
}

void Store::Impl::make_runtime_connection(Slot& slot) {
    slot.connection = std::make_unique<pqxx::connection>(open_store_connection(config_));
    deadlines_->arm(slot.index, slot.connection->sock(), kConnectedLeaseTimeout);
    pqxx::read_transaction tx{*slot.connection};
    if (!tx.exec("SELECT pg_try_advisory_lock_shared(hashtext('shin_server.lifecycle.v1'))")[0][0].as<bool>())
        throw ServiceError(503, "server-recovery-busy", "Database recovery requires exclusive access.");
    tx.commit();
}

void Store::Impl::release(Slot& slot, bool discard) noexcept {
    const bool expired = deadlines_->disarm(slot.index);
    if (discard || expired || !slot.connection || !slot.connection->is_open()) slot.connection.reset();
    {
        std::lock_guard lock{mutex_};
        slot.in_use = false;
    }
    available_.notify_one();
}
} // namespace shin::server
