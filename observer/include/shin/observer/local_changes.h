#pragma once

#include "io.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace shin::observer {
class LocalChanges {
public:
    // The predicate may run on a native worker; captures must outlive this object.
    // directory=true also denotes uncertainty about the entire subtree. Returning
    // false for a directory permits Linux to prune it. Do not reenter this object.
    // Invalid arguments throw Error; native/setup/predicate failures are unhealthy.
    LocalChanges(std::vector<fs::path> roots,
                 std::function<bool(const fs::path&, bool directory)> relevant);
    ~LocalChanges();
    LocalChanges(const LocalChanges&) = delete;
    LocalChanges& operator=(const LocalChanges&) = delete;
    LocalChanges(LocalChanges&&) = delete;
    LocalChanges& operator=(LocalChanges&&) = delete;

    // An invalidation token, not a count of writes (native events coalesce).
    // Health is sticky false after lost events, setup failure or token overflow.
    std::uint64_t sequence() const noexcept;
    bool healthy() const noexcept;
    // Safe first failure code only, no native path or exception text.
    std::string failure_reason() const;
    // Fence native delivery before comparing qualification's before/after tokens.
    // Destruction must be externally serialized with all calls to this object.
    void flush();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace shin::observer
