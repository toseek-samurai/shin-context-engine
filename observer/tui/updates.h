#pragma once
#include "shin/observer/update_progress.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <stop_token>
#include <thread>

namespace shin::observer::tui {
enum class UpdatePhase { idle, unsupported, checking, offer, current, installing, result, acknowledging, failed };
struct Updates {
    UpdatePhase phase = UpdatePhase::idle;
    Json offer = Json::object(), result = Json::object();
    std::string error;
    UpdateTimeline timeline;
    std::chrono::steady_clock::time_point changed{};
    bool busy() const { return phase == UpdatePhase::checking || phase == UpdatePhase::installing || phase == UpdatePhase::acknowledging; }
    bool installing() const { return phase == UpdatePhase::installing || phase == UpdatePhase::acknowledging; }
};
// Presentation owns no install policy. The same native updater handles integrity,
// offer revisions, qualification, journals and rollback. Injected operations let
// tests exercise the real UI lifecycle without downloading or replacing a kit.
using UpdateOperation = std::function<Json(const std::string&, const std::string&,
    const ReleaseUpdateProgressSink&, std::stop_token)>;
class UpdateController {
public:
    explicit UpdateController(UpdateOperation operation = {});
    ~UpdateController();
    Updates snapshot();
    bool check();
    bool review();
    bool install(const std::string& revision);
    bool acknowledge(const std::string& revision);
private:
    bool queue(const std::string& command, const std::string& revision = {});
    void work(std::stop_token stop);
    UpdateOperation operation_;
    struct Shared { std::mutex mutex; Updates value; std::uint64_t operation = 0; };
    std::shared_ptr<Shared> state_ = std::make_shared<Shared>();
    std::condition_variable_any changed_;
    std::string command_, revision_;
    std::jthread worker_;
};
} // namespace shin::observer::tui
