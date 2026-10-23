#pragma once
#include <algorithm>
#include <cmath>

namespace shin::ui {
// Render-thread state only. No timers, polling, or animation of measured values.
class MonitorTween {
public:
    void snap(float value) { from_ = target_ = value; elapsed_ = duration_; }
    float value() const {
        const float t = std::clamp(elapsed_ / duration_, 0.0f, 1.0f);
        const float eased = 1 - (1 - t) * (1 - t) * (1 - t);
        return from_ + (target_ - from_) * eased;
    }
    float step(float target, float seconds, bool reduced, float duration = .16f) {
        if (reduced || !std::isfinite(seconds) || seconds < 0 || seconds > .5f) {
            snap(target);
            return target;
        }
        if (target != target_) {
            from_ = value(); target_ = target; elapsed_ = 0;
            duration_ = std::max(.01f, duration);
        }
        elapsed_ = std::min(duration_, elapsed_ + seconds);
        return value();
    }
    bool moving() const { return elapsed_ < duration_ && from_ != target_; }
private:
    float from_ = 0, target_ = 0, elapsed_ = .16f, duration_ = .16f;
};
} // namespace shin::ui
