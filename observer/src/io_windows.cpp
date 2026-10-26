#include "shin/observer/io.h"
#include "shin/windows/file.h"
#include <windows.h>
#include <system_error>

namespace shin::observer {
namespace {
using Directory = windows::Directory;
using Access = Directory::Access;
template<class Action> auto io(Action&& action, const char* unsafe, const char* changed = "file-changed",
                               const char* large = "file-too-large") {
    try { return action(); }
    catch (const Error&) { throw; }
    catch (const windows::Changed&) { throw Error(changed, "Private path or file changed during the operation."); }
    catch (const std::length_error&) { throw Error(large, "Private file exceeds its byte limit."); }
    catch (const std::system_error& error) {
        if (error.code().category() != std::system_category()) throw Error(unsafe, "Private path is unavailable or unsafe.");
        const auto code = error.code().value();
        if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL)
            throw Error("storage-full", "Storage space or quota is exhausted.");
        if (code == ERROR_WRITE_PROTECT) throw Error("storage-read-only", "Storage is read-only.");
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
            throw Error(changed, "A concurrent file operation excludes this request.");
        throw Error(unsafe, "Private path is unavailable or unsafe.");
    }
    catch (const std::runtime_error&) { throw Error(unsafe, "Private path is unavailable or unsafe."); }
}
std::string name(const fs::path& path) {
    if (!path.is_absolute() || path.lexically_normal() != path)
        throw Error("unsafe-file", "An absolute normalized local path is required.");
    const auto utf8 = path.filename().u8string();
    return std::string(utf8.begin(), utf8.end());
}
}
std::string read_text(const fs::path& path, std::size_t maximum, bool private_only) {
    return io([&] {
        if (!private_only) { windows::ReadFile file(path); return file.read(maximum); }
        const auto leaf = name(path);
        windows::Directory parent(path.parent_path());
        auto value = parent.read(leaf, maximum);
        if (!value) throw Error("unsafe-file", "File is unavailable.");
        return *value;
    }, "unsafe-file");
}
std::string directory_identity(const fs::path& path, bool private_only) {
    return io([&] { return Directory(path, private_only ? Access::private_owner : Access::read_only).identity(); }, "unsafe-directory", "directory-changed");
}
void ensure_private_directory(const fs::path& path, bool exclusive) {
    io([&] { windows::Directory::ensure_private(path, exclusive); }, "unsafe-directory", "directory-changed");
}
fs::path current_executable_path() {
    return io([] { return windows::running_image_path(); }, "installation-unavailable", "installation-unavailable");
}
std::string executing_image_identity() {
    return io([] { return windows::running_image_identity(); }, "installation-unavailable", "installation-unavailable");
}
bool write_private_archive(const fs::path& path, const std::string& content) {
    if (content.size() > kMaximumStateBytes) throw Error("archive-too-large", "Archive exceeds its byte limit.");
    return io([&] {
        const auto leaf = name(path);
        Directory parent(path.parent_path(), Access::private_owner);
        const auto old = parent.read(leaf, kMaximumStateBytes);
        if (old && *old != content) throw Error("archive-conflict", "Existing archive differs; no overwrite.");
        return parent.create(leaf, content);
    }, "unsafe-file", "archive-conflict");
}
bool write_user_archive(const fs::path& path, const std::string& content) {
    if (content.size() > kMaximumStateBytes) throw Error("archive-too-large", "Archive exceeds its byte limit.");
    const auto leaf = name(path);
    const auto parent = io([&] { return std::make_unique<Directory>(path.parent_path(), Access::user_owned); }, "unsafe-directory");
    return io([&] {
        const auto old = parent->read(leaf, kMaximumStateBytes);
        if (old && *old != content) throw Error("archive-conflict", "Existing archive differs; no overwrite.");
        return parent->create(leaf, content);
    }, "unsafe-file", "archive-conflict");
}
void remove_user_archive(const fs::path& path, const std::string& expected, std::size_t maximum_bytes) {
    if (maximum_bytes > 256U * 1024U * 1024U) throw Error("archive-too-large", "Removal exceeds its bounded limit.");
    const auto leaf = name(path);
    const auto parent = io([&] { return std::make_unique<Directory>(path.parent_path(), Access::user_owned); }, "unsafe-directory");
    io([&] {
        if (!parent->remove(leaf, maximum_bytes, expected)) throw Error("unsafe-file", "Archive is unavailable.");
    }, "unsafe-file", "archive-conflict");
}
void replace_user_archive(const fs::path& path, const std::string& expected, const std::string& content, const std::string& identity) {
    if (content.size() > kMaximumStateBytes) throw Error("archive-too-large", "Replacement exceeds its byte limit.");
    const auto leaf = name(path);
    const auto parent = io([&] { return std::make_unique<Directory>(path.parent_path(), Access::user_owned); }, "unsafe-directory");
    io([&] {
        if (parent->identity() != identity) throw Error("unsafe-directory", "Archive parent identity changed.");
    }, "unsafe-directory");
    io([&] { parent->replace(leaf, expected, content, kMaximumStateBytes); }, "unsafe-file", "archive-conflict");
}

void replace_user_config(const fs::path& path, const std::string& expected, const std::string& content, const std::string& identity) {
    // Profile relocation owns a private configuration. Keep that exact ACL
    // contract; automatic editing of broader client ACLs is not enabled here.
    replace_user_archive(path, expected, content, identity);
}

class StateStore::Impl {
public:
    std::unique_ptr<windows::Directory> directory;
    std::unique_ptr<windows::WriterLock> lock;
    bool recovered = false;
    Impl(const fs::path& path, bool writer, const std::string& expected, bool create) {
        directory = io([&] { return std::make_unique<Directory>(path, Access::private_owner); }, "unsafe-directory");
        if (!expected.empty() && directory->identity() != expected)
            throw Error("directory-changed", "State directory binding changed before opening.");
        if (writer) lock = io([&] { return directory->writer("observer.lock", create); }, "writer-unavailable", "writer-busy");
    }
    void stable() const {
        io([&] { directory->check(); }, "directory-changed", "directory-changed");
        if (lock) io([&] { lock->check(); }, "lock-changed", "lock-changed");
    }
    std::optional<std::string> read(const char* name) const {
        stable();
        return io([&] { return directory->read(name, kMaximumStateBytes); }, "state-unsafe", "state-changed", "state-too-large");
    }
};
StateStore::StateStore(fs::path directory, bool writer, const std::string& expected, bool create)
    : impl_(std::make_unique<Impl>(directory, writer, expected, create)) {}
StateStore::~StateStore() = default;
std::optional<Json> StateStore::load() const {
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            const auto bytes = impl_->read("state.json");
            impl_->stable();
            return bytes ? std::optional<Json>(parse_json(*bytes)) : std::nullopt;
        } catch (const Error& error) { if (error.code != "state-changed") throw; }
    }
    throw Error("state-changed", "State changed during three bounded read attempts.");
}
void StateStore::save(const Json& value) {
    if (!impl_->lock) throw Error("writer-required", "State saving requires the writer lock.");
    impl_->stable();
    std::string content;
    try { content = value.dump(); } catch (const std::exception&) { throw Error("invalid-state", "State cannot be serialized."); }
    if (content.size() > kMaximumStateBytes) throw Error("state-too-large", "State exceeds its byte limit.");
    if (impl_->read("state.pending")) throw Error("state-pending-exists", "Pending state must be explicitly recovered before saving.");
    io([&] { impl_->directory->publish("state.json", "state.pending", content); }, "state-unsafe", "state-changed", "state-too-large");
    impl_->stable();
}
void StateStore::recover_pending() {
    if (!impl_->lock) throw Error("writer-required", "State recovery requires the writer lock.");
    impl_->stable();
    if (io([&] { return impl_->directory->remove("state.pending", kMaximumStateBytes); },
           "state-unsafe", "state-changed", "state-too-large")) impl_->recovered = true;
    impl_->stable();
}
bool StateStore::recovered_pending() const { impl_->stable(); return impl_->recovered; }
bool StateStore::writer_present() const {
    impl_->stable();
    return impl_->lock || io([&] { return impl_->directory->writer_present("observer.lock"); }, "lock-unavailable", "lock-changed");
}
}
