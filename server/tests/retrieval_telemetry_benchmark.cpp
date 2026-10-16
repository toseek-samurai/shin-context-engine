#include "shin/server/retrieval_telemetry.h"

#include <array>
#include <exception>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace shin::server;
using Clock = RetrievalTelemetry::Clock;
constexpr std::size_t kIterations = 4096;
constexpr std::size_t kRepetitions = 5;

enum class Mode { baseline, counters, metadata, capture };

const char* name(Mode mode) {
    switch (mode) {
    case Mode::baseline: return "baseline";
    case Mode::counters: return "counters-only";
    case Mode::metadata: return "metadata-only";
    case Mode::capture: return "capture-request";
    }
    throw std::runtime_error("invalid benchmark mode");
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Json result_fixture() {
    return {{"route_id", "route"}, {"workspace_id", "workspace"}, {"source_host_id", "source-host"},
            {"snapshot_id", "snapshot"}, {"preparation_id", "preparation"}, {"revision", 7},
            {"evidence", {{"retrieval_mode", "strict"},
                {"evidence", Json::array({{{"path", "fixture.cpp"}, {"snippet", std::string(512, 's')}}})},
                {"query_diagnostics", {{"candidate_rows", 17}}},
                {"query_coverage", {{"covered_groups", 2}, {"total_groups", 3}}}}}};
}

Json run(Mode mode, std::size_t thread_count, std::size_t iterations) {
    ObservabilityConfig config;
    config.max_retained_requests = mode == Mode::counters ? 0 : 128;
    config.max_query_bytes = 4096;
    if (mode == Mode::capture) config.capture_query_projects = {"benchmark"};
    std::unique_ptr<RetrievalTelemetry> telemetry;
    if (mode != Mode::baseline) telemetry = std::make_unique<RetrievalTelemetry>(config);
    const Json request{{"query", std::string(2048, 'q')}, {"limit", 20}};
    const auto result = result_fixture();
    const auto result_bytes = result.dump().size();
    std::latch ready(static_cast<std::ptrdiff_t>(thread_count));
    std::latch release(1);
    std::latch done(static_cast<std::ptrdiff_t>(thread_count));
    std::vector<std::uint64_t> checksums(thread_count);
    std::vector<std::exception_ptr> errors(thread_count);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&, worker] {
            ready.count_down();
            release.wait();
            try {
                std::uint64_t checksum = 0;
                for (std::size_t i = 0; i < iterations; ++i) {
                    if (telemetry) {
                        auto attempt = telemetry->begin(RetrievalOperation::search, RetrievalSelector::route);
                        attempt.request_validated = true;
                        attempt.mark(RetrievalPhase::selection);
                        attempt.mark(RetrievalPhase::admission);
                        attempt.mark(RetrievalPhase::execution);
                        const auto encoded = result.dump();
                        attempt.mark(RetrievalPhase::finalization);
                        attempt.result_bytes = encoded.size();
                        telemetry->finish(attempt, "benchmark", "route", request, &result, 200, "");
                        checksum += encoded.size() + static_cast<unsigned char>(encoded.front()) +
                                    static_cast<unsigned char>(encoded.back());
                    } else {
                        // Baseline executes the same serialization; its observable checksum prevents an empty-loop comparison.
                        const auto encoded = result.dump();
                        checksum += encoded.size() + static_cast<unsigned char>(encoded.front()) +
                                    static_cast<unsigned char>(encoded.back());
                    }
                }
                checksums[worker] = checksum;
            } catch (...) { errors[worker] = std::current_exception(); }
            done.count_down();
        });
    }
    ready.wait();
    const auto started = Clock::now();
    release.count_down();
    done.wait();
    const auto stopped = Clock::now();
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    std::uint64_t checksum = 0;
    for (const auto value : checksums) {
        check(value == iterations * (result_bytes + static_cast<unsigned char>('{') + static_cast<unsigned char>('}')),
              "benchmark serialization workload was not fully observed");
        checksum += value;
    }
    const auto operations = thread_count * iterations;
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stopped - started).count();
    Json report{{"mode", name(mode)}, {"threads", thread_count}, {"iterations_per_thread", iterations},
                {"operations", operations}, {"elapsed_ns", elapsed_ns},
                {"ns_per_request", static_cast<double>(elapsed_ns) / static_cast<double>(operations)},
                {"checksum", checksum}, {"request_encoded_bytes", request.dump().size()},
                {"result_encoded_bytes", result_bytes}, {"retention", nullptr}};
    if (telemetry) {
        const auto metrics = telemetry->metrics();
        const auto& cell = metrics.at("cells")[0];
        check(cell.at("started") == operations && cell.at("completed") == operations &&
                  cell.at("succeeded") == operations && cell.at("active") == 0 && cell.at("failed") == 0 &&
                  metrics.at("collection_errors") == 0 && metrics.at("retention").at("dropped") == 0,
              "benchmark telemetry lost work");
        const auto records = telemetry->requests(0, 1, "");
        if (mode == Mode::counters) {
            check(records.at("items").empty() && metrics.at("retention").at("disabled") == operations,
                  "counter-only benchmark retained diagnostics or lost counts");
        } else {
            check(records.at("items").size() == 1 && records.at("items")[0].at("query_capture") ==
                      (mode == Mode::capture ? "captured" : "disabled"), "benchmark did not exercise the requested capture mode");
            check(records.at("items")[0].at("request") == (mode == Mode::capture ? request : Json(nullptr)),
                  "benchmark capture payload differs from the workload");
        }
        report["retention"] = metrics.at("retention");
    }
    return report;
}
} // namespace

int main() {
    try {
        constexpr std::array<Mode, 4> modes{Mode::baseline, Mode::counters, Mode::metadata, Mode::capture};
        for (const auto mode : modes) static_cast<void>(run(mode, 1, 64));
        Json samples = Json::array();
        for (const std::size_t threads : {1, 4, 8}) {
            for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
                // Rotate order to avoid giving one mode the same warmup/thermal position in every repetition.
                for (std::size_t offset = 0; offset < modes.size(); ++offset) {
                    auto sample = run(modes[(repetition + offset) % modes.size()], threads, kIterations);
                    sample["repetition"] = repetition;
                    samples.push_back(std::move(sample));
                }
            }
        }
        const Json output{{"schema_version", "shin.retrieval-telemetry-benchmark.v1"},
            {"scope", "in-process synthetic JSON serialization plus collector; no network, SQL or filesystem workload"},
            {"timing", "steady-clock wall time after worker readiness through completion; excludes thread creation, join and report reads"},
            {"baseline", "same result serialization and checked checksum, without collector or phase clock calls"},
            {"limitations", "not retrieval/service throughput, total RSS, or a speedup guarantee; compare repeated samples on the same build"},
            {"warmup_iterations_per_mode", 64}, {"samples", std::move(samples)}};
        std::cout << output.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
