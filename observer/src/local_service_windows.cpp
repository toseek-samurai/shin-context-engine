#include "shin/observer/local_service_platform.h"
#include "shin/observer/observer.h"
#include "shin/windows/file.h"
#include "shin/windows/transport.h"
#include <windows.h>
#include <array>
#include <vector>

namespace shin::observer {
namespace {
namespace native = shin::windows;
template<class F> decltype(auto) io(F&& action) {
    try { return action(); }
    catch (const native::TransportError& error) { throw Error(error.code, error.what()); }
}
void require(bool value) { if (!value) throw Error("service-start-failed", "Cannot establish owned native service startup."); }
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE input = nullptr) : value(input) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
class WindowsChannel final : public ServiceChannel {
public:
    explicit WindowsChannel(native::Channel input) : channel(std::move(input)) {}
    std::optional<std::string> read(ServiceDeadline deadline) override { return io([&] { return channel.read_some(deadline); }); }
    std::optional<std::size_t> write(std::string_view bytes) override { return io([&] { return channel.write_some(bytes); }); }
    void wait_write(ServiceDeadline deadline) override { (void)io([&] { return channel.wait_written(deadline); }); }
    bool input_or_closed() const override { return channel.input_or_closed(); }
    bool reply_consumed() const override { return io([&] { return channel.reply_consumed(); }); }
    void finish_reply(ServiceDeadline deadline, const ServiceCancelled& cancelled) override {
        io([&] { channel.finish_reply(deadline, cancelled); });
    }
private:
    native::Channel channel;
};
std::string endpoint_key(const std::string& identity) { return digest("shin.local-service.v1:" + identity); }
class WindowsEndpoint final : public ServiceEndpoint {
public:
    explicit WindowsEndpoint(const fs::path& home)
        : path(local_service_directory(home)), directory(path, native::Directory::Access::private_owner),
          identity(directory.identity()), listener(endpoint_key(identity)) {}
    std::unique_ptr<ServiceChannel> accept() override {
        try {
            auto channel = listener.try_accept();
            if (!channel) return {};
            return std::make_unique<WindowsChannel>(std::move(*channel));
        } catch (const native::TransportError& error) {
            if (error.code == "service-peer-denied") return {};
            throw Error(error.code, error.what());
        }
    }
    void verify() const override {
        directory.check();
        if (directory_identity(path, true) != identity)
            throw Error("service-endpoint-changed", "Private service directory identity changed.");
    }
private:
    fs::path path;
    native::Directory directory;
    std::string identity;
    native::PipeListener listener;
};
std::wstring quoted(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0; result += ch;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
class WindowsLaunch final : public ServiceLaunch {
public:
    WindowsLaunch(const fs::path& executable, const fs::path& home)
        : image(executable), job(CreateJobObjectW(nullptr, nullptr)) {
        image.require_executable_owner();
        require(image.image_identity() == native::running_image_identity() && job.value);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0);
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        Handle output(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        require(input.value != INVALID_HANDLE_VALUE && output.value != INVALID_HANDLE_VALUE);
        std::array<HANDLE, 2> inherited{input.value, output.value};
        for (const auto handle : inherited) require(SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != 0);
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        require(size && size < 65536);
        std::vector<unsigned char> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(attributes, 1, 0, &size) != 0);
        struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
        require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr) != 0);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = output.value; startup.StartupInfo.hStdError = output.value;
        std::array<wchar_t, 32768> windows{};
        const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
        require(length && length < windows.size());
        std::wstring environment = L"SystemRoot=" + std::wstring(windows.data(), length);
        environment += L'\0'; environment += L"TEMP=" + fs::temp_directory_path().native();
        environment += L'\0'; environment += L"TMP=" + fs::temp_directory_path().native();
        environment += L'\0'; environment += L'\0';
        auto command = quoted(executable.native()) + L" _serve-registered --home " + quoted(home.native());
        PROCESS_INFORMATION child{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment.data(), executable.parent_path().c_str(), &startup.StartupInfo, &child) != 0);
        process.value = child.hProcess; Handle thread(child.hThread);
        if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            TerminateProcess(process.value, 1); WaitForSingleObject(process.value, 5000);
            throw Error("service-start-failed", "Cannot bind native service startup lifetime.");
        }
    }
    ~WindowsLaunch() override {
        if (owned && process.value) { TerminateJobObject(job.value, 1); WaitForSingleObject(process.value, 5000); }
    }
    bool exited() override { return WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT; }
    void release() override {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0);
        owned = false;
    }
private:
    native::ReadFile image;
    Handle job, process;
    bool owned = true;
};
}
std::uint64_t service_process_id() { return GetCurrentProcessId(); }
const char* service_transport() { return "local-pipe-v1"; }
std::unique_ptr<ServiceReaping> service_reaping() { return {}; }
std::string service_executable_identity(const fs::path& executable) {
    try {
        native::ReadFile image(executable); image.require_executable_owner();
        const auto stamp = image.stamp();
        return image.image_identity() + ":" + std::to_string(stamp.volume) + ":" + std::to_string(stamp.file) + ":" + std::to_string(stamp.bytes);
    } catch (const std::exception&) { throw Error("service-executable-changed", "Native worker executable is unavailable or unsafe."); }
}
std::unique_ptr<ServiceEndpoint> make_service_endpoint(const fs::path& home) {
    return io([&] { return std::make_unique<WindowsEndpoint>(home); });
}
void check_stale_service_endpoint(const fs::path& home) { (void)directory_identity(local_service_directory(home), true); }
std::unique_ptr<ServiceChannel> connect_service(const fs::path& home, ServiceDeadline deadline, const ServiceCancelled& cancelled) {
    const auto directory = local_service_directory(home);
    const auto identity = directory_identity(directory, true);
    auto channel = io([&] { return native::Channel::connect(endpoint_key(identity), deadline, cancelled); });
    if (directory_identity(directory, true) != identity) throw Error("service-endpoint-changed", "Service directory changed during connection.");
    return std::make_unique<WindowsChannel>(std::move(channel));
}
std::unique_ptr<ServiceLaunch> launch_service(const fs::path& executable, const fs::path& home) {
    return std::make_unique<WindowsLaunch>(executable, home);
}
}
