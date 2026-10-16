#pragma once

#include "context_types.h"

namespace shin::context {

struct SnapshotLoadLimits {
    std::uintmax_t maximum_payload_bytes = 256ULL * 1024ULL * 1024ULL;
    std::uintmax_t maximum_file_bytes = 64ULL * 1024ULL * 1024ULL;
};

// Owns verified artifact bytes, never a publication lock or a reusable freshness proof.
class GeneratedSnapshot {
public:
    GeneratedSnapshot(const GeneratedSnapshot&) = delete;
    GeneratedSnapshot& operator=(const GeneratedSnapshot&) = delete;
    ~GeneratedSnapshot();
    static std::shared_ptr<const GeneratedSnapshot> load(
        RuntimeOptions options, SnapshotLoadLimits limits = {});

    const RuntimeOptions& options() const noexcept { return options_; }
    const Json& manifest() const noexcept { return manifest_; }
    const std::string& id() const noexcept { return id_; }
    std::uintmax_t payload_bytes() const noexcept { return payload_bytes_; }
    std::string_view file(const fs::path& path) const;
    bool contains(const fs::path& path) const;

private:
    GeneratedSnapshot();
    struct Backing;
    std::string relative_key(const fs::path& path) const;
    RuntimeOptions options_;
    Json manifest_;
    std::string id_;
    std::unique_ptr<Backing> backing_;
    std::map<std::string, std::string_view> files_;
    std::uintmax_t payload_bytes_ = 0;
};

// An independent cursor over either the CLI file stream or shared immutable bytes.
class GeneratedInput : public std::istream {
public:
    GeneratedInput(const RuntimeOptions& options, const fs::path& path);
    ~GeneratedInput() override;
    GeneratedInput(const GeneratedInput&) = delete;
    GeneratedInput& operator=(const GeneratedInput&) = delete;
private:
    std::shared_ptr<const GeneratedSnapshot> snapshot_;
    std::unique_ptr<std::streambuf> buffer_;
    std::ifstream file_;
};

bool generated_file_exists(const RuntimeOptions& options, const fs::path& path);

} // namespace shin::context
