#include "source_cache.h"
#include "context_internal.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace shin::context {
namespace {
void checked(bool ok, const char* code = "source-cache-io") {
    if (!ok) throw CliError(code, "source-cache", "Private source cache is unavailable or unsafe.",
                            1, false, "Check the private cache directory; uncached source search remains available.");
}
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (value >= 0) ::close(value);
        value = std::exchange(other.value, -1);
        return *this;
    }
};
struct stat status(int fd) {
    struct stat s {};
    checked(::fstat(fd, &s) == 0);
    return s;
}
void private_file(const struct stat& s) {
    checked(S_ISREG(s.st_mode) && s.st_uid == ::geteuid() && (s.st_mode & 07777) == 0600 && s.st_nlink == 1,
            "source-cache-unsafe-file");
}
bool same(const struct stat& a, const struct stat& b) {
#if defined(__APPLE__)
    const auto am = a.st_mtimespec, bm = b.st_mtimespec, ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
    const auto am = a.st_mtim, bm = b.st_mtim, ac = a.st_ctim, bc = b.st_ctim;
#endif
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mode == b.st_mode && a.st_uid == b.st_uid && a.st_nlink == b.st_nlink &&
        am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
Fd directory_at(const fs::path& path) {
    checked(path.is_absolute() && path.lexically_normal() == path, "source-cache-invalid-directory");
    Fd fd(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    checked(fd.value >= 0);
    for (const auto& part : path.relative_path()) {
        Fd child(::openat(fd.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        checked(child.value >= 0, "source-cache-invalid-directory");
        fd = std::move(child);
    }
    return fd;
}
}

struct SourceCache::Impl {
    Impl(const fs::path& path, const std::string& key, bool recover_pending) : path(path) {
        checked(!path.filename().empty() && key.size() == 16 &&
                key.find_first_not_of("0123456789abcdef") == std::string::npos, "source-cache-invalid-directory");
        const auto parent = directory_at(path.parent_path());
        checked(::mkdirat(parent.value, path.filename().c_str(), 0700) == 0 || errno == EEXIST);
        directory = directory_at(path);
        const auto s = status(directory.value);
        checked(s.st_uid == ::geteuid() && (s.st_mode & 07777) == 0700, "source-cache-invalid-directory");
        name = key + ".sqlite";
        pending = key + ".pending";
        lock = Fd(::openat(directory.value, (key + ".lock").c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
        checked(lock.value >= 0);
        private_file(status(lock.value));
        checked(::flock(lock.value, LOCK_EX | LOCK_NB) == 0, "source-cache-busy");
        verify_directory();
        if (recover_pending) {
            check_name(pending);
            checked(::unlinkat(directory.value, pending.c_str(), 0) == 0 || errno == ENOENT);
        }
    }
    void verify_directory() const {
        const auto current = directory_at(path);
        const auto a = status(directory.value), b = status(current.value);
        checked(a.st_dev == b.st_dev && a.st_ino == b.st_ino && b.st_uid == ::geteuid() &&
                (b.st_mode & 07777) == 0700, "source-cache-directory-changed");
        struct stat named {};
        const auto lock_name = name.substr(0, 16) + ".lock";
        checked(::fstatat(directory.value, lock_name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                same(status(lock.value), named), "source-cache-lock-changed");
    }
    void check_name(const std::string& file) const {
        struct stat s {};
        if (::fstatat(directory.value, file.c_str(), &s, AT_SYMLINK_NOFOLLOW) == 0) private_file(s);
        else checked(errno == ENOENT);
    }
    fs::path path;
    std::string name, pending;
    Fd directory, lock;
};
SourceCache::SourceCache(const fs::path& directory, const std::string& key, bool recover_pending)
    : impl_(std::make_unique<Impl>(directory, key, recover_pending)) {}
SourceCache::~SourceCache() = default;

std::optional<std::string> SourceCache::load() const {
    impl_->verify_directory();
    Fd file(::openat(impl_->directory.value, impl_->name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.value < 0) { checked(errno == ENOENT); return std::nullopt; }
    const auto before = status(file.value);
    private_file(before);
    // An oversized owned artifact is a cache miss, never an unbounded read.
    if (before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > maximum_bytes) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        checked(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    struct stat named {};
    checked(::fstatat(impl_->directory.value, impl_->name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
            same(before, status(file.value)) && same(before, named), "source-cache-file-changed");
    impl_->verify_directory();
    return bytes;
}

void SourceCache::save(std::string_view bytes) const {
    checked(bytes.size() <= maximum_bytes);
    impl_->verify_directory();
    impl_->check_name(impl_->name);
    impl_->check_name(impl_->pending);
    // Only our fixed pending slot is recoverable. Unknown files are untouched.
    checked(::unlinkat(impl_->directory.value, impl_->pending.c_str(), 0) == 0 || errno == ENOENT);
    Fd file(::openat(impl_->directory.value, impl_->pending.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    checked(file.value >= 0);
    private_file(status(file.value));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(file.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        checked(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    checked(::fsync(file.value) == 0);
    impl_->verify_directory();
    impl_->check_name(impl_->name);
    struct stat named {};
    checked(::fstatat(impl_->directory.value, impl_->pending.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
            same(status(file.value), named), "source-cache-file-changed");
    checked(::renameat(impl_->directory.value, impl_->pending.c_str(),
                       impl_->directory.value, impl_->name.c_str()) == 0);
    checked(::fsync(impl_->directory.value) == 0);
    impl_->verify_directory();
}
}
