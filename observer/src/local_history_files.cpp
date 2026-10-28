#include "shin/observer/local_history_files.h"
#include <sqlite3.h>
#ifdef _WIN32
#include "shin/windows/file.h"
#include <windows.h>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shin::observer {
namespace {
constexpr std::size_t maximum = 32U * 1024U * 1024U;
void require(bool value, const char* code) { if (!value) throw Error(code, "Local history files are unavailable or unsafe."); }
#ifdef _WIN32
struct HistoryLease {
    shin::windows::Directory directory;
    explicit HistoryLease(const fs::path& path) : directory(path, shin::windows::Directory::Access::private_owner) {}
};
std::mutex leases_mutex;
std::map<fs::path, HistoryLease*> leases;
sqlite3_vfs* original_vfs = nullptr;
sqlite3_vfs history_vfs{};
std::once_flag registration;
constexpr const char* vfs_name = "shin-private-history-win-v1";
template<class Action> auto journal_operation(Action action) {
    // A deleted journal can retain its name until the last metadata handle
    // closes. Reopen and revalidate it; never adopt unsafe ACLs or ownership.
    for (int attempt = 0;; ++attempt) {
        try { return action(); }
        catch (const shin::windows::Changed&) { if (attempt == 7) throw; }
        catch (const std::system_error& error) {
            if (attempt == 7 || (error.code().value() != ERROR_ACCESS_DENIED &&
                error.code().value() != ERROR_DELETE_PENDING)) throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
int private_open(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* out_flags) noexcept {
    file->pMethods = nullptr;
    try {
        if (!name || !(flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL))) {
            sqlite3_log(SQLITE_CANTOPEN, "private history VFS: unsupported file kind, flags=%d", flags);
            return SQLITE_CANTOPEN;
        }
        const std::string_view utf8(name);
        const auto path = fs::absolute(fs::path(std::u8string(utf8.begin(), utf8.end()))).lexically_normal();
        const std::lock_guard lock(leases_mutex);
        const auto found = leases.find(path.parent_path());
        if (found == leases.end()) {
            sqlite3_log(SQLITE_CANTOPEN, "private history VFS: directory lease unavailable");
            return SQLITE_CANTOPEN;
        }
        const auto leaf = path.filename().string();
        if (leaf != "telemetry.sqlite" && leaf != "telemetry.sqlite-journal") return SQLITE_CANTOPEN;
        auto& directory = found->second->directory;
        const auto open = [&] {
            int native_flags = flags;
            if (flags & SQLITE_OPEN_CREATE) {
                const bool created = directory.create_empty_file(leaf);
                if ((flags & SQLITE_OPEN_EXCLUSIVE) && !created) return SQLITE_CANTOPEN;
                if (created) native_flags &= ~SQLITE_OPEN_EXCLUSIVE;
            }
            if (!directory.metadata(leaf, maximum)) {
                if (leaf == "telemetry.sqlite-journal") throw shin::windows::Changed();
                return SQLITE_CANTOPEN;
            }
            // SQLite's native Windows VFS still owns locks, journals, flushes and IO.
            return original_vfs->xOpen(original_vfs, name, file, native_flags, out_flags);
        };
        return leaf == "telemetry.sqlite-journal" ? journal_operation(open) : open();
    } catch (const std::system_error& error) {
        sqlite3_log(SQLITE_CANTOPEN, "private history VFS: file operation refused, system=%d", error.code().value());
        return SQLITE_CANTOPEN;
    } catch (...) {
        sqlite3_log(SQLITE_CANTOPEN, "private history VFS: file metadata refused");
        return SQLITE_CANTOPEN;
    }
}
void register_history_vfs() {
    std::call_once(registration, [] {
        original_vfs = sqlite3_vfs_find("win32-longpath");
        require(original_vfs != nullptr, "history-vfs-unavailable");
        history_vfs = *original_vfs;
        history_vfs.zName = vfs_name;
        history_vfs.xOpen = private_open;
        require(sqlite3_vfs_register(&history_vfs, 0) == SQLITE_OK, "history-vfs-unavailable");
    });
}
#else
struct stat stamp(const fs::path& path, bool optional = false) {
    struct stat value{};
    if (::lstat(path.c_str(), &value) != 0) {
        require(optional && errno == ENOENT, "history-file-unsafe"); return value;
    }
    require(S_ISREG(value.st_mode) && value.st_uid == ::geteuid() && (value.st_mode & 0777) == 0600 &&
        value.st_nlink == 1 && value.st_size >= 0 && static_cast<std::uintmax_t>(value.st_size) <= maximum, "history-file-unsafe");
    return value;
}
#endif
}
class LocalHistoryFiles::Impl {
public:
    fs::path directory, file;
    std::string identity;
    bool created = false;
#ifdef _WIN32
    HistoryLease lease;
    shin::windows::FileStamp initial;
    explicit Impl(const fs::path& path) : directory(path), file(path / "telemetry.sqlite"), identity(directory_identity(path, true)), lease(path) {
        register_history_vfs();
        created = lease.directory.create_empty_file("telemetry.sqlite");
        initial = lease.directory.metadata("telemetry.sqlite", maximum).value();
        check();
        const std::lock_guard lock(leases_mutex);
        require(leases.emplace(fs::absolute(path).lexically_normal(), &lease).second, "history-writer-busy");
    }
    ~Impl() { const std::lock_guard lock(leases_mutex); leases.erase(fs::absolute(directory).lexically_normal()); }
    void check() const {
        require(directory_identity(directory, true) == identity, "history-directory-changed");
        const auto current = lease.directory.metadata("telemetry.sqlite", maximum);
        require(current && current->volume == initial.volume && current->file == initial.file, "history-file-changed");
        journal_operation([&] { return lease.directory.metadata("telemetry.sqlite-journal", maximum); });
        for (const auto* suffix : {"-wal", "-shm"})
            require(!lease.directory.metadata(std::string("telemetry.sqlite") + suffix, maximum), "history-sidecar-unsafe");
    }
#else
    struct stat initial{};
    explicit Impl(const fs::path& path) : directory(path), file(path / "telemetry.sqlite"), identity(directory_identity(path, true)) {
        const int fd = ::open(file.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        created = fd >= 0;
        if (created) ::close(fd); else require(errno == EEXIST, "history-file-unsafe");
        initial = stamp(file); check();
    }
    void check() const {
        require(directory_identity(directory, true) == identity, "history-directory-changed");
        const auto current = stamp(file);
        require(current.st_dev == initial.st_dev && current.st_ino == initial.st_ino, "history-file-changed");
        (void)stamp(file.string() + "-journal", true);
        for (const auto* suffix : {"-wal", "-shm"}) {
            struct stat unexpected{};
            require(::lstat((file.string() + suffix).c_str(), &unexpected) != 0 && errno == ENOENT, "history-sidecar-unsafe");
        }
    }
#endif
};
LocalHistoryFiles::LocalHistoryFiles(const fs::path& directory) {
    try { impl_ = std::make_unique<Impl>(directory); }
    catch (const Error&) { throw; }
    catch (const std::exception&) { throw Error("history-file-unsafe", "Local history files are unavailable or unsafe."); }
}
LocalHistoryFiles::~LocalHistoryFiles() = default;
bool LocalHistoryFiles::created() const { return impl_->created; }
std::string LocalHistoryFiles::path() const { const auto text = impl_->file.u8string(); return {text.begin(), text.end()}; }
void LocalHistoryFiles::check() const {
    try { impl_->check(); }
    catch (const Error&) { throw; }
    catch (const std::exception&) { throw Error("history-file-unsafe", "Local history files are unavailable or unsafe."); }
}
const char* LocalHistoryFiles::vfs() const {
#ifdef _WIN32
    return vfs_name;
#else
    return nullptr;
#endif
}
}
