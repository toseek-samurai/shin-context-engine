#include "view.h"
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace shin::observer;
using namespace shin::observer::tui;
namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class Predicate> void wait_for(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error("update state did not settle");
        std::this_thread::yield();
    }
}
void model_tests() {
    UpdateController unavailable;
    require(unavailable.snapshot().phase == UpdatePhase::unsupported && !unavailable.check(), "unsupported backend launched a check");
    std::promise<void> release_check;
    auto checked = release_check.get_future().share();
    std::atomic<int> installs{0}, acknowledgements{0};
    ReleaseUpdateProgressSink retained_progress;
    UpdateController controller([&](const std::string& command, const std::string& revision,
                                  const ReleaseUpdateProgressSink& progress, std::stop_token) -> Json {
        if (command == "update-check") {
            checked.wait();
            return {{"ok", true}, {"available", true}, {"version", "2.0.5-dev.99"}, {"revision", "offer-r1"}};
        }
        if (command == "update-install") {
            ++installs; require(revision == "offer-r1", "unreviewed revision entered updater");
            retained_progress = progress;
            progress({ReleaseUpdateStep::downloading, 50, 100});
            progress({ReleaseUpdateStep::switching_kit}); progress({ReleaseUpdateStep::rolling_back}); progress({ReleaseUpdateStep::failed});
            return {{"ok", false}, {"phase", "review-required"}, {"candidate", "/fixture/candidate"}};
        }
        if (command == "update-status") return {{"ok", false}, {"phase", "review-required"}, {"can_acknowledge", true}, {"review_revision", "result-r1"}};
        ++acknowledgements; require(revision == "result-r1", "unreviewed result acknowledged");
        return {{"ok", true}, {"phase", "reviewed"}};
    });
    require(controller.check() && !controller.check() && !controller.install("offer-r1"), "pending check was duplicated or bypassed");
    release_check.set_value(); wait_for([&] { return controller.snapshot().phase == UpdatePhase::offer; });
    require(!controller.install("") && !controller.install("stale") && installs == 0, "stale offer was installed");
    require(controller.install("offer-r1"), "reviewed offer was refused");
    wait_for([&] { return !controller.snapshot().busy(); });
    auto outcome = controller.snapshot();
    require(installs == 1 && outcome.phase == UpdatePhase::result && outcome.result.at("phase") == "review-required" &&
        outcome.timeline.current.step == ReleaseUpdateStep::failed && !outcome.result.at("ok").get<bool>(),
        "progress was mistaken for a successful installation");
    require(!controller.acknowledge("result-r1"), "result was acknowledged before explicit review");
    require(controller.review(), "result review failed"); wait_for([&] { return !controller.snapshot().busy(); });
    require(!controller.acknowledge("stale") && controller.acknowledge("result-r1"), "acknowledgement lost revision binding");
    wait_for([&] { return !controller.snapshot().busy(); });
    require(acknowledgements == 1 && controller.snapshot().result.at("phase") == "reviewed", "verified result did not settle");
    retained_progress({ReleaseUpdateStep::downloading, 99, 100});
    require(controller.snapshot().timeline.current.step == ReleaseUpdateStep::failed,
        "late progress from an old operation overwrote the reviewed outcome");
    require(!controller.acknowledge("result-r1"), "acknowledgement repeated itself");
    std::promise<void> started;
    auto start = started.get_future();
    {
        UpdateController cancelled([&](const std::string&, const std::string&, const ReleaseUpdateProgressSink&, std::stop_token stop) -> Json {
            started.set_value();
            std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; });
            throw Error("update-cancelled", "Read-only check cancelled.");
        });
        cancelled.check(); require(start.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "cancellable check did not start");
    }
    ReleaseUpdateProgressSink retired;
    {
        UpdateController temporary([&](const std::string&, const std::string&, const ReleaseUpdateProgressSink& progress, std::stop_token) -> Json {
            retired = progress; return {{"ok", true}, {"available", false}};
        });
        temporary.check(); wait_for([&] { return temporary.snapshot().phase == UpdatePhase::current; });
    }
    retired({ReleaseUpdateStep::completed}); // A retired presenter never leaves a dangling controller callback.
    std::cout << "Update controller: explicit revisions, coalescing, progress versus receipt, recovery review, cancellable checks passed\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 1) { model_tests(); return 0; }
        if (argc != 4 || std::string_view(argv[1]) != "stats" || std::string_view(argv[2]) != "--home") return 2;
        LocalProjectRequest request; request.home = argv[3];
        std::atomic<int> installed{0}, acknowledged{0};
        auto operation = [&](const std::string& command, const std::string& revision,
                              const ReleaseUpdateProgressSink& progress, std::stop_token) -> Json {
            if (command == "update-check") return {{"ok", true}, {"available", true}, {"current_version", "2.0.5-dev.45"},
                {"version", "2.0.5-dev.99"}, {"revision", "reviewed-fixture"}, {"release_url", "https://github.com/example/project/releases/tag/v2.0.5-dev.99"}};
            if (command == "update-status") return {{"ok", false}, {"phase", "review-required"}, {"can_acknowledge", true}, {"review_revision", "reviewed-result"}};
            if (command == "update-acknowledge") {
                require(revision == "reviewed-result", "wrong result revision"); ++acknowledged;
                return {{"ok", true}, {"phase", "reviewed"}};
            }
            require(command == "update-install" && revision == "reviewed-fixture", "unexpected installation request"); ++installed;
            // This is test-owned observation pacing, not product retry/sleep logic.
            progress({ReleaseUpdateStep::downloading, 50, 100}); std::this_thread::sleep_for(std::chrono::seconds(2));
            progress({ReleaseUpdateStep::checking_projects, 2, 3}); std::this_thread::sleep_for(std::chrono::seconds(1));
            progress({ReleaseUpdateStep::completed});
            return {{"ok", true}, {"phase", "installed"}, {"version", "2.0.5-dev.99"}};
        };
        const auto code = run(request, {}, "en", operation);
        std::cout << "\nfixture-installs=" << installed << " acknowledgements=" << acknowledged << '\n'; return code;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
