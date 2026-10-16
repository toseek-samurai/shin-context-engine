#include "shin/observer/io.h"
#include <array>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/attr.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int shin_identity_test_fstat(int descriptor, struct stat* status);
extern "C" int shin_identity_test_fstatat(int descriptor, const char* name, struct stat* status, int flags);
extern "C" int shin_identity_test_fgetattrlist(int descriptor, struct attrlist* attributes,
                                             void* buffer, size_t size, unsigned long options);

namespace {
using namespace shin::observer;
bool another_boot = false;
bool another_volume = false;
bool no_volume = false;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(const char* code, Action action) {
    try { action(); }
    catch (const Error& error) { check(error.code == code, error.what()); return; }
    throw std::runtime_error("Expected identity refusal");
}
}

extern "C" int shin_identity_test_fstat(int descriptor, struct stat* status) {
    const int result = ::fstat(descriptor, status);
    if (result == 0 && another_boot) status->st_dev ^= 1;
    return result;
}
extern "C" int shin_identity_test_fstatat(int descriptor, const char* name, struct stat* status, int flags) {
    const int result = ::fstatat(descriptor, name, status, flags);
    if (result == 0 && another_boot) status->st_dev ^= 1;
    return result;
}
extern "C" int shin_identity_test_fgetattrlist(int descriptor, struct attrlist* attributes,
                                             void* buffer, size_t size, unsigned long options) {
    const int result = ::fgetattrlist(descriptor, attributes, buffer, size, options);
    if (result == 0 && size == 20) {
        auto* bytes = static_cast<unsigned char*>(buffer) + 4;
        if (another_volume) bytes[0] ^= 1;
        if (no_volume) std::memset(bytes, 0, 16);
    }
    return result;
}

int main() {
    using namespace shin::observer;
    try {
    std::string pattern = "/tmp/shin-identity-XXXXXX";
    if (!::mkdtemp(pattern.data())) return 2;
    const fs::path root = fs::canonical(pattern);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
        const auto identity = directory_identity(root, true);
        check(identity.starts_with("mac-volume-v1:"), "Persistent format missing");
        {
            StateStore state(root, true, identity);
            state.save({{"value", 7}});
            another_boot = true;
            // A held descriptor's in-process anti-replacement guard stays strict.
            rejects("directory-changed", [&] { (void)state.load(); });
        }
        check(directory_identity(root, true) == identity, "Reboot changed persistent identity");
        {
            StateStore reader(root, false, identity);
            const auto value = reader.load();
            if (!value || value->at("value") != 7) throw std::runtime_error("State lost after reboot");
        }
        another_volume = true;
        rejects("directory-changed", [&] { StateStore reader(root, false, identity); });
        another_volume = false;
        no_volume = true;
        rejects("directory-identity-unavailable", [&] { (void)directory_identity(root); });
        no_volume = false;
        const auto child = root / "child";
        ensure_private_directory(child);
        const auto old = directory_identity(child);
        fs::rename(child, root / "retired");
        ensure_private_directory(child);
        check(directory_identity(child) != old, "Replaced directory reused identity");
        rejects("directory-changed", [&] { StateStore reader(child, false, old); });
        fs::create_directory_symlink(child, root / "alias");
        rejects("unsafe-directory", [&] { (void)directory_identity(root / "alias"); });
        std::cout << "persistent identity: reboot, volume, replacement, symlink and missing UUID checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
