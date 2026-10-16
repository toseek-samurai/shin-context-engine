#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace shin::windows {
// NTFS hints, not source authority. Loss/overflow requires full requalification.
// One event-driven thread; up to 63 deduplicated root/ancestor handles.
// NTFS ancestor rename can refuse while a descendant handle is open; this
// adapter alone is not desktop lifecycle/release qualification.
class DirectoryChanges {
public:
    DirectoryChanges(const std::vector<std::filesystem::path>& roots,
        std::function<void(const std::filesystem::path&, bool)> changed, std::function<void()> lost);
    ~DirectoryChanges();
    DirectoryChanges(const DirectoryChanges&) = delete;
    DirectoryChanges& operator=(const DirectoryChanges&) = delete;
    // Drain completed native events and verify root identities. Not a global
    // filesystem transaction; callers retain selected-source/currentness checks.
    void flush();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
