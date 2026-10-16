#include "source_cache.h"
#include "context_internal.h"
#include "shin/windows/file.h"
#include <system_error>
#include <windows.h>

namespace shin::context {
namespace {
[[noreturn]] void cache_error(const char* code) {
    throw CliError(code, "source-cache", "Private source cache is unavailable or unsafe.",
                   1, false, "Uncached source search remains available; unsafe cache data is not reused.");
}
template<class Action> auto checked(Action&& action, const char* code) {
    try { return action(); }
    catch (const CliError&) { throw; }
    catch (const std::system_error& error) {
        if (error.code().category() == std::system_category() && error.code().value() == ERROR_SHARING_VIOLATION)
            cache_error("source-cache-busy");
        cache_error(code);
    }
    catch (const std::runtime_error&) { cache_error(code); }
}
}
struct SourceCache::Impl {
    std::unique_ptr<windows::PrivateCache> cache;
};
SourceCache::SourceCache(const fs::path& directory, const std::string& key, bool recover_pending) : impl_(std::make_unique<Impl>()) {
    impl_->cache = checked([&] { return std::make_unique<windows::PrivateCache>(directory, key, recover_pending); },
                           "source-cache-invalid-directory");
}
SourceCache::~SourceCache() = default;
std::optional<std::string> SourceCache::load() const {
    return checked([&] { return impl_->cache->load(maximum_bytes); }, "source-cache-unsafe-file");
}
void SourceCache::save(std::string_view bytes) const {
    if (bytes.size() > maximum_bytes) cache_error("source-cache-io");
    checked([&] { impl_->cache->save(bytes); }, "source-cache-unsafe-file");
}
}
