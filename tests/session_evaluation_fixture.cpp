#include "context_internal.h"
#include "generated_snapshot.h"
#include "impact.h"
#include "session_contract.h"

#include <charconv>
#include <future>
#include <fstream>

namespace fs = std::filesystem;

namespace shin::context {
namespace {

struct Invocation {
    std::string command;
    bool warm = false;
    RuntimeOptions options;
    SessionEvaluationOptions evaluation;
    std::string scope_mismatch;
};

[[noreturn]] void usage() {
    throw_usage_error(
        "invalid-session-evaluation-fixture",
        "usage: session-evaluation-fixture COMMAND cold|warm ROOT PROFILE_DIR OUT_DIR PROFILE "
        "TASK_ID THREAD_ID OBSERVED_AT REQUIRE_CURRENT JOBS CURRENT_TASK TRUSTED_PRODUCERS "
        "[SCOPE_MISMATCH_OR_SIGNAL_DIR]");
}

bool parse_bool(std::string_view value) {
    if (value == "0") return false;
    if (value == "1") return true;
    usage();
}

int parse_int(std::string_view value) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) usage();
    return result;
}

std::int64_t parse_time(std::string_view value) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) usage();
    return result;
}

std::set<std::string> parse_producers(std::string_view value) {
    std::set<std::string> producers;
    if (value == "-") return producers;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(',', start);
        const std::string item(value.substr(start, end == std::string_view::npos
                                                       ? value.size() - start : end - start));
        if (item.empty()) usage();
        producers.insert(item);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return producers;
}

std::string read_stdin() {
    return {std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
}

Invocation parse_invocation(int argc, char** argv) {
    if (argc != 14 && argc != 15) usage();
    Invocation invocation;
    invocation.command = argv[1];
    if (invocation.command != "checkpoint" && invocation.command != "resume" &&
        invocation.command != "impact" &&
        invocation.command != "host-resume" && invocation.command != "host-impact" && invocation.command != "host-checkpoint" &&
        invocation.command != "parallel-checkpoint" && invocation.command != "parallel-resume" &&
        invocation.command != "staged-resume") {
        usage();
    }
    const std::string_view temperature = argv[2];
    if (temperature == "cold") invocation.warm = false;
    else if (temperature == "warm") invocation.warm = true;
    else usage();

    invocation.options.root = fs::canonical(argv[3]);
    invocation.options.profile_dir = fs::canonical(argv[4]);
    invocation.options.out_root = fs::canonical(argv[5]);
    invocation.options.profile = make_profile(argv[6], invocation.options.profile_dir);
    invocation.options.force_index = true;
    invocation.evaluation.task_id = argv[7];
    invocation.evaluation.thread_id = argv[8];
    invocation.evaluation.observed_at = parse_time(argv[9]);
    invocation.evaluation.explicit_clock = true;
    invocation.evaluation.require_current = parse_bool(argv[10]);
    invocation.evaluation.jobs = parse_int(argv[11]);
    invocation.evaluation.current_task = std::string(argv[12]) == "-" ? "" : argv[12];
    invocation.evaluation.trusted_producers = parse_producers(argv[13]);
    if (argc == 15) invocation.scope_mismatch = argv[14];
    return invocation;
}

void attach_snapshot(Invocation& invocation) {
    if (!invocation.warm) return;
    const std::shared_ptr<const GeneratedSnapshot> snapshot = GeneratedSnapshot::load(invocation.options);
    invocation.options.generated_snapshot = snapshot;
    if (invocation.command == "staged-resume") return;
    if (invocation.scope_mismatch.empty()) return;
    if (invocation.scope_mismatch == "root") {
        invocation.options.root = invocation.options.profile_dir;
    } else if (invocation.scope_mismatch == "profile") {
        invocation.options.profile.id = "snapshot-profile-mismatch";
    } else if (invocation.scope_mismatch == "profile-config") {
        invocation.options.profile.config_hash = "fnv1a64:ffffffffffffffff";
    } else if (invocation.scope_mismatch == "profile-copy") {
        invocation.options.profile.source_label = "Untrusted mutable profile copy";
    } else if (invocation.scope_mismatch == "out") {
        invocation.options.out_root = invocation.options.profile_dir;
    } else {
        usage();
    }
}

Json evaluate(const Invocation& invocation, std::string_view input) {
    if (invocation.command == "impact") {
        ImpactEvaluationOptions command;
        command.require_current = invocation.evaluation.require_current;
        if (input != "-") command.receipt_options = invocation.evaluation;
        return evaluate_impact(invocation.options, command,
                               input == "-" ? std::nullopt
                                            : std::optional<std::string_view>(input));
    }
    if (invocation.command.ends_with("checkpoint")) {
        return evaluate_checkpoint(invocation.options, invocation.evaluation, input);
    }
    return evaluate_resume(invocation.options, invocation.evaluation, input);
}

void write_signal(const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw CliError("fixture-signal-unavailable", "filesystem",
                                "Cannot create native fixture signal.", 1, true,
                                "Verify the external fixture control directory.");
    output << "ready\n";
}

void wait_for_signal(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!fs::exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw CliError("fixture-signal-timeout", "runtime", "Native fixture did not receive its continue signal.",
                           1, true, "Resume the fixture only after the replacement publication completes.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int run_fixture(int argc, char** argv) {
    Invocation invocation = parse_invocation(argc, argv);
    const bool host_policy = invocation.command.starts_with("host-");
    if (host_policy) invocation.command.erase(0, 5);
    attach_snapshot(invocation);
    std::string input = read_stdin();
    if (host_policy) {
        const auto envelope = Json::parse(input);
        for (const auto& document : envelope.at("trusted_documents"))
            invocation.evaluation.trusted_receipt_documents.insert(document.get<std::string>());
        input = envelope.at("document").dump();
    }
    if (invocation.command == "staged-resume") {
        if (!invocation.warm || invocation.scope_mismatch.empty()) usage();
        const fs::path control = fs::canonical(invocation.scope_mismatch);
        if (!fs::is_directory(control)) usage();
        const Json before = evaluate(invocation, input);
        write_signal(control / "first-complete");
        wait_for_signal(control / "continue");
        const Json after = evaluate(invocation, input);
        std::cout << Json{{"schema_version", "shin.session-evaluation-fixture.v1"},
                          {"outputs", Json::array({before, after})}}.dump() << '\n';
        return after.value("ok", true) ? 0 : 1;
    }
    if (!invocation.command.starts_with("parallel-")) {
        const Json output = evaluate(invocation, input);
        std::cout << output.dump() << '\n';
        return output.value("ok", true) ? 0 : 1;
    }

    std::vector<std::future<Json>> calls;
    calls.reserve(8);
    for (int index = 0; index < 8; ++index) {
        calls.push_back(std::async(std::launch::async, [&invocation, &input] {
            return evaluate(invocation, input);
        }));
    }
    Json outputs = Json::array();
    int exit_code = 0;
    for (auto& call : calls) {
        Json output = call.get();
        if (!output.value("ok", true)) exit_code = 1;
        outputs.push_back(std::move(output));
    }
    std::cout << Json{{"schema_version", "shin.session-evaluation-fixture.v1"},
                      {"outputs", std::move(outputs)}}.dump() << '\n';
    return exit_code;
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        return shin::context::run_fixture(argc, argv);
    } catch (const shin::context::CliError& error) {
        shin::context::emit_error_result_noexcept(
            argc > 1 ? argv[1] : "session-evaluation-fixture", error.code(), error.category(),
            error.what(), error.exit_code(), error.retryable(), error.remediation());
        return error.exit_code();
    } catch (const std::exception& error) {
        shin::context::emit_error_result_noexcept(
            argc > 1 ? argv[1] : "session-evaluation-fixture", "execution-failed", "runtime",
            error.what(), 1, false, "Inspect the fixture invocation and generated snapshot.");
        return 1;
    }
}
