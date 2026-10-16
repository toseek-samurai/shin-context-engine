#pragma once

#include "shin/server/store.h"
#include "store_deadlines.h"
#include <pqxx/pqxx>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace shin::server {
class Store::Impl {
public:
    explicit Impl(StoreConfig config);

    template <typename Function>
    auto execute(Function&& function) -> std::invoke_result_t<Function, pqxx::connection&> {
        auto lease = acquire();
        try {
            return function(lease.connection());
        } catch (const pqxx::failure& error) {
            if (error.poisons_connection() || !lease.connection().is_open()) lease.discard();
            throw;
        }
    }

private:
    struct Slot {
        std::unique_ptr<pqxx::connection> connection;
        std::size_t index = 0;
        bool in_use = false;
    };

    class Lease {
    public:
        Lease(Impl& owner, Slot& slot) : owner_(owner), slot_(slot) {}
        ~Lease() { owner_.release(slot_, discard_); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;
        pqxx::connection& connection() { return *slot_.connection; }
        void discard() noexcept { discard_ = true; }

    private:
        Impl& owner_;
        Slot& slot_;
        bool discard_ = false;
    };

    Lease acquire();
    void make_runtime_connection(Slot& slot);
    void release(Slot& slot, bool discard) noexcept;

    StoreConfig config_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::unique_ptr<StoreDeadlines> deadlines_;
};
} // namespace shin::server
