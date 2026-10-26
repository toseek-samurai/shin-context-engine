#include "updates.h"

namespace shin::observer::tui {
UpdateController::UpdateController(UpdateOperation operation) : operation_(std::move(operation)) {
    if (!operation_) state_->value.phase = UpdatePhase::unsupported;
    else worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
}
UpdateController::~UpdateController() { worker_.request_stop(); changed_.notify_all(); }
Updates UpdateController::snapshot() { std::lock_guard lock(state_->mutex); return state_->value; }
bool UpdateController::check() { return queue("update-check"); }
bool UpdateController::review() { return queue("update-status"); }
bool UpdateController::install(const std::string& revision) { return queue("update-install", revision); }
bool UpdateController::acknowledge(const std::string& revision) { return queue("update-acknowledge", revision); }
bool UpdateController::queue(const std::string& command, const std::string& revision) {
    std::lock_guard lock(state_->mutex);
    if (!operation_ || state_->value.busy() || !command_.empty()) return false;
    if (command == "update-install" && (state_->value.phase != UpdatePhase::offer || revision.empty() ||
        state_->value.offer.value("revision", "") != revision)) return false;
    if (command == "update-acknowledge" && (state_->value.phase != UpdatePhase::result || revision.empty() ||
        !state_->value.result.value("can_acknowledge", false) || state_->value.result.value("review_revision", "") != revision)) return false;
    command_ = command; revision_ = revision; state_->value.error.clear();
    ++state_->operation;
    state_->value.phase = command == "update-install" ? UpdatePhase::installing :
        command == "update-acknowledge" ? UpdatePhase::acknowledging : UpdatePhase::checking;
    if (command == "update-install") { state_->value.timeline = {}; state_->value.changed = std::chrono::steady_clock::now(); }
    changed_.notify_all(); return true;
}
void UpdateController::work(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::string command, revision;
        std::uint64_t operation;
        {
            std::unique_lock lock(state_->mutex);
            changed_.wait(lock, stop, [&] { return !command_.empty(); });
            if (stop.stop_requested()) return;
            command.swap(command_); revision.swap(revision_);
            operation = state_->operation;
        }
        const auto observe = [state = state_, operation](ReleaseUpdateProgress progress) {
            std::lock_guard lock(state->mutex);
            if (state->operation != operation) return;
            if (state->value.timeline.current.step != progress.step) state->value.changed = std::chrono::steady_clock::now();
            state->value.timeline.observe(progress);
        };
        try {
            auto result = operation_(command, revision, observe, stop);
            if (!result.is_object()) throw Error("update-response-invalid", "Unexpected update response.");
            std::lock_guard lock(state_->mutex);
            if (command == "update-check" && result.value("ok", false)) {
                state_->value.offer = std::move(result);
                state_->value.phase = state_->value.offer.value("available", false) ? UpdatePhase::offer : UpdatePhase::current;
            } else {
                state_->value.result = std::move(result); state_->value.phase = UpdatePhase::result;
                if (command == "update-check") state_->value.phase = UpdatePhase::failed;
                // An acknowledge refusal retains the exact reviewed result. A
                // new explicit status read is required before another attempt.
                if (command == "update-acknowledge" && !state_->value.result.value("ok", false)) state_->value.phase = UpdatePhase::failed;
            }
        } catch (const Error& error) {
            std::lock_guard lock(state_->mutex); state_->value.phase = UpdatePhase::failed; state_->value.error = error.code;
        } catch (...) {
            std::lock_guard lock(state_->mutex); state_->value.phase = UpdatePhase::failed; state_->value.error = "update-unavailable";
        }
    }
}
} // namespace shin::observer::tui
