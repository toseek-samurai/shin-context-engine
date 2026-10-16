#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace shin::generated_io {

enum class LockMode {
    shared_existing,
    exclusive
};

enum class LockTarget {
    index,
    writer,
    ownership,
    freshness
};

class IndexLock {
public:
    IndexLock(const std::filesystem::path& out_root,
              LockMode mode,
              LockTarget target = LockTarget::index,
              const std::string& publisher = {});
    ~IndexLock();

    IndexLock(const IndexLock&) = delete;
    IndexLock& operator=(const IndexLock&) = delete;
    IndexLock(IndexLock&&) = delete;
    IndexLock& operator=(IndexLock&&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A durable single-writer claim. Normal CLI writers cannot bypass it.
std::optional<std::string> publisher_owner(const std::filesystem::path& out_root);
void claim_publisher(const std::filesystem::path& out_root, const std::string& owner);
void release_publisher(const std::filesystem::path& out_root, const std::string& owner);

bool publication_in_progress(const std::filesystem::path& out_root);

std::optional<std::string> read_freshness_cache(const std::filesystem::path& out_root);
void write_freshness_cache(const std::filesystem::path& out_root, const std::string& content);

// The caller must hold the writer lock while recovering or publishing snapshots.
void recover_snapshot(const std::filesystem::path& out_root,
                      const std::filesystem::path& generated_relative_root);

void publish_snapshot(const std::filesystem::path& out_root,
                      const std::filesystem::path& generated_relative_root,
                      const std::map<std::string, std::string>& files);

// Removes only the generated snapshot after proving it is still the snapshot
// inspected by the caller. Control locks remain available for a later rebuild.
void prune_snapshot(const std::filesystem::path& out_root,
                    const std::filesystem::path& generated_relative_root,
                    const std::string& expected_manifest_content);

} // namespace shin::generated_io
