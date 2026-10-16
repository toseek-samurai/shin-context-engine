#include "shin/server/route_files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shin::server {
namespace {
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
private:
    int value_;
};

[[noreturn]] void unavailable() {
    throw ServiceError(409, "route-input-unavailable", "A configured route input is unavailable or changed.");
}

bool same_file(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
    const auto mt_before = before.st_mtimespec;
    const auto mt_after = after.st_mtimespec;
    const auto ct_before = before.st_ctimespec;
    const auto ct_after = after.st_ctimespec;
#else
    const auto mt_before = before.st_mtim;
    const auto mt_after = after.st_mtim;
    const auto ct_before = before.st_ctim;
    const auto ct_after = after.st_ctim;
#endif
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size && before.st_mode == after.st_mode &&
           mt_before.tv_sec == mt_after.tv_sec && mt_before.tv_nsec == mt_after.tv_nsec &&
           ct_before.tv_sec == ct_after.tv_sec && ct_before.tv_nsec == ct_after.tv_nsec;
}
} // namespace

std::string read_route_file(const std::filesystem::path& path, std::uintmax_t maximum_bytes) {
    std::error_code error;
    if (!path.is_absolute() || std::filesystem::canonical(path, error) != path || error) unavailable();
    Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat before{};
    if (file.get() < 0 || ::fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > maximum_bytes) unavailable();
    std::string result(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < result.size()) {
        const auto count = ::read(file.get(), result.data() + offset, result.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) unavailable();
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    struct stat after{};
    if (::read(file.get(), &extra, 1) != 0 || ::fstat(file.get(), &after) != 0 || !same_file(before, after))
        unavailable();
    return result;
}

std::string route_directory_identity(const std::filesystem::path& path) {
    std::error_code error;
    if (!path.is_absolute() || std::filesystem::canonical(path, error) != path || error) unavailable();
    Descriptor directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat identity{};
    if (directory.get() < 0 || ::fstat(directory.get(), &identity) != 0 || !S_ISDIR(identity.st_mode)) unavailable();
    return std::to_string(static_cast<std::uintmax_t>(identity.st_dev)) + ":" +
           std::to_string(static_cast<std::uintmax_t>(identity.st_ino));
}

} // namespace shin::server
