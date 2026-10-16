#include "context_internal.h"
#include "execution_io.h"

#ifdef _WIN32
#include "shin/windows/file.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shin::context {
namespace {

#ifndef _WIN32
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
    void replace(int value) { if (value_ >= 0) ::close(value_); value_ = value; }
private:
    int value_;
};
#endif

[[noreturn]] void unavailable() {
    throw CliError("unsafe-execution-file", "filesystem",
                   "Execution input must be an accessible regular file without symlink components.",
                   1, false, "Review the explicitly selected local input path.");
}

}  // namespace

std::string read_execution_file(const fs::path& root, const std::string& relative_path,
                                std::size_t maximum_bytes) {
    if (!profile_relative_path_valid(relative_path)) unavailable();
#ifdef _WIN32
    try {
        windows::ReadFile file(root / fs::path(std::u8string(relative_path.begin(), relative_path.end())));
        return file.read(maximum_bytes);
    } catch (const std::length_error&) {
        throw_usage_error("execution-input-too-large", "Execution input exceeds its byte budget.");
    } catch (const std::exception&) { unavailable(); }
#else
    Descriptor directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) unavailable();
    const fs::path relative(relative_path);
    for (const fs::path& component : relative.parent_path()) {
        const int child = ::openat(directory.get(), component.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) unavailable();
        directory.replace(child);
    }
    Descriptor file(::openat(directory.get(), relative.filename().c_str(),
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (file.get() < 0 || ::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) unavailable();
    if (static_cast<std::uintmax_t>(status.st_size) > maximum_bytes) {
        throw_usage_error("execution-input-too-large", "Execution input exceeds its byte budget.");
    }
    const fs::path path = root / relative;
    const FileMetadata before = file_metadata(path);
    if (before.device_id != static_cast<std::uint64_t>(status.st_dev) ||
        before.file_id != static_cast<std::uint64_t>(status.st_ino) ||
        before.byte_count != static_cast<std::uintmax_t>(status.st_size)) {
        throw_file_changed_during_verification(path);
    }
    std::string content;
    content.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 65536> buffer{};
    for (;;) {
        const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) unavailable();
        if (count == 0) break;
        if (static_cast<std::size_t>(count) > maximum_bytes - content.size()) {
            throw_usage_error("execution-input-too-large", "Execution input grew beyond its byte budget.");
        }
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (profile_path_contains_symlink(root, relative_path) || file_metadata(path) != before ||
        content.size() != before.byte_count) {
        throw_file_changed_during_verification(path);
    }
    return content;
#endif
}

std::string read_execution_report_file(const fs::path& path, std::size_t maximum_bytes) {
    const fs::path absolute = fs::absolute(path);
    std::error_code error;
    const fs::path parent = fs::canonical(absolute.parent_path(), error);
    if (error) unavailable();
    return read_execution_file(parent, absolute.filename().generic_string(), maximum_bytes);
}

}  // namespace shin::context
