#pragma once
#include "io.h"
#include <chrono>
#include <functional>
#include <string_view>

namespace shin::observer {
using ServiceDeadline = std::chrono::steady_clock::time_point;
using ServiceCancelled = std::function<bool()>;
// Private platform IO only. Scheduling, framing, admission and history are shared.
class ServiceChannel {
public:
    virtual ~ServiceChannel() = default;
    virtual std::optional<std::string> read(ServiceDeadline deadline) = 0;
    virtual std::optional<std::size_t> write(std::string_view bytes) = 0;
    virtual void wait_write(ServiceDeadline deadline) = 0;
    virtual bool input_or_closed() const = 0;
    virtual bool reply_consumed() const = 0;
    virtual void finish_reply(ServiceDeadline deadline, const ServiceCancelled& cancelled) = 0;
};
class ServiceEndpoint {
public:
    virtual ~ServiceEndpoint() = default;
    virtual std::unique_ptr<ServiceChannel> accept() = 0;
    virtual void verify() const = 0;
};
class ServiceLaunch {
public:
    virtual ~ServiceLaunch() = default;
    virtual bool exited() = 0;
    virtual void release() = 0;
};
class ServiceReaping { public: virtual ~ServiceReaping() = default; };
fs::path local_service_directory(const fs::path& home);
std::string service_executable_identity(const fs::path& executable);
std::uint64_t service_process_id();
const char* service_transport();
std::unique_ptr<ServiceEndpoint> make_service_endpoint(const fs::path& home);
std::unique_ptr<ServiceChannel> connect_service(const fs::path& home, ServiceDeadline deadline, const ServiceCancelled& cancelled);
std::unique_ptr<ServiceLaunch> launch_service(const fs::path& executable, const fs::path& home);
std::unique_ptr<ServiceReaping> service_reaping();
void check_stale_service_endpoint(const fs::path& home);
void service_write(ServiceChannel& channel, const std::string& bytes, ServiceDeadline deadline, const ServiceCancelled& cancelled = {});
std::string service_read(ServiceChannel& channel, std::size_t limit, ServiceDeadline deadline, const ServiceCancelled& cancelled = {});
}
