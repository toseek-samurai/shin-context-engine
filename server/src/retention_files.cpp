#include "shin/server/retention_files.h"
#include "shin/server/config.h"
#include "shin/server/route_files.h"
#include "context_internal.h"

#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <map>
#include <memory>
#include <utility>

namespace shin::server {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void unsafe_tree() {
    throw ServiceError(409, "retention-tree-unsafe", "Candidate tree is not a bounded managed artifact.");
}
[[noreturn]] void changed_tree() {
    throw ServiceError(409, "retention-tree-changed", "Candidate tree differs from the durable cleanup plan.");
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

std::string identity(const struct stat& info) {
    return std::to_string(static_cast<std::uintmax_t>(info.st_dev)) + ":" +
           std::to_string(static_cast<std::uintmax_t>(info.st_ino));
}

const std::map<std::string, bool>& managed_paths() {
    static const auto paths = [] {
        std::map<std::string, bool> result{{"", true}, {"docs", true}, {"docs/kb", true}, {"docs/kb/generated", true},
            {"docs/kb/.shin-context.lock", false}, {"docs/kb/.shin-context-writer.lock", false}};
        for (const bool compact : {false, true})
            for (const auto& path : shin::context::expected_generated_relative_files(compact)) result.emplace(path, false);
        return result;
    }();
    return paths;
}

bool same_stat(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
    const auto mt = before.st_mtimespec, ct = before.st_ctimespec;
    const auto next_mt = after.st_mtimespec, next_ct = after.st_ctimespec;
#else
    const auto mt = before.st_mtim, ct = before.st_ctim;
    const auto next_mt = after.st_mtim, next_ct = after.st_ctim;
#endif
    return identity(before) == identity(after) && before.st_size == after.st_size &&
        before.st_mode == after.st_mode && before.st_nlink == after.st_nlink && before.st_uid == after.st_uid &&
        mt.tv_sec == next_mt.tv_sec && mt.tv_nsec == next_mt.tv_nsec && ct.tv_sec == next_ct.tv_sec && ct.tv_nsec == next_ct.tv_nsec;
}

std::string hash_file(const fs::path& path, const struct stat& named) {
    Descriptor file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat before{}, after{};
    if (file.get() < 0 || ::fstat(file.get(), &before) != 0 || !same_stat(named, before)) unsafe_tree();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) unsafe_tree();
    std::array<char, 65536> buffer{};
    std::uint64_t consumed = 0;
    while (true) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) unsafe_tree();
        if (count == 0) break;
        consumed += static_cast<std::uint64_t>(count);
        if (consumed > static_cast<std::uint64_t>(before.st_size) ||
            EVP_DigestUpdate(digest.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) unsafe_tree();
    }
    if (::fstat(file.get(), &after) != 0 || !same_stat(before, after) ||
        consumed != static_cast<std::uint64_t>(before.st_size)) unsafe_tree();
    Digest value{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digest.get(), value.data(), &length) != 1 || length != value.size()) unsafe_tree();
    return digest_hex(value);
}

Json entry_record(const fs::path& root, const std::string& relative, std::uint64_t& remaining) {
    const auto expected = managed_paths().find(relative);
    if (expected == managed_paths().end()) unsafe_tree();
    const auto path = relative.empty() ? root : root / relative;
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || info.st_uid != ::geteuid() || (info.st_mode & 0022) != 0)
        unsafe_tree();
    const bool directory = expected->second;
    if ((directory && !S_ISDIR(info.st_mode)) || (!directory && (!S_ISREG(info.st_mode) || info.st_nlink != 1)))
        unsafe_tree();
    std::uint64_t bytes = 0;
    std::string hash;
    if (!directory) {
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > remaining) unsafe_tree();
        bytes = static_cast<std::uint64_t>(info.st_size);
        remaining -= bytes;
        hash = hash_file(path, info);
    }
    return {{"path", relative}, {"directory", directory}, {"identity", identity(info)},
            {"mode", info.st_mode & 0777}, {"bytes", bytes}, {"sha256", hash}};
}

std::map<std::string, Json> inventory_entries(const Json& inventory) {
    if (!inventory.is_object() || !inventory.contains("entries") || !inventory.at("entries").is_array() ||
        inventory.at("entries").size() > managed_paths().size()) changed_tree();
    std::map<std::string, Json> result;
    for (const auto& entry : inventory.at("entries")) {
        if (!entry.is_object() || !entry.contains("path") || !entry.at("path").is_string()) changed_tree();
        const auto path = entry.at("path").get<std::string>();
        const auto known = managed_paths().find(path);
        if (known == managed_paths().end() || !entry.contains("directory") || entry.at("directory") != known->second ||
            !result.emplace(path, entry).second) changed_tree();
    }
    return result;
}

void sync_directory(int descriptor) {
    if (::fsync(descriptor) != 0)
        throw ServiceError(503, "retention-sync-failed", "Cleanup durability could not be confirmed.");
}

Descriptor open_directory(int parent, const std::string& component, const Json& expected) {
    Descriptor opened(::openat(parent, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (opened.get() < 0 || ::fstat(opened.get(), &info) != 0 || identity(info) != expected.at("identity").get<std::string>() ||
        (info.st_mode & 0777) != expected.at("mode") || info.st_uid != ::geteuid()) changed_tree();
    return opened;
}
} // namespace

Json inspect_retention_tree(const fs::path& root, std::uint64_t maximum_bytes) {
    if (maximum_bytes == 0 || maximum_bytes > 1073741824) unsafe_tree();
    struct stat info{};
    Json entries = Json::array();
    if (::lstat(root.c_str(), &info) != 0) {
        if (errno == ENOENT) return {{"root_present", false}, {"entries", entries}, {"payload_bytes", 0}};
        unsafe_tree();
    }
    if (!S_ISDIR(info.st_mode) || (info.st_mode & 0077) != 0) unsafe_tree();
    (void)route_directory_identity(root);
    std::uint64_t remaining = maximum_bytes;
    std::map<std::string, Json> ordered;
    ordered.emplace("", entry_record(root, "", remaining));
    for (const auto& item : fs::recursive_directory_iterator(root)) {
        if (ordered.size() >= managed_paths().size()) unsafe_tree();
        const auto path = item.path().lexically_relative(root).generic_string();
        ordered.emplace(path, entry_record(root, path, remaining));
    }
    for (auto& [path, entry] : ordered) { (void)path; entries.push_back(std::move(entry)); }
    return {{"root_present", true}, {"entries", entries}, {"payload_bytes", maximum_bytes - remaining}};
}

void remove_retention_tree(const fs::path& state_root, const std::string& state_identity, const std::string& name,
                           const Json& inventory, std::uint64_t maximum_bytes) {
    if (!name.starts_with("job-") || name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
        changed_tree();
    if (route_directory_identity(state_root) != state_identity) changed_tree();
    const auto expected = inventory_entries(inventory);
    const auto root = state_root / name;
    const auto current = inventory_entries(inspect_retention_tree(root, maximum_bytes));
    for (const auto& [path, entry] : current) {
        const auto previous = expected.find(path);
        if (previous == expected.end() || previous->second != entry) changed_tree();
    }
    Descriptor state(::open(state_root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat state_info{};
    if (state.get() < 0 || ::fstat(state.get(), &state_info) != 0 || identity(state_info) != state_identity ||
        state_info.st_uid != ::geteuid() || (state_info.st_mode & 0077) != 0) unsafe_tree();

    // Delete only the persisted inventory, bottom-up; a crash leaves a verifiable subset.
    for (auto item = current.rbegin(); item != current.rend(); ++item) {
        if (route_directory_identity(state_root) != state_identity) changed_tree();
        const auto& [relative, entry] = *item;
        Descriptor parent(::dup(state.get()));
        if (parent.get() < 0) unsafe_tree();
        std::string leaf = name;
        if (!relative.empty()) {
            parent = open_directory(parent.get(), name, expected.at(""));
            fs::path walked;
            const auto relative_path = fs::path(relative);
            for (const auto& component : relative_path.parent_path()) {
                walked /= component;
                parent = open_directory(parent.get(), component.string(), expected.at(walked.generic_string()));
            }
            leaf = relative_path.filename().string();
        }
        struct stat named{};
        if (::fstatat(parent.get(), leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 || identity(named) != entry.at("identity").get<std::string>())
            changed_tree();
        if (::unlinkat(parent.get(), leaf.c_str(), entry.at("directory").get<bool>() ? AT_REMOVEDIR : 0) != 0)
            throw ServiceError(503, "retention-delete-failed", "Cleanup was interrupted; its reservation is retained.");
        sync_directory(parent.get());
    }
    sync_directory(state.get());
    if (route_directory_identity(state_root) != state_identity || fs::exists(fs::symlink_status(root))) changed_tree();
}

} // namespace shin::server
