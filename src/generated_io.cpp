#include "generated_io.h"

#include <atomic>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "shin/windows/file.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace shin::generated_io {
namespace {

const fs::path kControlRelativeRoot = fs::path("docs") / "kb";
const fs::path kLockName = ".shin-context.lock";
const fs::path kWriterLockName = ".shin-context-writer.lock";
const fs::path kOwnerName = ".shin-context-owner";
const fs::path kFreshnessLockName = ".shin-context-freshness.lock";
const fs::path kFreshnessCacheName = ".shin-context-freshness.json";
const fs::path kMarkerName = ".shin-context-publishing";
const fs::path kBackupName = ".shin-context-backup";
constexpr const char* kStagePrefix = ".shin-context-stage.";
constexpr const char* kFreshnessStagePrefix = ".shin-context-freshness-stage.";
constexpr std::uintmax_t kMaxFreshnessCacheBytes = 64ULL * 1024ULL * 1024ULL;

std::atomic<unsigned long long> g_stage_counter{0};

unsigned long long process_id() noexcept {
#ifdef _WIN32
    return static_cast<unsigned long long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

fs::path control_root(const fs::path& out_root) {
    return out_root / kControlRelativeRoot;
}

fs::path lock_path(const fs::path& out_root, LockTarget target) {
    switch (target) {
        case LockTarget::index:
            return control_root(out_root) / kLockName;
        case LockTarget::writer:
        case LockTarget::ownership:
            return control_root(out_root) / kWriterLockName;
        case LockTarget::freshness:
            return control_root(out_root) / kFreshnessLockName;
    }
    throw std::runtime_error("Unknown generated output lock target");
}

fs::path marker_path(const fs::path& out_root) {
    return control_root(out_root) / kMarkerName;
}

fs::path freshness_cache_path(const fs::path& out_root) {
    return control_root(out_root) / kFreshnessCacheName;
}

void reject_symlink_or_non_directory(const fs::path& path, const std::string& label) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status)) {
        return;
    }
    if (fs::is_symlink(status)) {
        throw std::runtime_error(label + " must not be a symlink: " + path.string());
    }
    if (!fs::is_directory(status)) {
        throw std::runtime_error(label + " must be a directory: " + path.string());
    }
}

void ensure_control_root(const fs::path& out_root) {
#ifdef _WIN32
    const auto absolute = fs::absolute(out_root).lexically_normal();
    auto existing = absolute;
    std::vector<fs::path> missing;
    while (!fs::exists(fs::symlink_status(existing))) {
        if (existing == existing.parent_path()) throw std::runtime_error("Output ancestor is unavailable.");
        missing.push_back(existing); existing = existing.parent_path();
    }
    shin::windows::Directory pinned(existing);
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) shin::windows::Directory::ensure_user_owned(*it);
    shin::windows::Directory checked(absolute, shin::windows::Directory::Access::user_owned);
    shin::windows::Directory::ensure_user_owned(absolute / "docs");
    shin::windows::Directory::ensure_user_owned(control_root(absolute));
#else
    reject_symlink_or_non_directory(out_root, "Output root");
    fs::create_directories(out_root);

    fs::path current = out_root;
    for (const fs::path& component : {fs::path("docs"), fs::path("kb")}) {
        current /= component;
        reject_symlink_or_non_directory(current, "Generated output directory");
        fs::create_directory(current);
    }
#endif
}

void reject_symlink_or_non_regular_file(const fs::path& path, const std::string& label) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status)) {
        return;
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw std::runtime_error(label + " must be a regular file: " + path.string());
    }
}

void write_private_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("Cannot flush " + path.string());
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot close " + path.string());
    }
}

fs::path create_stage_path(const fs::path& out_root, const char* prefix = kStagePrefix) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const unsigned long long sequence = g_stage_counter.fetch_add(1, std::memory_order_relaxed);
        const fs::path candidate =
            control_root(out_root) /
            (std::string(prefix) + std::to_string(process_id()) + "." + std::to_string(sequence));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            throw std::runtime_error("Cannot create generated staging directory: " + ec.message());
        }
    }
    throw std::runtime_error("Cannot allocate a unique generated staging directory");
}

fs::path generated_file_relative_path(const fs::path& generated_relative_root, const std::string& path) {
    const fs::path relative = fs::path(path).lexically_relative(generated_relative_root);
    if (relative.empty() || relative.is_absolute()) {
        throw std::runtime_error("Generated file is outside the generated root: " + path);
    }
    for (const fs::path& component : relative) {
        if (component == "..") {
            throw std::runtime_error("Generated file escapes the generated root: " + path);
        }
    }
    return relative;
}

void stage_snapshot(const fs::path& stage,
                    const fs::path& generated_relative_root,
                    const std::map<std::string, std::string>& files) {
    try {
        for (const auto& [path, content] : files) {
            const fs::path relative = generated_file_relative_path(generated_relative_root, path);
            write_private_file(stage / relative, content);
        }
    } catch (...) {
        std::error_code cleanup_ec;
        fs::remove_all(stage, cleanup_ec);
        throw;
    }
}

void remove_tree_if_exists(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status)) {
        return;
    }
    if (fs::is_symlink(status)) {
        throw std::runtime_error("Refusing to remove generated control symlink: " + path.string());
    }
    fs::remove_all(path, ec);
    if (ec) {
        throw std::runtime_error("Cannot remove " + path.string() + ": " + ec.message());
    }
}

void rename_path(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        throw std::runtime_error("Cannot rename " + from.string() + " to " + to.string() + ": " + ec.message());
    }
}

void recover_interrupted_publication(const fs::path& active, const fs::path& backup, const fs::path& marker) {
    const bool active_exists = fs::exists(fs::symlink_status(active));
    const bool backup_exists = fs::exists(fs::symlink_status(backup));
    if (!active_exists && backup_exists) {
        reject_symlink_or_non_directory(backup, "Generated backup");
        rename_path(backup, active);
    } else if (active_exists && backup_exists) {
        reject_symlink_or_non_directory(active, "Generated snapshot");
        remove_tree_if_exists(backup);
    }
    std::error_code marker_ec;
    fs::remove(marker, marker_ec);
    if (marker_ec) {
        throw std::runtime_error("Cannot remove publication marker: " + marker_ec.message());
    }
}

void remove_stale_stages(const fs::path& out_root, const char* prefix = kStagePrefix) {
    for (const fs::directory_entry& entry : fs::directory_iterator(control_root(out_root))) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0) {
            remove_tree_if_exists(entry.path());
        }
    }
}

void commit_snapshot(const fs::path& out_root, const fs::path& stage, const fs::path& generated_relative_root) {
    const fs::path active = out_root / generated_relative_root;
    const fs::path backup = control_root(out_root) / kBackupName;
    const fs::path marker = marker_path(out_root);

    {
        IndexLock lock(out_root, LockMode::exclusive);
        recover_interrupted_publication(active, backup, marker);
        reject_symlink_or_non_directory(active, "Generated snapshot");
        remove_tree_if_exists(backup);
        write_private_file(marker, stage.filename().string() + "\n");

        bool active_moved = false;
        try {
            if (fs::exists(active)) {
                rename_path(active, backup);
                active_moved = true;
            }
            rename_path(stage, active);
            std::error_code marker_ec;
            fs::remove(marker, marker_ec);
            if (marker_ec) {
                throw std::runtime_error("Cannot remove publication marker: " + marker_ec.message());
            }
        } catch (...) {
            std::error_code restore_ec;
            if (!fs::exists(active) && active_moved && fs::exists(backup)) {
                fs::rename(backup, active, restore_ec);
                if (!restore_ec) {
                    std::error_code marker_ec;
                    fs::remove(marker, marker_ec);
                }
            }
            throw;
        }
    }
    std::error_code backup_ec;
    fs::remove_all(backup, backup_ec);
}

} // namespace

struct IndexLock::Impl {
#ifdef _WIN32
    std::unique_ptr<shin::windows::Directory> directory;
    std::unique_ptr<shin::windows::WriterLock> lock;
#else
    int descriptor = -1;
#endif
};

IndexLock::IndexLock(const fs::path& out_root,
                     LockMode mode,
                     LockTarget target,
                     const std::string& publisher)
    : impl_(std::make_unique<Impl>()) {
    const fs::path path = lock_path(out_root, target);
    const bool existing_only = mode == LockMode::shared_existing;
    if (existing_only) {
        for (const fs::path& directory : {out_root, out_root / "docs", control_root(out_root)}) {
            reject_symlink_or_non_directory(directory, "Generated output directory");
        }
    } else {
        ensure_control_root(out_root);
    }
    reject_symlink_or_non_regular_file(path, "Generated output lock");

#ifdef _WIN32
    impl_->directory = std::make_unique<shin::windows::Directory>(fs::absolute(control_root(out_root)).lexically_normal(),
        shin::windows::Directory::Access::user_owned);
    impl_->lock = impl_->directory->lock(path.filename().string(), mode == LockMode::exclusive, !existing_only, true);
#else
    int open_flags = existing_only ? O_RDONLY : O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif
    impl_->descriptor = open(path.c_str(), open_flags, 0600);
    if (impl_->descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "Cannot open index lock: " + path.string());
    }
    struct stat descriptor_status {};
    if (fstat(impl_->descriptor, &descriptor_status) != 0 || !S_ISREG(descriptor_status.st_mode)) {
        close(impl_->descriptor);
        impl_->descriptor = -1;
        throw std::runtime_error("Generated output lock must be a regular file: " + path.string());
    }
    const int operation = mode == LockMode::exclusive ? LOCK_EX : LOCK_SH;
    if (flock(impl_->descriptor, operation) != 0) {
        close(impl_->descriptor);
        impl_->descriptor = -1;
        throw std::runtime_error("Cannot acquire index lock: " + path.string());
    }
#endif
    try {
        if (target == LockTarget::writer && mode == LockMode::exclusive) {
            const auto owner = publisher_owner(out_root);
            if ((owner && *owner != publisher) || (!owner && !publisher.empty()))
                throw std::runtime_error("Index publisher ownership differs: use the managing Shin service or explicitly release its grant before manual writes.");
        }
    } catch (...) {
#ifdef _WIN32
        impl_->lock.reset();
#else
        flock(impl_->descriptor, LOCK_UN);
        close(impl_->descriptor); impl_->descriptor = -1;
#endif
        throw;
    }
}

std::optional<std::string> publisher_owner(const fs::path& out_root) {
    const auto path = control_root(out_root) / kOwnerName;
#ifdef _WIN32
    if (!fs::exists(fs::symlink_status(control_root(out_root)))) return std::nullopt;
    shin::windows::Directory directory(fs::absolute(control_root(out_root)).lexically_normal(),
        shin::windows::Directory::Access::user_owned);
    auto value = directory.read(kOwnerName.string(), 64);
    if (value && (value->size() != 64 || value->find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::runtime_error("Publisher ownership file is unsafe or invalid.");
    return value;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::runtime_error("Cannot safely read publisher ownership.");
    }
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat held{}, named{};
    if (::fstat(fd, &held) != 0 || !S_ISREG(held.st_mode) || held.st_uid != ::geteuid() ||
        (held.st_mode & 07777) != 0600 || held.st_nlink != 1 || held.st_size != 64)
        throw std::runtime_error("Publisher ownership file is unsafe or invalid.");
    std::string value(64, '\0');
    if (::read(fd, value.data(), value.size()) != 64 ||
        value.find_first_not_of("0123456789abcdef") != std::string::npos ||
        ::lstat(path.c_str(), &named) != 0 || held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
        named.st_nlink != 1 || named.st_mode != held.st_mode)
        throw std::runtime_error("Publisher ownership changed while reading.");
    return value;
#endif
}

void claim_publisher(const fs::path& out_root, const std::string& owner) {
    if (owner.size() != 64 || owner.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid publisher identity.");
    IndexLock lock(out_root, LockMode::exclusive, LockTarget::ownership);
    if (const auto existing = publisher_owner(out_root)) {
        if (*existing != owner) throw std::runtime_error("Index already has another publisher.");
        return;
    }
#ifdef _WIN32
    shin::windows::Directory directory(fs::absolute(control_root(out_root)).lexically_normal(),
        shin::windows::Directory::Access::user_owned);
    (void)directory.create(kOwnerName.string(), owner);
#else
    const auto path = control_root(out_root) / kOwnerName;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("Cannot claim index publisher ownership.");
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    if (::write(fd, owner.data(), owner.size()) != 64 || ::fsync(fd) != 0)
        throw std::runtime_error("Publisher claim is incomplete; preserve it for explicit recovery.");
    const int directory = ::open(control_root(out_root).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) throw std::runtime_error("Publisher claim directory is unavailable.");
    Close close_directory{directory};
    if (::fsync(directory) != 0) throw std::runtime_error("Cannot synchronize publisher ownership.");
#endif
}

void release_publisher(const fs::path& out_root, const std::string& owner) {
    IndexLock lock(out_root, LockMode::exclusive, LockTarget::ownership);
    const auto existing = publisher_owner(out_root);
    if (!existing) return;
    if (*existing != owner) throw std::runtime_error("Cannot release another publisher's ownership.");
#ifdef _WIN32
    shin::windows::Directory directory(fs::absolute(control_root(out_root)).lexically_normal(),
        shin::windows::Directory::Access::user_owned);
    if (!directory.remove(kOwnerName.string(), 64, owner))
        throw std::runtime_error("Publisher ownership disappeared before release.");
#else
    const auto path = control_root(out_root) / kOwnerName;
    if (::unlink(path.c_str()) != 0) throw std::runtime_error("Cannot release publisher ownership.");
    const int directory = ::open(control_root(out_root).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) throw std::runtime_error("Publisher directory is unavailable after release.");
    const int result = ::fsync(directory); ::close(directory);
    if (result != 0) throw std::runtime_error("Cannot synchronize publisher release.");
#endif
}

IndexLock::~IndexLock() {
    if (!impl_) {
        return;
    }
#ifdef _WIN32
    impl_->lock.reset();
#else
    if (impl_->descriptor < 0) {
        return;
    }
    flock(impl_->descriptor, LOCK_UN);
    close(impl_->descriptor);
#endif
}

bool publication_in_progress(const fs::path& out_root) {
    return fs::exists(marker_path(out_root));
}

std::optional<std::string> read_freshness_cache(const fs::path& out_root) {
    std::unique_ptr<IndexLock> lock;
    try {
        lock = std::make_unique<IndexLock>(out_root, LockMode::shared_existing, LockTarget::freshness);
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory) return std::nullopt;
        throw;
    }
    const fs::path path = freshness_cache_path(out_root);
    std::error_code status_ec;
    const fs::file_status status = fs::symlink_status(path, status_ec);
    if (status_ec || !fs::exists(status)) {
        return std::nullopt;
    }
    reject_symlink_or_non_regular_file(path, "Freshness cache");
    std::error_code size_ec;
    const std::uintmax_t byte_count = fs::file_size(path, size_ec);
    if (size_ec) {
        throw std::runtime_error("Cannot determine freshness cache size: " + size_ec.message());
    }
    if (byte_count > kMaxFreshnessCacheBytes) {
        throw std::runtime_error("Freshness cache exceeds the 64 MiB safety limit: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read freshness cache: " + path.string());
    }
    std::string content(static_cast<std::size_t>(byte_count), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (input.bad() || input.gcount() != static_cast<std::streamsize>(content.size())) {
        throw std::runtime_error("Cannot read freshness cache: " + path.string());
    }
    return content;
}

void write_freshness_cache(const fs::path& out_root, const std::string& content) {
    if (content.size() > kMaxFreshnessCacheBytes) {
        throw std::runtime_error("Freshness cache exceeds the 64 MiB safety limit");
    }
    IndexLock lock(out_root, LockMode::exclusive, LockTarget::freshness);
    const fs::path target = freshness_cache_path(out_root);
    reject_symlink_or_non_regular_file(target, "Freshness cache");
    remove_stale_stages(out_root, kFreshnessStagePrefix);
    const fs::path stage = create_stage_path(out_root, kFreshnessStagePrefix);
    const fs::path staged_file = stage / "freshness.json";
    try {
        write_private_file(staged_file, content);
#ifdef _WIN32
        std::error_code remove_ec;
        fs::remove(target, remove_ec);
        if (remove_ec) {
            throw std::runtime_error("Cannot replace freshness cache: " + remove_ec.message());
        }
#endif
        rename_path(staged_file, target);
        std::error_code cleanup_ec;
        fs::remove(stage, cleanup_ec);
    } catch (...) {
        std::error_code cleanup_ec;
        fs::remove_all(stage, cleanup_ec);
        throw;
    }
}

void recover_snapshot(const fs::path& out_root, const fs::path& generated_relative_root) {
    ensure_control_root(out_root);
    IndexLock lock(out_root, LockMode::exclusive);
    const fs::path active = out_root / generated_relative_root;
    const fs::path backup = control_root(out_root) / kBackupName;
    recover_interrupted_publication(active, backup, marker_path(out_root));
    reject_symlink_or_non_directory(active, "Generated snapshot");
    remove_stale_stages(out_root);
}

void publish_snapshot(const fs::path& out_root,
                      const fs::path& generated_relative_root,
                      const std::map<std::string, std::string>& files) {
    if (files.empty()) {
        throw std::runtime_error("Generated snapshot has no files");
    }
    ensure_control_root(out_root);
    const fs::path stage = create_stage_path(out_root);
    stage_snapshot(stage, generated_relative_root, files);
    try {
        commit_snapshot(out_root, stage, generated_relative_root);
    } catch (...) {
        std::error_code cleanup_ec;
        fs::remove_all(stage, cleanup_ec);
        throw;
    }
}

void prune_snapshot(const fs::path& out_root,
                    const fs::path& generated_relative_root,
                    const std::string& expected_manifest_content) {
    IndexLock writer_lock(out_root, LockMode::exclusive, LockTarget::writer);
    ensure_control_root(out_root);
    IndexLock index_lock(out_root, LockMode::exclusive, LockTarget::index);

    const fs::path active = out_root / generated_relative_root;
    const fs::path backup = control_root(out_root) / kBackupName;
    recover_interrupted_publication(active, backup, marker_path(out_root));
    remove_stale_stages(out_root);
    reject_symlink_or_non_directory(active, "Generated snapshot");

    const fs::path manifest = active / "manifest.json";
    reject_symlink_or_non_regular_file(manifest, "Generated manifest");
    std::error_code size_error;
    const std::uintmax_t manifest_bytes = fs::file_size(manifest, size_error);
    if (size_error || manifest_bytes != expected_manifest_content.size()) {
        throw std::runtime_error("Generated snapshot changed before prune: " +
                                 out_root.string());
    }
    std::ifstream input(manifest, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read generated manifest before prune: " +
                                 manifest.string());
    }
    std::string actual_manifest(expected_manifest_content.size(), '\0');
    input.read(actual_manifest.data(), static_cast<std::streamsize>(actual_manifest.size()));
    if (!input || actual_manifest != expected_manifest_content) {
        throw std::runtime_error("Generated snapshot changed before prune: " +
                                 out_root.string());
    }

    IndexLock freshness_lock(out_root, LockMode::exclusive, LockTarget::freshness);
    const fs::path freshness = freshness_cache_path(out_root);
    reject_symlink_or_non_regular_file(freshness, "Freshness cache");
    std::error_code remove_error;
    fs::remove(freshness, remove_error);
    if (remove_error) {
        throw std::runtime_error("Cannot remove freshness cache: " +
                                 remove_error.message());
    }
    remove_stale_stages(out_root, kFreshnessStagePrefix);
    remove_tree_if_exists(active);
}

} // namespace shin::generated_io
