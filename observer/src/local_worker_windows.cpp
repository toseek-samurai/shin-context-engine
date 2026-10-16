#include "shin/observer/local_worker.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_rpc.h"
#include "shin/windows/transport.h"
#include "context_internal.h"
#include <windows.h>
#include <thread>

namespace shin::observer {
namespace {
namespace native = shin::windows;
using Clock = std::chrono::steady_clock;
auto deadline(int seconds) { return Clock::now() + std::chrono::seconds(seconds); }
// Unlike a cooperative IO timeout, this also bounds a blocked Core operation.
class WorkerDeadline {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread timer;
public:
    explicit WorkerDeadline(DWORD milliseconds) {
        if (!done) throw Error("service-worker-failed", "Cannot establish worker deadline.");
        try {
            timer = std::thread([this, milliseconds] {
                if (WaitForSingleObject(done, milliseconds) != WAIT_OBJECT_0)
                    TerminateProcess(GetCurrentProcess(), 124);
            });
        } catch (...) { CloseHandle(done); throw; }
    }
    ~WorkerDeadline() { SetEvent(done); timer.join(); CloseHandle(done); }
};
int worker(bool refresh) {
    WorkerDeadline bound(refresh ? 60000 : 30000);
    auto [input, output] = native::Channel::worker_stdio();
    Json result;
    try {
        const auto payload = parse_json(input.read_frame(16384, deadline(2)));
        result = refresh ? execute_local_worker_refresh(payload, [&] { return input.input_or_closed(); },
            [&](const Json& progress) { output.write_frame(progress.dump(), 128 * 1024, deadline(2)); }) :
            execute_local_worker_read(payload);
    } catch (const Error& error) {
        result = refresh ? Json{{"ok", false}, {"error", error_json(error)}} : local_rpc::failure("", error.code, error.what());
    }
    output.write_frame(result.dump(), 128 * 1024, deadline(2));
    output.finish_reply(deadline(2));
    return 0;
}
}
int local_service_worker() { return worker(false); }
int local_refresh_worker() { return worker(true); }
int local_resident_worker() {
    auto [input, output] = native::Channel::worker_stdio();
    std::unique_ptr<LocalResidentWorker> resident;
    {
        WorkerDeadline bound(60000);
        try {
            resident = std::make_unique<LocalResidentWorker>(parse_json(input.read_frame(8192, deadline(2))));
            output.write_frame(resident->ready().dump(), 128 * 1024, deadline(2));
        } catch (const std::exception& error) {
            const auto* observer = dynamic_cast<const Error*>(&error);
            const auto* core = dynamic_cast<const shin::context::CliError*>(&error);
            const auto code = observer ? observer->code : core ? core->code() : "resident-preparation-failed";
            output.write_frame(Json{{"kind", "preparation-failed"}, {"code", code}}.dump(), 128 * 1024, deadline(2));
            output.finish_reply(deadline(2));
            return 1;
        }
    }
    for (;;) {
        bool available = false;
        try { available = input.wait_input(Clock::now() + std::chrono::milliseconds(100)); }
        catch (const native::TransportError& error) {
            if (error.code == "service-disconnected") return 0;
            throw;
        }
        if (!available) {
            if (resident->observed_unchanged()) continue;
            output.write_frame("{\"kind\":\"invalidated\",\"code\":\"project-updating\"}", 128 * 1024, deadline(2));
            output.finish_reply(deadline(2));
            return 0;
        }
        WorkerDeadline bound(30000);
        const auto started = Clock::now();
        std::string frame;
        try { frame = input.read_frame(8192, deadline(2)); }
        catch (const native::TransportError& error) {
            if (error.code == "service-disconnected") return 0;
            throw;
        }
        const auto result = resident->execute(frame, started);
        output.write_frame(result.dump(), 128 * 1024, deadline(2));
        if (!result.at("reusable").get<bool>()) {
            output.finish_reply(deadline(2));
            return 0;
        }
    }
}

} // namespace shin::observer
