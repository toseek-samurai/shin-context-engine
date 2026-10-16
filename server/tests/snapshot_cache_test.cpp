#include "shin/server/snapshot_cache.h"
#include "shin/server/route_files.h"
#include "context_internal.h"
#include "generated_io.h"

#include <atomic>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {
using namespace shin::server;
namespace core = shin::context;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(const char* code, Function&& action) {
    try { action(); }
    catch (const ServiceError& error) {
        check(error.code == code, "wrong cache error code");
        return;
    }
    throw std::runtime_error("expected cache rejection");
}

class Fixture {
public:
    fs::path path;
    core::RuntimeOptions options;
    std::string first_id;
    std::uintmax_t bytes;

    Fixture() {
        auto name = (fs::temp_directory_path() / "shin-server-cache-XXXXXX").string();
        if (!mkdtemp(name.data())) throw std::runtime_error("temporary directory failed");
        path = fs::canonical(name);
        options.root = path / "project";
        options.out_root = path / "output";
        options.profile.id = "neutral";
        options.profile.source_label = "neutral fixture";
        options.profile.config_hash = "neutral-cache-v1";
        options.profile.root_files = {"README.md"};
        fs::create_directories(options.root);
        publish("FirstRevision");
        const auto snapshot = load();
        first_id = snapshot->id();
        bytes = snapshot->payload_bytes();
    }
    ~Fixture() { std::error_code error; fs::remove_all(path, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    void publish(const char* revision) {
        { std::ofstream out(options.root / "README.md"); out << "# Cache\n\n" << revision << " immutable snapshot.\n"; }
        const auto generated = core::generate_outputs(options.profile, options.root);
        shin::generated_io::IndexLock lock(options.out_root, shin::generated_io::LockMode::exclusive,
                                           shin::generated_io::LockTarget::writer);
        shin::generated_io::publish_snapshot(options.out_root, core::kGeneratedDir, generated.files);
    }
    SnapshotCache::Snapshot load() const { return core::GeneratedSnapshot::load(options); }
    RetrievalConfig limits() const {
        RetrievalConfig result;
        result.max_generation_bytes = bytes * 2;
        result.max_file_bytes = bytes * 2;
        result.max_payload_bytes = bytes * 4;
        result.max_generations = 2;
        return result;
    }
};

void retention_and_rollback(Fixture& fixture) {
    SnapshotCache cache(fixture.limits());
    auto first = cache.acquire("one", fixture.first_id, [&] { return fixture.load(); });
    auto same = cache.acquire("one", fixture.first_id, []() -> SnapshotCache::Snapshot {
        throw std::runtime_error("cache hit called loader");
    });
    check(first == same && cache.statistics()["hits"] == 1, "identical generation not reused");
    same.reset();
    auto second = cache.acquire("two", fixture.first_id, [&] { return fixture.load(); });
    check(first != second && cache.statistics()["resident_generations"] == 2, "routes shared an owner");
    rejects("snapshot-cache-busy", [&] { cache.acquire("three", fixture.first_id, [&] { return fixture.load(); }); });
    check(cache.statistics()["resident_payload_bytes"] == first->payload_bytes() + second->payload_bytes(),
          "active generations escaped accounting");
    first.reset();
    auto third = cache.acquire("three", fixture.first_id, [&] { return fixture.load(); });
    check(cache.statistics()["evictions"] == 1 && cache.statistics()["resident_generations"] == 2,
          "idle LRU was not evicted");
    third.reset();
    rejects("injected-failure", [&] {
        cache.acquire("failure", fixture.first_id, []() -> SnapshotCache::Snapshot {
            throw ServiceError(503, "injected-failure", "fixture");
        });
    });
    check(!cache.statistics()["load_in_progress"].get<bool>() && cache.statistics()["reserved_payload_bytes"] == 0,
          "failed load leaked reservation");
    rejects("snapshot-generation-changed", [&] {
        cache.acquire("changed", "not-the-generation", [&] { return fixture.load(); });
    });
    auto restored = cache.acquire("restored", fixture.first_id, [&] { return fixture.load(); });
    check(restored && second, "rollback poisoned subsequent load");
}

void byte_budget(Fixture& fixture) {
    auto limits = fixture.limits();
    limits.max_payload_bytes = limits.max_generation_bytes;
    SnapshotCache cache(limits);
    auto active = cache.acquire("one", fixture.first_id, [&] { return fixture.load(); });
    rejects("snapshot-cache-busy", [&] { cache.acquire("two", fixture.first_id, [&] { return fixture.load(); }); });
    active.reset();
    check(cache.acquire("two", fixture.first_id, [&] { return fixture.load(); }) != nullptr, "idle payload not reclaimed");
    limits.max_generation_bytes = 1;
    SnapshotCache tiny(limits);
    rejects("snapshot-memory-budget-exceeded", [&] { tiny.acquire("one", fixture.first_id, [&] { return fixture.load(); }); });
    check(tiny.statistics()["reserved_payload_bytes"] == 0, "oversize load reservation leaked");
}

void concurrent_coalescing(Fixture& fixture) {
    SnapshotCache cache(fixture.limits());
    std::atomic<int> calls{0};
    std::promise<void> started;
    std::promise<void> release;
    const auto gate = release.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        return cache.acquire("one", fixture.first_id, [&] {
            ++calls;
            started.set_value();
            gate.wait();
            return fixture.load();
        });
    });
    started.get_future().wait();
    const auto during = cache.statistics();
    std::vector<std::future<SnapshotCache::Snapshot>> clients;
    clients.reserve(12);
    for (int index = 0; index < 12; ++index) {
        clients.push_back(std::async(std::launch::async, [&] {
            return cache.acquire("one", fixture.first_id, [&] { ++calls; return fixture.load(); });
        }));
    }
    release.set_value();
    auto result = first.get();
    check(during["load_in_progress"] == true && during["reserved_payload_bytes"] == fixture.limits().max_generation_bytes,
          "cold load was not reserved");
    for (auto& client : clients) check(client.get() == result, "coalesced request got a different owner");
    check(calls == 1 && cache.statistics()["loads"] == 1 && cache.statistics()["hits"] == 12,
          "concurrent miss loaded duplicate generations");
}

void generation_retirement(Fixture& fixture) {
    SnapshotCache cache(fixture.limits());
    auto old = cache.acquire("one", fixture.first_id, [&] { return fixture.load(); });
    fixture.publish("SecondRevision");
    std::string next_id;
    { const auto next = fixture.load(); next_id = next->id(); }
    check(next_id != fixture.first_id, "fixture did not change publication");
    auto next = cache.acquire("one", next_id, [&] { return fixture.load(); });
    check(cache.statistics()["resident_generations"] == 2, "retired active generation disappeared from accounting");
    check(old->file(fixture.options.out_root / core::kGeneratedDir / "manifest.json") !=
          next->file(fixture.options.out_root / core::kGeneratedDir / "manifest.json"), "generation bytes mixed");
    rejects("snapshot-cache-busy", [&] { cache.acquire("two", next_id, [&] { return fixture.load(); }); });
    old.reset();
    auto other = cache.acquire("two", next_id, [&] { return fixture.load(); });
    check(other && cache.statistics()["evictions"] == 1, "retired owner not reclaimed after release");
}

void busy_load_does_not_block_hits(Fixture& fixture) {
    SnapshotCache cache(fixture.limits());
    auto warm = cache.acquire("warm", fixture.first_id, [&] { return fixture.load(); });
    std::promise<void> started;
    std::promise<void> release;
    const auto gate = release.get_future().share();
    auto cold = std::async(std::launch::async, [&] {
        return cache.acquire("cold", fixture.first_id, [&] {
            started.set_value();
            gate.wait();
            return fixture.load();
        });
    });
    started.get_future().wait();
    auto hit = std::async(std::launch::async, [&] {
        return cache.acquire("warm", fixture.first_id, [&] { return fixture.load(); });
    });
    const bool hit_ready = hit.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    std::exception_ptr failure;
    try {
        rejects("snapshot-load-busy", [&] {
            cache.acquire("another-cold", fixture.first_id, [&] { return fixture.load(); });
        });
    } catch (...) { failure = std::current_exception(); }
    release.set_value();
    const auto loaded = cold.get();
    const auto cached = hit.get();
    if (failure) std::rethrow_exception(failure);
    check(hit_ready && cached == warm && loaded, "unrelated cold load blocked warm retrieval");
}

void route_file_guards(Fixture& fixture) {
    const auto path = fixture.options.root / "README.md";
    check(!read_route_file(path, 4096).empty(), "bounded file rejected");
    rejects("route-input-unavailable", [&] { read_route_file(path, 1); });
    const auto alias = fixture.path / "alias";
    fs::create_symlink(path, alias);
    rejects("route-input-unavailable", [&] { read_route_file(alias, 4096); });
    rejects("route-input-unavailable", [&] { read_route_file(fixture.path, 4096); });
    check(!route_directory_identity(fixture.path).empty(), "directory identity missing");
}

void repeated_publication_churn(Fixture& fixture) {
    std::weak_ptr<const core::GeneratedSnapshot> first;
    {
        const auto limits = fixture.limits();
        SnapshotCache cache(limits);
        for (int generation = 0; generation < 128; ++generation) {
            fixture.publish(("ChurnRevision" + std::to_string(generation)).c_str());
            std::string id;
            { const auto view = fixture.load(); id = view->id(); }
            const auto view = cache.acquire("one", id, [&] { return fixture.load(); });
            if (generation == 0) first = view;
            const auto stats = cache.statistics();
            check(stats["resident_generations"] <= limits.max_generations &&
                  stats["resident_payload_bytes"] <= limits.max_payload_bytes &&
                  stats["reserved_payload_bytes"] == 0, "churn escaped aggregate bounds");
            if (generation >= 2) check(first.expired(), "eviction retained a hidden snapshot owner");
        }
        check(cache.statistics()["loads"] == 128 && cache.statistics()["evictions"] == 126,
              "churn did not exercise every generation");
    }
    check(first.expired(), "cache teardown retained an owner");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        retention_and_rollback(fixture);
        byte_budget(fixture);
        concurrent_coalescing(fixture);
        busy_load_does_not_block_hits(fixture);
        route_file_guards(fixture);
        generation_retirement(fixture);
        repeated_publication_churn(fixture);
        std::cout << "resident cache bounds, lifetime, rollback, concurrency and route guards passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
