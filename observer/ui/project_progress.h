#pragma once
#include "language.h"
#include <nlohmann/json.hpp>
#include <array>
#include <functional>
#include <string>
#include <string_view>

namespace shin::ui {
inline constexpr std::array<std::string_view, 6> project_connection_stages{
    "checking-project", "candidate-build", "candidate-validation", "source-cache",
    "starting-service", "checking-connection"};

inline int project_connection_stage(std::string_view value) {
    for (std::size_t i = 0; i < project_connection_stages.size(); ++i)
        if (project_connection_stages[i] == value) return static_cast<int>(i);
    return -1;
}
inline const char* project_connection_label(int stage) {
    switch (stage) {
    case 0: return tr("Checking project settings");
    case 1: return tr("Building the context index");
    case 2: return tr("Checking the generated index");
    case 3: return tr("Preparing project search");
    case 4: return tr("Starting the background service");
    case 5: return tr("Confirming project readiness");
    default: return tr("Preparing project…");
    }
}

// Presentation only. stdout remains the command receipt; errors on stderr must
// survive interleaved, fragmented progress frames. Callers retain their raw pipe
// byte/deadline bounds. At most six forward stage notifications reach the UI.
class ProjectProgressReader {
    std::string pending_, diagnostics_;
    int last_ = -1;
public:
    void feed(std::string_view bytes, const std::function<void(int)>& progress) {
        pending_.append(bytes);
        std::size_t offset = 0;
        for (;;) {
            const auto end = pending_.find('\n', offset);
            if (end == std::string::npos) break;
            const auto line = std::string_view(pending_).substr(offset, end - offset);
            const auto frame = line.size() <= 512 ? nlohmann::json::parse(line, nullptr, false) : nlohmann::json();
            int stage = -1;
            if (frame.is_object() && frame.size() == 2 &&
                frame.value("schema_version", nlohmann::json()) == "shin.project-progress.v1" &&
                frame.contains("stage") && frame.at("stage").is_string())
                stage = project_connection_stage(frame.at("stage").get_ref<const std::string&>());
            if (stage < 0) diagnostics_.append(pending_, offset, end + 1 - offset);
            else if (stage > last_) { last_ = stage; if (progress) progress(stage); }
            offset = end + 1;
        }
        pending_.erase(0, offset);
    }
    std::string finish() { diagnostics_ += pending_; pending_.clear(); return diagnostics_; }
};
} // namespace shin::ui
