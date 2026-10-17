#include "shin/observer/io.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <random>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/attr.h>
#endif

#ifdef SHIN_OBSERVER_TEST_OPENAT
// Inject after system declarations: glibc may bind a renamed declaration back
// to the original assembler symbol, bypassing a command-line macro replacement.
extern "C" int shin_observer_test_openat(int directory, const char* name, int flags, ...);
extern "C" int shin_observer_test_fstatat(int directory, const char* name, struct stat* status, int flags);
#define openat shin_observer_test_openat
#define fstatat shin_observer_test_fstatat
#endif

namespace shin::observer {
namespace {

constexpr std::size_t kMaxStateBytes = kMaximumStateBytes;
constexpr char kLockName[] = "observer.lock";
constexpr char kStateName[] = "state.json";
constexpr char kPendingName[] = "state.pending";

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const { return value_; }

private:
    int value_;
};

struct FileStamp {
    dev_t device;
    ino_t inode;
    off_t size;
    mode_t mode;
    uid_t owner;
    nlink_t links;
    timespec modified;
    timespec changed;
};

[[noreturn]] void fail(const char* code, const char* message) {
    throw Error(code, message);
}

[[noreturn]] void fail_system(const char* code, const char* message, int error) {
    if (error == ENOSPC || error == EDQUOT) fail("storage-full", "Storage space or quota is exhausted.");
    if (error == EROFS) fail("storage-read-only", "Storage is mounted read-only.");
    if (error == EACCES || error == EPERM) fail("access-denied", "The operating system denied access.");
    fail(code, message);
}

timespec modified_time(const struct stat& status) {
#if defined(__APPLE__)
    return status.st_mtimespec;
#else
    return status.st_mtim;
#endif
}

timespec changed_time(const struct stat& status) {
#if defined(__APPLE__)
    return status.st_ctimespec;
#else
    return status.st_ctim;
#endif
}

FileStamp stamp(const struct stat& status) {
    return {status.st_dev, status.st_ino, status.st_size, status.st_mode, status.st_uid, status.st_nlink,
            modified_time(status), changed_time(status)};
}

bool same_stamp(const FileStamp& left, const FileStamp& right) {
    return left.device == right.device && left.inode == right.inode && left.size == right.size &&
           left.mode == right.mode && left.owner == right.owner && left.links == right.links &&
           left.modified.tv_sec == right.modified.tv_sec && left.modified.tv_nsec == right.modified.tv_nsec &&
           left.changed.tv_sec == right.changed.tv_sec && left.changed.tv_nsec == right.changed.tv_nsec;
}

bool same_inode(const FileStamp& left, const FileStamp& right) {
    return left.device == right.device && left.inode == right.inode;
}

void require_regular(const struct stat& status, bool private_only, const char* code, const char* message) {
    if (!S_ISREG(status.st_mode)) fail(code, message);
    if (private_only && (status.st_uid != ::geteuid() || (status.st_mode & 07777) != 0600 || status.st_nlink != 1)) {
        fail(code, message);
    }
}

void require_regular(const FileStamp& status, bool private_only, const char* code, const char* message) {
    if (!S_ISREG(status.mode)) fail(code, message);
    if (private_only && (status.owner != ::geteuid() || (status.mode & 07777) != 0600 || status.links != 1)) {
        fail(code, message);
    }
}

void require_private_directory(const struct stat& status, const char* code, const char* message) {
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 07777) != 0700) {
        fail(code, message);
    }
}

// st_dev identifies a mount in this boot, not the volume across boots.
std::string persistent_identity(int descriptor, const struct stat& status) {
#ifdef __APPLE__
    struct attrlist attributes{};
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_UUID;
    struct Volume { std::uint32_t length; std::array<unsigned char, 16> uuid; } volume{};
    if (::fgetattrlist(descriptor, &attributes, &volume, sizeof(volume), 0) != 0 ||
        volume.length != sizeof(volume) ||
        std::all_of(volume.uuid.begin(), volume.uuid.end(), [](unsigned char byte) { return byte == 0; }))
        fail("directory-identity-unavailable", "A persistent volume UUID is required; boot-local identity fallback is forbidden.");
    std::string result = "mac-volume-v1:";
    for (const auto byte : volume.uuid) {
        result.push_back("0123456789abcdef"[byte >> 4]);
        result.push_back("0123456789abcdef"[byte & 15]);
    }
    return result + ":" + std::to_string(static_cast<std::uintmax_t>(status.st_ino)) + ":" +
        std::to_string(status.st_birthtimespec.tv_sec) + ":" + std::to_string(status.st_birthtimespec.tv_nsec);
#else
    (void)descriptor;
    return std::to_string(static_cast<std::uintmax_t>(status.st_dev)) + ":" +
           std::to_string(static_cast<std::uintmax_t>(status.st_ino));
#endif
}

std::string persistent_identity(int descriptor) {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode))
        fail("unsafe-directory", "Cannot identify the opened directory.");
    return persistent_identity(descriptor, status);
}

fs::path normalized_absolute(const fs::path& path, const char* code, const char* message) {
    if (!path.is_absolute() || path.lexically_normal() != path) fail(code, message);
    return path;
}

std::vector<std::string> path_components(const fs::path& absolute, const char* code, const char* message) {
    std::vector<std::string> components;
    for (const fs::path& component : absolute) {
        if (component == absolute.root_name() || component == absolute.root_directory() || component == ".") continue;
        if (component == ".." || component.empty()) fail(code, message);
        components.push_back(component.string());
    }
    return components;
}

Descriptor open_directory(const fs::path& path, const char* code, const char* message) {
    const fs::path absolute = normalized_absolute(path, code, message);
    Descriptor directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) fail(code, message);
    for (const std::string& component : path_components(absolute, code, message)) {
        const int child = ::openat(directory.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) fail_system(code, message, errno);
        directory = Descriptor(child);
    }
    return directory;
}

struct ParentDirectory {
    Descriptor directory;
    std::string name;
};

ParentDirectory open_parent(const fs::path& path, const char* code, const char* message) {
    const fs::path absolute = normalized_absolute(path, code, message);
    const std::vector<std::string> components = path_components(absolute, code, message);
    if (components.empty()) fail(code, message);
    Descriptor directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) fail(code, message);
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const int child = ::openat(directory.get(), components[index].c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) fail_system(code, message, errno);
        directory = Descriptor(child);
    }
    return {std::move(directory), components.back()};
}

FileStamp named_stamp(int directory, const char* name, const char* code, const char* message) {
    struct stat status {};
    if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) fail(code, message);
    return stamp(status);
}

FileStamp descriptor_stamp(int descriptor, const char* code, const char* message) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) fail(code, message);
    return stamp(status);
}

std::string read_open_file(Descriptor file, const FileStamp& before, std::size_t maximum_bytes,
                           const char* too_large_code, const char* changed_code) {
    if (before.size < 0 || static_cast<std::uintmax_t>(before.size) > maximum_bytes) {
        fail(too_large_code, "File exceeds the configured byte limit.");
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(before.size));
    std::array<char, 65536> buffer {};
    for (;;) {
        const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail_system("file-unavailable", "File is unavailable for bounded reading.", errno);
        if (count == 0) break;
        if (static_cast<std::size_t>(count) > maximum_bytes - result.size()) {
            fail(too_large_code, "File exceeds the configured byte limit.");
        }
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (result.size() != static_cast<std::size_t>(before.size) ||
        !same_stamp(before, descriptor_stamp(file.get(), changed_code, "File changed during reading."))) {
        fail(changed_code, "File changed during reading.");
    }
    return result;
}

std::string read_named_file(int directory, const char* name, std::size_t maximum_bytes, bool private_only,
                            const char* unsafe_code, const char* too_large_code, const char* changed_code) {
    const FileStamp named_before = named_stamp(directory, name, unsafe_code, "File is not a safe regular file.");
    // A named stat can resolve an inode just before an atomic rename unlinks it.
    // Retry only a regular, otherwise safe inode; never accept its stale bytes.
    if (named_before.links == 0 && S_ISREG(named_before.mode) &&
        (!private_only || (named_before.owner == ::geteuid() && (named_before.mode & 07777) == 0600)))
        fail(changed_code, "File changed during reading.");
    require_regular(named_before, private_only, unsafe_code, "File is not a safe regular file.");
    Descriptor file(::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
    if (file.get() < 0) fail_system(unsafe_code, "File is not a safe regular file.", errno);
    const FileStamp descriptor_before = descriptor_stamp(file.get(), unsafe_code, "File is not a safe regular file.");
    // An atomic replacement can unlink the opened inode before fstat observes it.
    if (!same_stamp(named_before, descriptor_before)) fail(changed_code, "File changed during reading.");
    require_regular(descriptor_before, private_only, unsafe_code, "File is not a safe regular file.");
    std::string result = read_open_file(std::move(file), descriptor_before, maximum_bytes, too_large_code, changed_code);
    const FileStamp named_after = named_stamp(directory, name, changed_code, "File changed during reading.");
    if (!same_stamp(named_before, named_after)) fail(changed_code, "File changed during reading.");
    return result;
}

void fsync_directory(int directory) {
    if (::fsync(directory) != 0) fail_system("state-sync-failed", "State directory could not be synchronized.", errno);
}

void write_all(int descriptor, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail_system("state-write-failed", "State file could not be written.", errno);
        if (count == 0) fail("state-write-failed", "State file could not be written.");
        offset += static_cast<std::size_t>(count);
    }
}

bool missing_name(int directory, const char* name) {
    struct stat status {};
    if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) == 0) return false;
    if (errno == ENOENT) return true;
    fail("state-unsafe", "State directory contains an unsafe state entry.");
}

void validate_existing_private_file(int directory, const char* name, bool allow_missing) {
    struct stat status {};
    if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (allow_missing && errno == ENOENT) return;
        fail("state-unsafe", "State directory contains an unsafe state entry.");
    }
    require_regular(status, true, "state-unsafe", "State directory contains an unsafe state entry.");
}

} // namespace

std::string read_text(const fs::path& path, std::size_t maximum_bytes, bool private_only) {
    ParentDirectory parent = open_parent(path, "unsafe-file", "File path is not an existing non-symlink path.");
    return read_named_file(parent.directory.get(), parent.name.c_str(), maximum_bytes, private_only,
                           "unsafe-file", "file-too-large", "file-changed");
}

fs::path current_executable_path() {
    std::array<char, 4096> path{};
#ifdef __APPLE__
    std::uint32_t size = path.size();
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        throw Error("installation-unavailable", "Cannot locate the native installation.");
#else
    const auto count = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (count < 0 || static_cast<std::size_t>(count) >= path.size() - 1)
        throw Error("installation-unavailable", "Cannot locate the native installation.");
#endif
    std::error_code error;
    const auto canonical = fs::canonical(path.data(), error);
    if (error) throw Error("installation-unavailable", "The executing image path is no longer available; restart with one matched kit.");
    return canonical;
}

std::string executing_image_identity() {
#ifdef __APPLE__
    const auto* header = _dyld_get_image_header(0);
    if (!header || header->magic != MH_MAGIC_64)
        throw Error("installation-unavailable", "Cannot identify the executing image.");
    const auto* cursor = reinterpret_cast<const unsigned char*>(header) + sizeof(mach_header_64);
    std::size_t remaining = header->sizeofcmds;
    for (std::uint32_t i = 0; i < header->ncmds && remaining >= sizeof(load_command); ++i) {
        const auto* command = reinterpret_cast<const load_command*>(cursor);
        if (command->cmdsize < sizeof(load_command) || command->cmdsize > remaining) break;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(uuid_command)) {
            const auto* uuid = reinterpret_cast<const uuid_command*>(command);
            std::string result = "macho-uuid:";
            for (const auto byte : uuid->uuid) {
                result += "0123456789abcdef"[byte >> 4];
                result += "0123456789abcdef"[byte & 15];
            }
            return result;
        }
        remaining -= command->cmdsize;
        cursor += command->cmdsize;
    }
    throw Error("installation-unavailable", "The executing image has no build identity.");
#else
    // This kernel link refers to the running inode, even after path replacement.
    const Descriptor file(::open("/proc/self/exe", O_RDONLY | O_CLOEXEC));
    struct stat status{};
    if (file.get() < 0 || ::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode))
        throw Error("installation-unavailable", "Cannot identify the executing image.");
    return "linux-exe:" + std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino) + ":" + std::to_string(status.st_size);
#endif
}

std::string directory_identity(const fs::path& path, bool private_only) {
    Descriptor directory = open_directory(path, "unsafe-directory", "Directory is not an existing non-symlink directory.");
    struct stat status {};
    if (::fstat(directory.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        fail("unsafe-directory", "Directory is not an existing non-symlink directory.");
    }
    if (private_only) require_private_directory(status, "unsafe-directory", "Directory is not a private directory.");
    return persistent_identity(directory.get(), status);
}

void ensure_private_directory(const fs::path& path, bool exclusive) {
    auto parent = open_parent(path, "unsafe-directory", "Private directory parent is unavailable.");
    const auto before = descriptor_stamp(parent.directory.get(), "unsafe-directory", "Parent is unavailable.");
    if (::mkdirat(parent.directory.get(), parent.name.c_str(), 0700) != 0 && (exclusive || errno != EEXIST))
        fail_system("directory-create-failed", "Private directory could not be created.", errno);
    Descriptor child(::openat(parent.directory.get(), parent.name.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (child.get() < 0) fail("unsafe-directory", "Private directory is unsafe.");
    const auto held = descriptor_stamp(child.get(), "unsafe-directory", "Private directory is unavailable.");
    if (!S_ISDIR(held.mode) || held.owner != ::geteuid() || (held.mode & 07777) != 0700)
        fail("unsafe-directory", "Existing directory must be owned by this user with mode 0700.");
    fsync_directory(parent.directory.get());
    auto current_parent = open_directory(path.parent_path(), "directory-changed", "Parent changed.");
    if (!same_inode(before, descriptor_stamp(current_parent.get(), "directory-changed", "Parent changed.")) ||
        directory_identity(path, true) != persistent_identity(child.get()))
        fail("directory-changed", "Private directory changed during creation.");
}

namespace {
bool write_archive(const fs::path& path, const std::string& content, bool private_parent) {
    if (content.size() > kMaxStateBytes) fail("archive-too-large", "Archive exceeds the bounded state limit.");
    ParentDirectory parent = open_parent(path, "unsafe-file", "Archive path is not an existing non-symlink path.");
    const auto identity = directory_identity(path.parent_path(), private_parent);
    const auto held_parent = descriptor_stamp(parent.directory.get(), "unsafe-directory", "Archive directory is unavailable.");
    if (held_parent.owner != ::geteuid() || (held_parent.mode & 0022) != 0 ||
        (private_parent && (held_parent.mode & 07777) != 0700) ||
        identity != persistent_identity(parent.directory.get()))
        fail("unsafe-directory", "Archive directory is not private or changed before export.");
    Descriptor file(::openat(parent.directory.get(), parent.name.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0600));
    const bool created = file.get() >= 0;
    if (!created) {
        if (errno != EEXIST) fail("archive-write-failed", "Archive file could not be created.");
        if (read_named_file(parent.directory.get(), parent.name.c_str(), kMaxStateBytes, true,
                            "unsafe-file", "file-too-large", "file-changed") != content)
            fail("archive-conflict", "Existing archive differs; preserve it and choose a new archive file.");
        file = Descriptor(::openat(parent.directory.get(), parent.name.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
        if (file.get() < 0) fail("unsafe-file", "Existing archive is unavailable.");
    }
    const auto before = descriptor_stamp(file.get(), "unsafe-file", "Archive file is unavailable.");
    require_regular(before, true, "unsafe-file", "Archive file is not private.");
    // A failed or interrupted exclusive create may leave an incomplete copy,
    // never a replacement of the original state or an acknowledged archive.
    if (created) write_all(file.get(), content);
    if (::fsync(file.get()) != 0) fail("archive-sync-failed", "Archive file could not be synchronized.");
    const auto after = descriptor_stamp(file.get(), "unsafe-file", "Archive file changed during export.");
    const auto named = named_stamp(parent.directory.get(), parent.name.c_str(), "unsafe-file", "Archive file changed during export.");
    require_regular(after, true, "unsafe-file", "Archive file is not private.");
    if (!same_inode(before, after) || !same_stamp(after, named) ||
        read_named_file(parent.directory.get(), parent.name.c_str(), kMaxStateBytes, true,
                        "unsafe-file", "file-too-large", "file-changed") != content)
        fail("archive-changed", "Archive changed before verification.");
    fsync_directory(parent.directory.get());
    const auto current_parent = open_directory(path.parent_path(), "unsafe-directory", "Archive directory changed.");
    if (!same_inode(held_parent, descriptor_stamp(current_parent.get(), "unsafe-directory", "Archive directory changed.")) ||
        directory_identity(path.parent_path(), private_parent) != identity)
        fail("directory-changed", "Archive directory changed during export.");
    return created;
}
}

bool write_private_archive(const fs::path& path, const std::string& content) { return write_archive(path, content, true); }
bool write_user_archive(const fs::path& path, const std::string& content) { return write_archive(path, content, false); }

void replace_user_archive(const fs::path& path, const std::string& expected_content,
                          const std::string& content, const std::string& parent_identity) {
    if (content.size() > kMaxStateBytes) fail("archive-too-large", "Replacement exceeds the bounded state limit.");
    auto parent = open_parent(path, "unsafe-file", "Archive parent is unavailable.");
    const auto held = descriptor_stamp(parent.directory.get(), "unsafe-directory", "Archive parent is unavailable.");
    const auto identity = persistent_identity(parent.directory.get());
    if (held.owner != ::geteuid() || (held.mode & 0022) || identity != parent_identity ||
        directory_identity(path.parent_path()) != identity)
        fail("unsafe-directory", "Archive parent ownership or identity changed.");
    const auto before = named_stamp(parent.directory.get(), parent.name.c_str(), "unsafe-file", "Archive is unavailable.");
    require_regular(before, true, "unsafe-file", "Archive must be private and singly linked.");
    const auto current = read_named_file(parent.directory.get(), parent.name.c_str(), kMaxStateBytes, true,
                                         "unsafe-file", "file-too-large", "file-changed");
    if (current != expected_content && current != content)
        fail("archive-conflict", "Changed archive must not be replaced.");
    if (current == content) { fsync_directory(parent.directory.get()); return; }
    std::random_device random;
    const auto temporary = ".shin-agent-" + std::to_string(random()) + "-" + std::to_string(random()) + ".tmp";
    Descriptor staged(::openat(parent.directory.get(), temporary.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (staged.get() < 0) fail("archive-write-failed", "Cannot stage the replacement.");
    struct Cleanup {
        int directory; int file; const std::string& name;
        ~Cleanup() {
            struct stat held{}, named{};
            if (::fstat(file, &held) == 0 && ::fstatat(directory, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                held.st_dev == named.st_dev && held.st_ino == named.st_ino)
                ::unlinkat(directory, name.c_str(), 0);
        }
    } cleanup{parent.directory.get(), staged.get(), temporary};
    write_all(staged.get(), content);
    if (::fsync(staged.get()) != 0) fail("archive-sync-failed", "Cannot synchronize the replacement.");
    const auto written = descriptor_stamp(staged.get(), "unsafe-file", "Staged file changed.");
    require_regular(written, true, "unsafe-file", "Staged file is not private.");
    if (!same_stamp(written, named_stamp(parent.directory.get(), temporary.c_str(), "file-changed", "Staged file changed.")) ||
        !same_stamp(before, named_stamp(parent.directory.get(), parent.name.c_str(), "file-changed", "Archive changed.")) ||
        directory_identity(path.parent_path()) != identity)
        fail("file-changed", "Archive changed before replacement.");
    if (::renameat(parent.directory.get(), temporary.c_str(), parent.directory.get(), parent.name.c_str()) != 0)
        fail("archive-rename-failed", "Cannot publish the replacement.");
    fsync_directory(parent.directory.get());
    if (directory_identity(path.parent_path()) != identity)
        fail("directory-changed", "Archive parent changed during replacement.");
}

void remove_user_archive(const fs::path& path, const std::string& expected_content, std::size_t maximum_bytes) {
    if (maximum_bytes > 256U * 1024U * 1024U) fail("archive-too-large", "Removal exceeds its bounded limit.");
    ParentDirectory parent = open_parent(path, "unsafe-file", "Archive parent is unavailable.");
    const auto identity = directory_identity(path.parent_path());
    const auto held = descriptor_stamp(parent.directory.get(), "unsafe-directory", "Archive parent is unavailable.");
    if (held.owner != ::geteuid() || (held.mode & 0022) != 0)
        fail("unsafe-directory", "Archive parent must be owned by this user and not writable by others.");
    const auto before = named_stamp(parent.directory.get(), parent.name.c_str(), "unsafe-file", "Archive is unavailable.");
    require_regular(before, true, "unsafe-file", "Archive must be a private regular file.");
    if (read_named_file(parent.directory.get(), parent.name.c_str(), maximum_bytes, true,
                        "unsafe-file", "file-too-large", "file-changed") != expected_content)
        fail("archive-conflict", "Changed archive must not be removed.");
    const auto current = named_stamp(parent.directory.get(), parent.name.c_str(), "file-changed", "Archive changed.");
    if (!same_stamp(before, current) || directory_identity(path.parent_path()) != identity)
        fail("file-changed", "Archive changed before removal.");
    if (::unlinkat(parent.directory.get(), parent.name.c_str(), 0) != 0)
        fail("archive-remove-failed", "Owned archive could not be removed.");
    fsync_directory(parent.directory.get());
}

class StateStore::Impl {
public:
    Impl(fs::path requested_directory, bool requested_writer, const std::string& expected_identity, bool create_lock)
        : path(normalized_absolute(requested_directory, "unsafe-directory", "State directory is invalid.")),
          directory(open_directory(path, "unsafe-directory", "State directory is invalid.")), writer(requested_writer) {
        const struct stat status = directory_status();
        require_private_directory(status, "unsafe-directory", "State directory is not private.");
        directory_before = stamp(status);
        const auto identity = persistent_identity(directory.get(), status);
        if (!expected_identity.empty() && identity != expected_identity)
            fail("directory-changed", "State directory binding changed before opening.");
        if (!writer) return;

        const int flags = O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW;
        // Separate atomic creation from opening an existing lock. Concurrent
        // first users must all lock the same inode, never replace its name.
        if (create_lock) {
            lock = Descriptor(::openat(directory.get(), kLockName, flags | O_CREAT | O_EXCL, 0600));
            if (lock.get() < 0 && errno == EEXIST)
                lock = Descriptor(::openat(directory.get(), kLockName, flags));
        } else lock = Descriptor(::openat(directory.get(), kLockName, flags));
        if (lock.get() < 0) fail("writer-unavailable", "Observer writer lock is unavailable.");
        const FileStamp lock_before = descriptor_stamp(lock.get(), "writer-unavailable", "Observer writer lock is unavailable.");
        require_regular(lock_before, true, "writer-unavailable", "Observer writer lock is unsafe.");
        if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
            fail("writer-busy", "Observer writer lock is already held.");
        }
        lock_before_ = lock_before;
        stable();
    }

    void stable() const {
        const struct stat held_status = directory_status();
        require_private_directory(held_status, "directory-changed", "State directory changed during the operation.");
        if (!same_inode(directory_before, stamp(held_status))) {
            fail("directory-changed", "State directory changed during the operation.");
        }
        Descriptor current = open_directory(path, "directory-changed", "State directory changed during the operation.");
        const FileStamp current_stamp = descriptor_stamp(current.get(), "directory-changed", "State directory changed during the operation.");
        if (!same_inode(directory_before, current_stamp)) {
            fail("directory-changed", "State directory changed during the operation.");
        }
        if (writer) {
            const FileStamp held_lock = descriptor_stamp(lock.get(), "lock-changed", "Observer writer lock changed during the operation.");
            const FileStamp named_lock = named_stamp(directory.get(), kLockName, "lock-changed", "Observer writer lock changed during the operation.");
            if (!same_stamp(lock_before_, held_lock) || !same_stamp(lock_before_, named_lock)) {
                fail("lock-changed", "Observer writer lock changed during the operation.");
            }
            require_regular(held_lock, true, "lock-changed", "Observer writer lock changed during the operation.");
        }
    }

    void recover_pending() {
        stable();
        struct stat pending {};
        if (::fstatat(directory.get(), kPendingName, &pending, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) return;
            fail("state-unsafe", "State directory contains an unsafe state entry.");
        }
        require_regular(pending, true, "state-unsafe", "State directory contains an unsafe state entry.");
        if (pending.st_size < 0 || static_cast<std::uintmax_t>(pending.st_size) > kMaxStateBytes) {
            fail("state-too-large", "Pending state exceeds the bounded limit.");
        }
        const FileStamp pending_before = stamp(pending);
        const FileStamp named_pending = named_stamp(directory.get(), kPendingName,
                                                    "state-unsafe", "State directory contains an unsafe state entry.");
        if (!same_stamp(pending_before, named_pending)) {
            fail("state-changed", "Pending state changed during recovery.");
        }
        if (::unlinkat(directory.get(), kPendingName, 0) != 0) {
            fail("state-recovery-failed", "Pending state could not be recovered.");
        }
        fsync_directory(directory.get());
        recovered = true;
        stable();
    }

    struct stat directory_status() const {
        struct stat status {};
        if (::fstat(directory.get(), &status) != 0) fail("directory-changed", "State directory changed during the operation.");
        return status;
    }

    fs::path path;
    Descriptor directory;
    Descriptor lock;
    FileStamp directory_before {};
    FileStamp lock_before_ {};
    bool writer;
    bool recovered = false;
};

StateStore::StateStore(fs::path directory, bool writer, const std::string& expected_identity, bool create_lock)
    : impl_(std::make_unique<Impl>(std::move(directory), writer, expected_identity, create_lock)) {}
StateStore::~StateStore() = default;

std::optional<Json> StateStore::load() const {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        impl_->stable();
        if (missing_name(impl_->directory.get(), kStateName)) return std::nullopt;
        try {
            const std::string content = read_named_file(impl_->directory.get(), kStateName, kMaxStateBytes, true,
                                                        "state-unsafe", "state-too-large", "state-changed");
            impl_->stable();
            return parse_json(content);
        } catch (const Error& error) {
            if (error.code != "file-changed" && error.code != "state-changed") throw;
        }
    }
    fail("state-changed", "State changed during three bounded read attempts.");
}

void StateStore::save(const Json& state) {
    if (!impl_->writer) fail("writer-required", "State saving requires the observer writer lock.");
    impl_->stable();
    std::string content;
    try {
        content = state.dump();
    } catch (const std::exception&) {
        fail("invalid-state", "State cannot be serialized.");
    }
    if (content.size() > kMaxStateBytes) fail("state-too-large", "State exceeds the bounded limit.");
    validate_existing_private_file(impl_->directory.get(), kStateName, true);
    if (!missing_name(impl_->directory.get(), kPendingName)) {
        validate_existing_private_file(impl_->directory.get(), kPendingName, false);
        fail("state-pending-exists", "Pending state must be recovered before saving.");
    }

    Descriptor pending(::openat(impl_->directory.get(), kPendingName,
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0600));
    if (pending.get() < 0) fail_system("state-write-failed", "Pending state could not be created.", errno);
    const FileStamp pending_before = descriptor_stamp(pending.get(), "state-write-failed", "Pending state could not be created.");
    require_regular(pending_before, true, "state-write-failed", "Pending state is not private.");
    write_all(pending.get(), content);
    if (::fsync(pending.get()) != 0) fail_system("state-sync-failed", "Pending state could not be synchronized.", errno);
    impl_->stable();
    validate_existing_private_file(impl_->directory.get(), kStateName, true);
    const FileStamp pending_after = descriptor_stamp(pending.get(), "state-write-failed", "Pending state changed before commit.");
    const FileStamp named_pending = named_stamp(impl_->directory.get(), kPendingName,
                                                "state-write-failed", "Pending state changed before commit.");
    require_regular(pending_after, true, "state-write-failed", "Pending state is not private.");
    if (!same_inode(pending_before, pending_after) || !same_inode(pending_before, named_pending)) {
        fail("state-changed", "Pending state changed before commit.");
    }
    require_regular(named_pending, true, "state-write-failed", "Pending state is not private.");
    if (::renameat(impl_->directory.get(), kPendingName, impl_->directory.get(), kStateName) != 0) {
        fail_system("state-rename-failed", "State could not be committed atomically.", errno);
    }
    fsync_directory(impl_->directory.get());
    impl_->stable();
    validate_existing_private_file(impl_->directory.get(), kStateName, false);
}

bool StateStore::writer_present() const {
    impl_->stable();
    if (impl_->writer) return true;
    struct stat status {};
    if (::fstatat(impl_->directory.get(), kLockName, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return false;
        fail("lock-unavailable", "Observer writer lock is unavailable.");
    }
    require_regular(status, true, "lock-unavailable", "Observer writer lock is unsafe.");
    Descriptor probe(::openat(impl_->directory.get(), kLockName, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
    if (probe.get() < 0) fail("lock-unavailable", "Observer writer lock is unavailable.");
    const FileStamp named = stamp(status);
    const FileStamp opened = descriptor_stamp(probe.get(), "lock-unavailable", "Observer writer lock is unavailable.");
    if (!same_stamp(named, opened)) fail("lock-changed", "Observer writer lock changed during probing.");
    if (::flock(probe.get(), LOCK_EX | LOCK_NB) == 0) {
        ::flock(probe.get(), LOCK_UN);
        return false;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) return true;
    fail("lock-unavailable", "Observer writer lock is unavailable.");
}

bool StateStore::recovered_pending() const {
    impl_->stable();
    return impl_->recovered;
}

void StateStore::recover_pending() {
    if (!impl_->writer) fail("writer-required", "State recovery requires the observer writer lock.");
    impl_->recover_pending();
}

} // namespace shin::observer
