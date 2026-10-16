#include "shin/observer/observer.h"
#include "shin/observer/delivery.h"
#include <charconv>
#include <atomic>
#include <csignal>
#include <iostream>
#include <cstdio>

namespace {
std::atomic_flag interrupted = ATOMIC_FLAG_INIT;
void stop(int) { interrupted.test_and_set(std::memory_order_relaxed); }

int failure() noexcept {
    std::fputs("{\"schema_version\":\"shin.observer-response.v1\",\"tool\":\"shin-observer\","
        "\"version\":\"" SHIN_OBSERVER_VERSION "\",\"core_version\":\"" SHIN_VERSION "\","
        "\"stability\":\"" SHIN_OBSERVER_STABILITY "\",\"ok\":false,\"error\":{\"code\":\"observer-failed\","
        "\"message\":\"Observer operation failed.\"}}\n", stderr);
    return 1;
}

int run(int argc, char** argv) {
    using namespace shin::observer;
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << response({{"observer_available", true}, {"watch_policy", "periodic-content-rescan"},
            {"server_required", false}, {"postgresql_required", false}, {"git_required", false},
            {"automatic_publication", false}, {"durable_local_outbox", true}, {"metrics_available", true},
            {"synchronization_available", true}, {"delivery_scope", "explicit-loopback-metadata"},
            {"source_query_available", SHIN_OBSERVER_SOURCE_DISCOVERY != 0},
            {"source_context_available", SHIN_OBSERVER_SOURCE_DISCOVERY != 0},
            {"source_followup_available", SHIN_OBSERVER_SOURCE_DISCOVERY != 0},
            {"compact_source_view_available", SHIN_OBSERVER_SOURCE_DISCOVERY != 0},
            {"compact_source_inventory_available", SHIN_OBSERVER_SOURCE_DISCOVERY != 0}}).dump() << '\n';
        return 0;
    }
    if (argc < 4 || argc > 12 || argc % 2 != 0) throw Error("invalid-arguments", "Use an Observer command --config FILE with explicit options.");
    const std::string_view command(argv[1]);
    const bool source_command = command == "source-query" || command == "source-context";
    const bool source_inspect = command == "source-inspect";
    const bool source_read = command == "source-read";
    const bool archive_command = command == "outbox-export" || command == "outbox-verify";
    if ((!archive_command && !source_command && !source_inspect && !source_read && command != "sample" && command != "watch" && command != "status" && command != "metrics" && command != "deliver" && command != "reconcile") || std::string_view(argv[2]) != "--config")
        throw Error("invalid-arguments", "Unsupported Observer command.");
    std::uint64_t samples = command == "watch" ? 0 : 1;
    std::uint64_t maximum = 128;
    bool count_seen = false;
    std::optional<fs::path> delivery_path;
    std::optional<fs::path> archive_path;
    std::optional<std::string> query;
    std::optional<std::string> source_path, snapshot;
    std::optional<fs::path> input_path;
    std::optional<std::string> projection;
    std::optional<std::size_t> view_bytes;
    for (int i = 4; i < argc; i += 2) {
        const std::string_view option(argv[i]), value(argv[i + 1]);
        if ((command == "source-context" || source_read || source_inspect) && option == "--projection" && !projection &&
            (value == "compact" || value == "audit")) { projection = value; continue; }
        if ((command == "source-context" || source_read || source_inspect) && option == "--max-bytes" && !view_bytes) {
            std::size_t count = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count < 2048 || count > 32768)
                throw Error("invalid-arguments", "Compact source view requires 2048..32768 bytes.");
            view_bytes = count;
            continue;
        }
        if (source_inspect && option == "--path" && !source_path) { source_path = value; continue; }
        if (source_inspect && option == "--snapshot" && !snapshot) { snapshot = value; continue; }
        if (source_read && option == "--input" && !input_path) { input_path = fs::path(value); continue; }
        if (source_command && option == "--query" && !query) {
            query = value;
            continue;
        }
        if (archive_command && option == "--archive" && !archive_path) {
            archive_path = fs::path(value);
            continue;
        }
        if (option == "--delivery-config" && !delivery_path && (command == "sample" || command == "watch" || command == "deliver" || command == "reconcile")) {
            delivery_path = fs::path(value);
            continue;
        }
        if (count_seen || !((command == "watch" && option == "--samples") || (command == "deliver" && option == "--max-submissions")))
            throw Error("invalid-arguments", "Unsupported or duplicate Observer option.");
        count_seen = true;
        auto& count = command == "watch" ? samples : maximum;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count < 1 || count > (command == "watch" ? 1000U : 128U))
            throw Error("invalid-arguments", "Requested count exceeds its command bound.");
    }
    if ((command == "deliver" || command == "reconcile") && !delivery_path) throw Error("invalid-arguments", "Delivery/reconciliation requires --delivery-config.");
    if (archive_command && !archive_path) throw Error("invalid-arguments", "Outbox export/verification requires --archive.");
    if (source_command && !query) throw Error("invalid-arguments", "Source search requires --query.");
    if (source_inspect && (!source_path || !snapshot)) throw Error("invalid-arguments", "Source inspection requires --path and --snapshot.");
    if (source_read && !input_path) throw Error("invalid-arguments", "Source reading requires --input FILE.");
    if (view_bytes && projection != "compact") throw Error("invalid-arguments", "--max-bytes requires --projection compact.");
    const auto config = load_config(argv[3]);
    const auto delivery = delivery_path ? std::optional(load_delivery_config(config, *delivery_path)) : std::nullopt;
    if (std::signal(SIGINT, stop) == SIG_ERR || std::signal(SIGTERM, stop) == SIG_ERR)
        throw Error("signal-setup-failed", "Cannot install Observer stop handlers.");
    Json result;
    if (source_command) result = query_source(config, *query, command == "source-context");
    else if (source_inspect) result = inspect_source(config, *source_path, *snapshot);
    else if (source_read) result = read_source(config, read_text(*input_path, 128 * 1024, true));
    else if (command == "status") result = observer_status(config);
    else if (archive_command) result = archive_outbox(config, *archive_path, command == "outbox-verify");
    else if (command == "metrics") result = observer_metrics(config);
    else if (command == "reconcile" && delivery) result = reconcile_outbox(config, *delivery);
    else if (command == "deliver" && delivery) result = deliver_outbox(config, *delivery, maximum, [] { return interrupted.test(std::memory_order_relaxed); });
    else result = run_observer(config, samples, [] { return interrupted.test(std::memory_order_relaxed); }, delivery ? &*delivery : nullptr);
    if (projection == "compact") std::cout << source_view(result, view_bytes.value_or(8192));
    else std::cout << response(result).dump() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) noexcept {
    try { return run(argc, argv); }
    catch (const shin::observer::Error& error) {
        try {
            auto output = shin::observer::response(nullptr);
            output.erase("result"); output["ok"] = false;
            output["error"] = {{"code", error.code}, {"message", error.what()}};
            std::cerr << output.dump() << '\n';
            return 1;
        } catch (...) {
            return failure();
        }
    } catch (...) {
        return failure();
    }
}
