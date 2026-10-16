#include "shin/server/config.h"
#include "shin/server/store.h"

#include <pqxx/pqxx>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
    std::filesystem::path config_path;
    std::size_t iterations = 0;
    std::size_t warmup = 0;
};

bool parse_count(std::string_view text, std::size_t minimum, std::size_t maximum, std::size_t& output) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum) return false;
    output = value;
    return true;
}

bool parse_arguments(int argc, char** argv, Arguments& arguments) {
    if (argc != 7) return false;

    for (int index = 1; index < argc; index += 2) {
        const std::string_view option{argv[index]};
        const std::string_view value{argv[index + 1]};
        if (option == "--config" && arguments.config_path.empty()) {
            arguments.config_path = value;
        } else if (option == "--iterations" && arguments.iterations == 0) {
            if (!parse_count(value, 1, 10000, arguments.iterations)) return false;
        } else if (option == "--warmup" && arguments.warmup == 0) {
            if (!parse_count(value, 1, 10000, arguments.warmup)) return false;
        } else {
            return false;
        }
    }

    return !arguments.config_path.empty() && arguments.iterations != 0 && arguments.warmup != 0;
}

void require_ready(const shin::server::Json& readiness) {
    if (!readiness.is_object() || readiness.value("status", "") != "ready" ||
        readiness.value("scope", "") != "store") {
        throw std::runtime_error{"unexpected readiness result"};
    }
}

template <typename Function>
double measure_milliseconds(Function&& operation) {
    const auto started = Clock::now();
    operation();
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    if (!std::isfinite(elapsed) || elapsed < 0.0) throw std::runtime_error{"invalid elapsed time"};
    return elapsed;
}

std::vector<double> measure_warm_readiness(shin::server::Store& store,
                                           std::size_t warmup,
                                           std::size_t iterations) {
    for (std::size_t index = 0; index < warmup; ++index) require_ready(store.readiness());

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t index = 0; index < iterations; ++index) {
        samples.push_back(measure_milliseconds([&store] { require_ready(store.readiness()); }));
    }
    return samples;
}

std::vector<double> measure_raw_select_one(const shin::server::StoreConfig& config,
                                           std::size_t warmup,
                                           std::size_t iterations) {
    // Connection setup is intentionally outside the baseline: this is an already-connected-session lower bound.
    pqxx::connection connection{config.connection_string};
    const auto select_one = [&connection] {
        pqxx::read_transaction transaction{connection};
        const auto result = transaction.exec("SELECT 1");
        if (result.size() != 1 || result[0].size() != 1 || result[0][0].as<int>() != 1) {
            throw std::runtime_error{"unexpected SELECT 1 result"};
        }
        transaction.commit();
    };

    for (std::size_t index = 0; index < warmup; ++index) select_one();

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t index = 0; index < iterations; ++index) {
        samples.push_back(measure_milliseconds(select_one));
    }
    return samples;
}

int fail() {
    std::cerr << "error: store cost probe failed\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parse_arguments(argc, argv, arguments)) {
        std::cerr << "usage: shin-server-store-cost-probe --config PATH --iterations 1..10000 --warmup 1..10000\n";
        return 2;
    }

    try {
        const auto config = shin::server::load_config(arguments.config_path);
        shin::server::Store store{config.database};
        const auto cold_first_readiness_ms = measure_milliseconds([&store] { require_ready(store.readiness()); });
        const auto warm_pooled_readiness_ms = measure_warm_readiness(store, arguments.warmup, arguments.iterations);
        const auto raw_select1_lower_bound_ms = measure_raw_select_one(
            config.database, arguments.warmup, arguments.iterations);

        const shin::server::Json report{
            {"schema_version", "shin.server-store-cost-probe.v1"},
            {"tool", "shin-server-store-cost-probe"},
            {"ok", true},
            {"iterations", arguments.iterations},
            {"warmup", arguments.warmup},
            {"cold_first_readiness_ms", cold_first_readiness_ms},
            {"warm_pooled_readiness_ms", warm_pooled_readiness_ms},
            {"raw_select1_lower_bound_ms", raw_select1_lower_bound_ms},
        };
        std::cout << report.dump() << '\n';
        return 0;
    } catch (...) {
        return fail();
    }
}
