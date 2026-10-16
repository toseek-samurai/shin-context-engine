#include "shin/observer/local_supervision.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/observer.h"
#ifdef __APPLE__
#include <sys/stat.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace shin::observer {
const char* local_service_manager() {
#ifdef __APPLE__
    return "launchd-user";
#else
    return "unsupported";
#endif
}
namespace {
#ifdef __APPLE__
struct Property {
    CFTypeRef value;
    explicit Property(CFTypeRef input) : value(input) {
        if (!value) throw Error("service-definition-failed", "Cannot serialize service definition.");
    }
    ~Property() { CFRelease(value); }
    Property(const Property&) = delete;
    Property& operator=(const Property&) = delete;
};

CFTypeRef property(const Json& input) {
    if (input.is_string()) {
        const auto text = input.get<std::string>();
        return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text.data()),
            static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
    }
    if (input.is_boolean()) return CFRetain(input.get<bool>() ? kCFBooleanTrue : kCFBooleanFalse);
    if (input.is_number_integer()) {
        const auto number = input.get<std::int64_t>();
        return CFNumberCreate(nullptr, kCFNumberSInt64Type, &number);
    }
    if (input.is_array()) {
        auto array = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        Property owner(array);
        for (const auto& item : input) { Property child(property(item)); CFArrayAppendValue(array, child.value); }
        return CFRetain(array);
    }
    if (input.is_object()) {
        auto object = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        Property owner(object);
        for (const auto& item : input.items()) {
            Property key(property(Json(item.key()))), value(property(item.value()));
            CFDictionarySetValue(object, key.value, value.value);
        }
        return CFRetain(object);
    }
    throw Error("service-definition-failed", "Unsupported property-list value.");
}
#endif
}

std::string native_launchd_plist(const Json& unit) {
#ifndef __APPLE__
    (void)unit;
    throw Error("service-manager-unsupported", "Native launchd definitions require macOS.");
#else
    Property value(property(unit));
    Property bytes(CFPropertyListCreateData(nullptr, value.value, kCFPropertyListXMLFormat_v1_0, 0, nullptr));
    const auto data = static_cast<CFDataRef>(bytes.value);
    const auto size = CFDataGetLength(data);
    if (size < 1 || size > 32768) throw Error("service-definition-failed", "Service definition exceeds its limit.");
    return {reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(size)};
#endif
}

Json local_service_definition(const LocalProjectRequest& request, const fs::path& executable, bool installed_entry) {
#ifndef __APPLE__
    (void)request; (void)executable; (void)installed_entry;
    throw Error("service-manager-unsupported", "This build does not yet export a native service-manager definition for this platform.");
#else
    const auto home = fs::absolute(request.home).lexically_normal();
    (void)directory_identity(home, true);
    if (fs::canonical(home) != home) throw Error("unsafe-directory", "Service home must use its canonical non-symlink path.");
    const auto projects = registered_local_projects(home);
    // A valid empty registry is the normal result of detaching the last project.
    const auto binary = fs::canonical(executable);
    struct stat status{};
    if (::lstat(binary.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        (status.st_uid != ::geteuid() && status.st_uid != 0) || (status.st_mode & 06022) ||
        ::access(binary.c_str(), X_OK) != 0)
        throw Error("unsafe-executable", "The native service executable must be owned by this user or root and not writable by others.");
    const auto label = "org.shin.context." + digest(home.string()).substr(0, 32);
    const auto arguments = installed_entry ? Json::array({binary.string(), "_serve-supervised", "--home", home.string()}) :
        Json::array({binary.string(), "serve", "--registered", "--home", home.string()});
    const Json unit{{"Label", label}, {"Program", binary.string()},
        {"ProgramArguments", arguments},
        {"WorkingDirectory", "/"}, {"EnvironmentVariables", {{"LC_ALL", "C"}}},
        {"RunAtLoad", true}, {"KeepAlive", {{"SuccessfulExit", false}}},
        {"ThrottleInterval", 10}, {"ExitTimeOut", 45}, {"Umask", 63},
        {"StandardOutPath", "/dev/null"}, {"StandardErrorPath", "/dev/null"}};
    const auto plist = native_launchd_plist(unit);
    return {{"schema_version", "shin.local-service-definition.v1"}, {"tool", "shin-ce"},
        {"command", "service-definition"}, {"ok", true}, {"version", SHIN_OBSERVER_VERSION},
        {"manager", "launchd-user"}, {"label", label}, {"service_home", home.string()},
        {"registered_projects", projects.size()}, {"unit", unit}, {"plist", plist},
        {"installation_performed", false}, {"manager_state", "not-assessed"},
        {"readiness_assessed", false}, {"consumer_mutated", false},
        {"normal_stop_restarts", false}, {"abnormal_exit_restarts", true}};
#endif
}
}
