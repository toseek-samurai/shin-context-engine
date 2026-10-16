#include "shin/observer/local_changes.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#include "shin/windows/changes.h"
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#elif defined(__linux__)
#include <array>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#endif

namespace shin::observer {
namespace {
constexpr std::size_t maximum_watches = 32768;
constexpr std::size_t maximum_depth = 256;
constexpr std::size_t maximum_path_bytes = 4096;
#if !defined(_WIN32)
constexpr std::size_t maximum_event_bytes = 4U * 1024U * 1024U;

bool within(const fs::path& path, const fs::path& root) {
    const auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    return mismatch.first == root.end();
}
#endif

void require_directory(const fs::path& path) {
    fs::path component;
    std::size_t depth = 0;
    for (const auto& part : path) {
        if (++depth > maximum_depth) throw Error("change-monitor-limit", "Monitor path depth exceeded");
        component /= part;
        if (!fs::is_directory(fs::symlink_status(component))) {
            throw Error("change-monitor-setup", "Monitor roots must be existing non-symlink directories");
        }
    }
}

#if defined(__linux__)
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {
        if (value_ < 0) throw Error("change-monitor-setup", "Cannot open monitor directory");
    }
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
    void reset(int value) { if (value_ >= 0) ::close(value_); value_ = value; }
private:
    int value_;
};

int open_directory(const fs::path& path) {
    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (const auto& part : path.relative_path()) {
        Descriptor next(::openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        current.reset(next.release());
    }
    return current.release();
}
#endif
} // namespace

class LocalChanges::Impl {
public:
    Impl(std::vector<fs::path> input, std::function<bool(const fs::path&, bool)> predicate)
        : roots(std::move(input)), relevant(std::move(predicate)) {
        if (roots.empty() || roots.size() > maximum_watches || !relevant) {
            throw Error("change-monitor-invalid", "Monitor requires bounded roots and a predicate");
        }
    }

    void changed() noexcept {
        if (serial.fetch_add(1, std::memory_order_acq_rel) == std::numeric_limits<std::uint64_t>::max()) {
            failure.store("event-token-overflow", std::memory_order_release);
            good.store(false, std::memory_order_release);
        }
    }

    void failed(const char* reason = "native-monitor-failed") noexcept {
        auto previous = failure.load(std::memory_order_acquire);
        if (*previous == '\0') failure.compare_exchange_strong(previous, reason, std::memory_order_acq_rel);
        good.store(false, std::memory_order_release);
        changed();
    }

    void start() noexcept {
        try {
            for (auto& root : roots) {
                root = fs::absolute(root).lexically_normal();
                if (root != root.root_path() && root.filename().empty()) root = root.parent_path();
                if (root.native().size() > maximum_path_bytes || root.native().find('\0') != std::string::npos) {
                    throw Error("change-monitor-limit", "Monitor root path exceeded its bound");
                }
                require_directory(root);
            }
            std::sort(roots.begin(), roots.end());
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            start_native();
        } catch (...) {
            failed("native-setup-failed");
        }
    }

    std::vector<fs::path> roots;
    std::function<bool(const fs::path&, bool)> relevant;
    std::atomic<std::uint64_t> serial{0};
    std::atomic<bool> good{true};
    std::atomic<const char*> failure{""};
    std::mutex delivery;

#if defined(_WIN32)
    std::unique_ptr<windows::DirectoryChanges> native;
    void start_native() {
        native = std::make_unique<windows::DirectoryChanges>(roots,
            [this](const fs::path& path, bool directory) { if (relevant(path, directory)) changed(); },
            [this] { failed(); });
    }
    void flush() { if (native) native->flush(); }
#elif defined(__APPLE__)
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    bool started = false;

    static void fence(void*) {}

    static void callback(ConstFSEventStreamRef, void* context, std::size_t count, void* paths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            if (count > maximum_watches) { self.failed("event-batch-overflow"); return; }
            auto names = static_cast<char**>(paths);
            std::size_t bytes = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto flag = flags[i];
                if (flag & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped |
                            kFSEventStreamEventFlagEventIdsWrapped | kFSEventStreamEventFlagRootChanged)) {
                    self.failed(flag & kFSEventStreamEventFlagRootChanged ? "root-changed" : "events-lost");
                    continue;
                }
                if (flag & kFSEventStreamEventFlagHistoryDone) continue;
                const auto length = ::strnlen(names[i], maximum_path_bytes + 1);
                bytes += length;
                if (length > maximum_path_bytes || bytes > maximum_event_bytes) { self.failed("event-byte-budget"); return; }
                const fs::path path = fs::path(std::string(names[i], length)).lexically_normal();
                if (!path.is_absolute()) { self.failed(); continue; }
                const bool directory = (flag & (kFSEventStreamEventFlagItemIsDir |
                    kFSEventStreamEventFlagMustScanSubDirs)) ||
                    !(flag & (kFSEventStreamEventFlagItemIsFile | kFSEventStreamEventFlagItemIsSymlink));
                const bool lost_volume = flag & (kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount);
                for (const auto& root : self.roots) {
                    if (within(path, root)) {
                        if (lost_volume) self.failed("volume-changed");
                        else if (self.relevant(path, directory)) self.changed();
                        break;
                    }
                    if (directory && within(root, path)) {
                        if (lost_volume) self.failed("volume-changed");
                        else if (self.relevant(root, true)) self.changed();
                    }
                }
            }
        } catch (...) { self.failed("event-callback-failed"); }
    }

    void start_native() {
        std::vector<std::string> names;
        std::vector<struct stat> identities(roots.size());
        for (std::size_t i = 0; i < roots.size(); ++i) {
            names.push_back(roots[i].string());
            if (::lstat(names.back().c_str(), &identities[i]) != 0 || !S_ISDIR(identities[i].st_mode)) {
                throw Error("change-monitor-setup", "Cannot identify monitor root");
            }
        }
        queue = dispatch_queue_create("shin.observer.local-changes", DISPATCH_QUEUE_SERIAL);
        if (!queue) throw Error("change-monitor-setup", "Cannot create monitor dispatch queue");
        dispatch_queue_set_specific(queue, this, this, nullptr);
        auto paths = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        if (!paths) throw Error("change-monitor-setup", "Cannot allocate monitor roots");
        for (const auto& name : names) {
            auto path = CFStringCreateWithFileSystemRepresentation(nullptr, name.c_str());
            if (!path) {
                CFRelease(paths);
                throw Error("change-monitor-setup", "Cannot encode monitor root");
            }
            CFArrayAppendValue(paths, path);
            CFRelease(path);
        }
        FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
        stream = FSEventStreamCreate(nullptr, callback, &context, paths, kFSEventStreamEventIdSinceNow,
            0.020, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                   kFSEventStreamCreateFlagWatchRoot);
        CFRelease(paths);
        if (!stream) throw Error("change-monitor-setup", "Cannot create native event stream");
        FSEventStreamSetDispatchQueue(stream, queue);
        started = FSEventStreamStart(stream);
        if (!started) throw Error("change-monitor-setup", "Cannot start native event stream");
        for (std::size_t i = 0; i < roots.size(); ++i) {
            require_directory(roots[i]);
            struct stat current{};
            if (::lstat(names[i].c_str(), &current) != 0 || current.st_dev != identities[i].st_dev ||
                current.st_ino != identities[i].st_ino) {
                throw Error("change-monitor-setup", "Monitor root changed during stream setup");
            }
        }
    }

    void flush() {
        if (queue && dispatch_get_specific(this)) {
            throw Error("change-monitor-reentrant", "Cannot flush from the monitor predicate");
        }
        std::lock_guard<std::mutex> lock(delivery);
        if (started) {
            if (!stream || !queue) { failed(); return; }
            FSEventStreamFlushSync(stream);
            dispatch_sync_f(queue, nullptr, fence);
        }
    }

    ~Impl() {
        if (stream) {
            if (started) FSEventStreamStop(stream);
            FSEventStreamInvalidate(stream);
            dispatch_sync_f(queue, nullptr, fence);
            FSEventStreamRelease(stream);
        }
        if (queue) dispatch_release(queue);
    }

#elif defined(__linux__)
    struct Watch { fs::path path; bool root; bool subtree; };
    int events = -1;
    int wake = -1;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::unordered_map<int, Watch> watches;
    std::unordered_set<int> retired;

    void add_tree(const fs::path& path, bool root) {
        Descriptor directory(open_directory(path));
        std::size_t entries = 0;
        walk(directory.get(), path, root, 0, entries);
        Descriptor current(open_directory(path));
        struct stat before{}, after{};
        if (::fstat(directory.get(), &before) != 0 || ::fstat(current.get(), &after) != 0 ||
            before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
            throw Error("change-monitor-setup", "Monitor directory changed during setup");
        }
    }

    bool add_watch(int descriptor, const fs::path& path, bool root, bool subtree) {
        if (path.native().size() > maximum_path_bytes) {
            throw Error("change-monitor-limit", "Monitor directory watch bound exceeded");
        }
        // /proc is only an alias to an already O_NOFOLLOW-pinned directory. No
        // source symlinks are followed, including during concurrent replacement.
        const auto pinned = "/proc/self/fd/" + std::to_string(descriptor);
        const auto mask = IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_MASK_ADD |
            (subtree ? IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                       IN_MOVED_FROM | IN_MOVED_TO : 0U);
        const int watch = ::inotify_add_watch(events, pinned.c_str(), mask);
        if (watch < 0 || retired.count(watch)) {
            throw Error("change-monitor-setup", "Cannot install unambiguous native directory watch");
        }
        const auto found = watches.find(watch);
        if (found != watches.end()) {
            if (found->second.path != path) throw Error("change-monitor-setup", "Aliased monitor directories");
            found->second.root = found->second.root || root;
            const bool new_subtree = subtree && !found->second.subtree;
            found->second.subtree = found->second.subtree || subtree;
            return new_subtree;
        }
        if (watches.size() + retired.size() >= maximum_watches) {
            ::inotify_rm_watch(events, watch);
            throw Error("change-monitor-limit", "Monitor directory watch bound exceeded");
        }
        watches.emplace(watch, Watch{path, root, subtree});
        return subtree;
    }

    void walk(int descriptor, const fs::path& path, bool root, std::size_t depth, std::size_t& entries) {
        if (depth > maximum_depth) throw Error("change-monitor-limit", "Monitor recursion bound exceeded");
        if (!add_watch(descriptor, path, root, true)) return;
        if (!relevant(path, true)) return;
        Descriptor duplicate(::fcntl(descriptor, F_DUPFD_CLOEXEC, 0));
        DIR* raw = ::fdopendir(duplicate.get());
        if (!raw) throw Error("change-monitor-setup", "Cannot enumerate monitor directory");
        duplicate.release();
        std::unique_ptr<DIR, int(*)(DIR*)> directory(raw, ::closedir);
        while (true) {
            errno = 0;
            auto* entry = ::readdir(directory.get());
            if (!entry) {
                if (errno) throw Error("change-monitor-setup", "Cannot read monitor directory");
                break;
            }
            if (++entries > 1024U * 1024U) throw Error("change-monitor-limit", "Monitor enumeration bound exceeded");
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            struct stat status{};
            if (::fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
                throw Error("change-monitor-setup", "Cannot inspect monitor entry");
            }
            if (!S_ISDIR(status.st_mode)) continue;
            const auto child_path = path / entry->d_name;
            if (!relevant(child_path, true)) continue;
            Descriptor child(::openat(descriptor, entry->d_name,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            walk(child.get(), child_path, false, depth + 1, entries);
        }
    }

    void remove_tree(const fs::path& path) {
        for (auto it = watches.begin(); it != watches.end();) {
            if (!within(it->second.path, path)) { ++it; continue; }
            if (it->second.root) failed("root-changed");
            const int watch = it->first;
            if (::inotify_rm_watch(events, watch) != 0 && errno != EINVAL) failed();
            retired.insert(watch);
            it = watches.erase(it);
        }
    }

    void event(const inotify_event& entry, const char* name) {
        if (entry.mask & IN_Q_OVERFLOW) { failed("events-lost"); return; }
        if (retired.count(entry.wd)) {
            if (entry.mask & IN_IGNORED) retired.erase(entry.wd);
            return;
        }
        auto found = watches.find(entry.wd);
        if (found == watches.end()) { failed(); return; }
        const Watch watch = found->second;
        if (entry.mask & IN_UNMOUNT) { failed("volume-changed"); return; }
        if (entry.mask & IN_IGNORED) { failed(); watches.erase(found); return; }
        if (entry.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            if (watch.root) { failed(); return; }
            if (relevant(watch.path, true)) changed();
            remove_tree(watch.path);
            return;
        }
        if (!watch.subtree) return;
        const auto path = entry.len ? watch.path / name : watch.path;
        if (path.native().size() > maximum_path_bytes) { failed(); return; }
        const bool directory = !entry.len || (entry.mask & IN_ISDIR);
        const bool accepted = relevant(path, directory);
        if (accepted) changed();
        if (directory && (entry.mask & (IN_DELETE | IN_MOVED_FROM))) remove_tree(path);
        if (directory && accepted && (entry.mask & (IN_CREATE | IN_MOVED_TO))) {
            // Bracket watch installation: qualification crossing the unwatched
            // interval must see the second invalidation, even if files preexist.
            add_tree(path, false);
            changed();
        }
    }

    void drain() noexcept {
        try {
            alignas(inotify_event) std::array<char, 64U * 1024U> buffer{};
            std::size_t bytes = 0;
            while (good.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire)) {
                const auto count = ::read(events, buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && errno == EAGAIN) return;
                if (count <= 0) { failed(); return; }
                bytes += static_cast<std::size_t>(count);
                if (bytes > maximum_event_bytes) { failed(); return; }
                std::size_t offset = 0;
                while (offset < static_cast<std::size_t>(count)) {
                    const auto remaining = static_cast<std::size_t>(count) - offset;
                    if (remaining < sizeof(inotify_event)) { failed(); return; }
                    const auto& entry = *reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                    if (entry.len > remaining - sizeof(entry) || entry.len > maximum_path_bytes) { failed(); return; }
                    const char* name = buffer.data() + offset + sizeof(entry);
                    if (entry.len && (!std::memchr(name, '\0', entry.len) || std::strchr(name, '/') ||
                        !std::strcmp(name, ".") || !std::strcmp(name, ".."))) { failed(); return; }
                    event(entry, name);
                    if (!good.load(std::memory_order_acquire)) return;
                    offset += sizeof(entry) + entry.len;
                }
            }
        } catch (...) { failed(); }
    }

    void start_native() {
        events = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        wake = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (events < 0 || wake < 0) throw Error("change-monitor-setup", "Cannot create native monitor descriptors");
        for (const auto& root : roots) {
            // inotify does not report a root's MOVE_SELF when only an ancestor
            // moves. Protect each ancestor without recursively observing it.
            fs::path parent = root.parent_path();
            while (!parent.empty()) {
                Descriptor directory(open_directory(parent));
                add_watch(directory.get(), parent, true, false);
                if (parent == parent.root_path()) break;
                parent = parent.parent_path();
            }
            add_tree(root, true);
        }
        worker = std::thread([this] {
            while (!stopping.load(std::memory_order_acquire) && good.load(std::memory_order_acquire)) {
                pollfd descriptors[]{{events, POLLIN, 0}, {wake, POLLIN, 0}};
                const int result = ::poll(descriptors, 2, -1);
                if (result < 0 && errno == EINTR) continue;
                if (result < 0 || ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL))) {
                    failed(); return;
                }
                if (descriptors[1].revents & POLLIN) return;
                if (descriptors[0].revents & POLLIN) {
                    std::lock_guard<std::mutex> lock(delivery);
                    drain();
                }
            }
        });
    }

    void flush() {
        std::lock_guard<std::mutex> lock(delivery);
        if (events >= 0 && good.load(std::memory_order_acquire)) drain();
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        if (worker.joinable()) {
            const std::uint64_t signal = 1;
            ssize_t result;
            do { result = ::write(wake, &signal, sizeof(signal)); } while (result < 0 && errno == EINTR);
            worker.join();
        }
        if (events >= 0) ::close(events);
        if (wake >= 0) ::close(wake);
    }
#else
    void start_native() { throw Error("change-monitor-unavailable", "Native change monitoring is unsupported"); }
    void flush() {}
#endif
};

LocalChanges::LocalChanges(std::vector<fs::path> roots,
    std::function<bool(const fs::path&, bool)> relevant)
    : impl_(std::make_unique<Impl>(std::move(roots), std::move(relevant))) { impl_->start(); }
LocalChanges::~LocalChanges() = default;
std::uint64_t LocalChanges::sequence() const noexcept { return impl_->serial.load(std::memory_order_acquire); }
bool LocalChanges::healthy() const noexcept { return impl_->good.load(std::memory_order_acquire); }
std::string LocalChanges::failure_reason() const { return impl_->failure.load(std::memory_order_acquire); }
void LocalChanges::flush() { impl_->flush(); }
} // namespace shin::observer
