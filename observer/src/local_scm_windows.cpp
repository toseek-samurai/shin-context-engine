#include "local_scm_windows.h"
#include "shin/observer/local_service.h"
#include "shin/windows/file.h"
#include "shin/windows/transport.h"
#include <thread>
#include <system_error>

namespace shin::observer {
const char* local_service_manager() { return "windows-scm-host"; }
namespace scm {
std::wstring command(const fs::path& executable) { return L"\"" + executable.native() + L"\" _scm-host"; }
std::string endpoint() { return digest("shin.windows-scm-host.v1"); }
Service manager(DWORD access) {
    const auto handle = OpenSCManagerW(nullptr, nullptr, access);
    if (!handle) failed("Cannot open Windows Service Control Manager");
    return Service(handle);
}
Service open(SC_HANDLE scm, const std::wstring& service, DWORD access) {
    const auto handle = OpenServiceW(scm, service.c_str(), access);
    if (!handle && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) failed("Cannot open Observer host");
    return Service(handle);
}
SERVICE_STATUS_PROCESS state(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{}; DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes))
        failed("Cannot inspect Observer host state");
    return status;
}
void verify(SC_HANDLE service, const fs::path& executable, bool disabled) {
    DWORD bytes = 0; QueryServiceConfigW(service, nullptr, 0, &bytes);
    require(bytes >= sizeof(QUERY_SERVICE_CONFIGW) && bytes <= 65536, "service-install-changed", "Invalid SCM configuration.");
    std::vector<unsigned char> buffer(bytes); auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, bytes, &bytes)) failed("Cannot inspect Observer host configuration");
    require(config->dwServiceType == SERVICE_WIN32_OWN_PROCESS && std::wstring(config->lpBinaryPathName) == command(executable) &&
        std::wstring(config->lpServiceStartName) == L"LocalSystem" &&
        (config->dwStartType == SERVICE_AUTO_START || (disabled && config->dwStartType == SERVICE_DISABLED)),
        "service-install-changed", "SCM identity, account, executable or startup policy differs; no takeover is permitted.");
}
void protected_image(const fs::path& executable) {
    try { windows::ReadFile image(executable); image.require_machine_owned(); (void)image.image_identity(); }
    catch (const std::system_error& error) {
        throw Error("unsafe-service-executable", "Cannot verify protected installation metadata (Windows " + std::to_string(error.code().value()) + ").");
    } catch (const std::exception&) {
        throw Error("unsafe-service-executable", "SCM requires administrator-protected code, not a writable build or download directory.");
    }
}
void wait_stopped(SC_HANDLE service) {
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (state(service).dwCurrentState != SERVICE_STOPPED) {
        require(Clock::now() < deadline, "service-stop-timeout", "Observer host did not stop; data and files were preserved.");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
Json host_request(const fs::path& executable, const char* action) {
    auto scm = manager(); auto service = open(scm.value, name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    require(service.value != nullptr, "service-not-installed", "Install the Windows desktop kit first.");
    verify(service.value, executable);
    const auto status = state(service.value);
    require(status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId, "service-manager-not-ready", "Windows Observer host is not running.");
    try {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        auto channel = windows::Channel::connect_system(endpoint(), status.dwProcessId, deadline);
        channel.write_frame(action, 16, deadline);
        auto result = Json::parse(channel.read_frame(4096, deadline));
        require(result.is_object() && result.value("schema_version", "") == "shin.scm-user.v1" && result.value("ok", false),
            "service-manager-not-ready", "SCM user runtime is unavailable.");
        return result;
    } catch (const windows::TransportError& error) { throw Error(error.code, error.what()); }
}
void log_failure(const char* code) noexcept {
    wchar_t text[160] = L"Shin Windows host/runtime failure: "; std::size_t offset = wcslen(text);
    for (std::size_t i = 0; code[i] && offset + 1 < std::size(text); ++i) text[offset++] = static_cast<unsigned char>(code[i]);
    const auto source = RegisterEventSourceW(nullptr, name);
    if (source) { const wchar_t* strings[]{text}; ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr); DeregisterEventSource(source); }
}
}
namespace {
using namespace scm;
void legacy_receipt(const fs::path& home) {
    if (fs::exists(fs::symlink_status(home / "supervision")))
        throw Error("service-install-changed", "Legacy supervision receipt was not adopted or bypassed.");
}
Json response(const char* action) {
    return {{"schema_version", "shin.local-autostart-status.v1"}, {"tool", "shin-ce"}, {"version", SHIN_OBSERVER_VERSION},
        {"command", action}, {"ok", true}, {"manager", local_service_manager()}, {"scope", "machine-host-private-user-workers"},
        {"data_preserved", true}, {"readiness_assessed", false}};
}
void recovery(SC_HANDLE service) {
    SC_ACTION actions[]{{SC_ACTION_RESTART,10000},{SC_ACTION_RESTART,10000},{SC_ACTION_RESTART,10000},{SC_ACTION_NONE,0}};
    SERVICE_FAILURE_ACTIONSW policy{}; policy.dwResetPeriod=86400; policy.cActions=4; policy.lpsaActions=actions;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &policy)) failed("Cannot configure SCM recovery");
    SERVICE_FAILURE_ACTIONS_FLAG noncrash{TRUE};
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &noncrash)) failed("Cannot configure SCM failure classification");
}
}
int windows_scm_dispatch() {
    wchar_t service_name[] = L"ShinObserverHost"; SERVICE_TABLE_ENTRYW table[]{{service_name, scm::host_main},{nullptr,nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) scm::failed("SCM host entry must be started by Windows");
    return 0;
}
Json windows_scm_setup(WindowsScmAction action, const fs::path& input) {
    using namespace scm;
    const auto executable=fs::canonical(input); protected_image(executable);
    auto scm=manager(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    auto legacy=open(scm.value, L"ShinObserver", SERVICE_QUERY_CONFIG);
    require(!legacy.value, "service-legacy-installation", "Remove the per-user desktop preview before installing the machine host; user data is preserved.");
    constexpr DWORD access=SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS|SERVICE_CHANGE_CONFIG|SERVICE_START|SERVICE_STOP|DELETE;
    auto existing=open(scm.value,name,access);
    if(existing.value) verify(existing.value,executable,true);
    const bool install=action==WindowsScmAction::install, remove=action==WindowsScmAction::remove;
    auto value=response(install?"service-install":remove?"service-uninstall":"service-maintenance-stop");
    if(install) {
        if(!existing.value) {
            existing.value=CreateServiceW(scm.value,name,L"Shin Observer Host",access,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,command(executable).c_str(),nullptr,nullptr,nullptr,L"LocalSystem",nullptr);
            if(!existing.value) failed("Cannot register Observer host");
        }
        if(!ChangeServiceConfigW(existing.value,SERVICE_NO_CHANGE,SERVICE_AUTO_START,SERVICE_NO_CHANGE,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr))
            failed("Cannot enable Observer host");
        recovery(existing.value);
        SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(L"Starts isolated, unelevated Shin Observers for signed-in users. Does not index project data itself.")};
        if(!ChangeServiceConfig2W(existing.value,SERVICE_CONFIG_DESCRIPTION,&description)) failed("Cannot describe Observer host");
        if(state(existing.value).dwCurrentState==SERVICE_STOP_PENDING) wait_stopped(existing.value);
        if(state(existing.value).dwCurrentState==SERVICE_STOPPED && !StartServiceW(existing.value,0,nullptr)) failed("Cannot start Observer host");
        const auto deadline=Clock::now()+std::chrono::seconds(20);
        while(state(existing.value).dwCurrentState==SERVICE_START_PENDING && Clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(state(existing.value).dwCurrentState==SERVICE_RUNNING,"service-manager-not-ready","SCM host failed to become ready.");
        value["phase"]="installed";
    } else if(existing.value) {
        if(!ChangeServiceConfigW(existing.value,SERVICE_NO_CHANGE,SERVICE_DISABLED,SERVICE_NO_CHANGE,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr))
            failed("Cannot disable Observer host");
        if(state(existing.value).dwCurrentState!=SERVICE_STOPPED) {
            SERVICE_STATUS ignored{};
            if(!ControlService(existing.value,SERVICE_CONTROL_STOP,&ignored) && GetLastError()!=ERROR_SERVICE_NOT_ACTIVE) failed("Cannot stop Observer host");
            wait_stopped(existing.value);
        }
        if(remove && !DeleteService(existing.value) && GetLastError()!=ERROR_SERVICE_MARKED_FOR_DELETE) failed("Cannot remove Observer host");
        value["phase"]=remove?"removed":"maintenance-stopped";
    } else value["phase"]="not-installed";
    return value;
}
Json local_service_definition(const LocalProjectRequest&, const fs::path& executable, bool) {
    auto value=response("service-definition");
    value.update({{"schema_version","shin.local-service-definition.v1"},{"service_name","ShinObserverHost"},
        {"executable",fs::canonical(executable).string()},{"arguments",Json::array({"_scm-host"})},
        {"service_type","SERVICE_WIN32_OWN_PROCESS"},{"host_account","LocalSystem"},{"runtime_account","logged-on-user-unelevated"},
        {"elevation","installation-and-host-only"},{"instance_creation","existing-and-new-user-sessions"},{"password_stored",false},
        {"recovery_owner","windows-scm-and-owned-jobs"},{"restart_attempts",3},{"restart_delay_seconds",10},
        {"normal_stop_restarts",false},{"installation_performed",false},{"manager_state","not-assessed"}});
    return value;
}
std::optional<Json> start_installed_local_service(const LocalProjectRequest& request,const fs::path& executable) {
    using namespace scm;
    legacy_receipt(request.home); Token token;
    if(fs::absolute(request.home).lexically_normal()!=token.home()) return {};
    auto scm=manager(); auto service=open(scm.value,name,SERVICE_QUERY_CONFIG);
    if(!service.value) return {};
    protected_image(executable); verify(service.value,executable);
    const auto before=host_request(executable,"status");
    if(!before.value("instance_present",false))
        throw Error(before.value("error_code","service-manager-not-ready"),"SCM cannot admit this user's runtime; inspect service-installation.");
    (void)host_request(executable,"start");
    const auto deadline=Clock::now()+std::chrono::seconds(20);
    while(Clock::now()<deadline) {
        auto query=request; query.command="service-status";
        try {
            auto status=request_local_service(query,[&]{return Clock::now()>=deadline;});
            require(status.value("supervision","")==local_service_manager(),"service-manager-conflict","An unmanaged runtime occupies the managed home.");
            status["started"]=!before.value("service_running",false); return status;
        } catch(const Error& error) {
            if(error.code!="service-unavailable" && error.code!="service-disconnected" && error.code!="unsafe-directory") throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw Error("service-manager-not-ready","User Observer did not become ready; inspect service-installation.");
}
// Set only after the child verifies the SCM host parent and its stop event.
bool windows_scm_user_runtime=false;
void check_local_service_supervision(const LocalProjectRequest& request,const fs::path& executable,bool supervised) {
    using namespace scm;
    legacy_receipt(request.home); Token token; token.desktop();
    if(supervised) {
        require(windows_scm_user_runtime && fs::absolute(request.home).lexically_normal()==token.home(),
            "service-managed-only","Only a verified SCM-host child may supervise this home."); return;
    }
    if(fs::absolute(request.home).lexically_normal()!=token.home()) return;
    auto scm=manager(); auto service=open(scm.value,name,SERVICE_QUERY_CONFIG);
    if(service.value) { verify(service.value,executable); throw Error("service-managed-only","SCM host owns this user's default home; use service-start."); }
}
Json local_autostart_status(const LocalProjectRequest&,const fs::path& executable) {
    using namespace scm;
    Token token; auto scm=manager(); auto service=open(scm.value,name,SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS);
    auto value=response("service-installation"); value["phase"]=service.value?"installed":"not-installed";
    if(!service.value) return value;
    protected_image(executable); verify(service.value,executable);
    const auto status=state(service.value);
    value.update({{"service_home",token.home().string()},{"scm_state",status.dwCurrentState},{"host_process_id",status.dwProcessId},
        {"host_running",status.dwCurrentState==SERVICE_RUNNING},{"service_running",false},{"process_id",0},{"instance_present",false}});
    if(status.dwCurrentState==SERVICE_RUNNING) {
        const auto worker=host_request(executable,"status");
        for(const auto* key:{"service_running","process_id","instance_present","worker_state","error_code"}) value[key]=worker.at(key);
    }
    return value;
}
Json configure_local_autostart(const LocalProjectRequest& request,const fs::path& executable) {
    // Setup's elevated hidden entry does not accept a data-home argument.
    // The public CLI must not silently discard a caller's custom home.
    scm::require(fs::absolute(request.home).lexically_normal()==scm::Token().profile_home(),
        "service-home-conflict","The SCM host uses each user's own profile, not a custom shared home.");
    scm::require(request.command=="service-install" || request.command=="service-uninstall","invalid-arguments","Unsupported SCM operation.");
    return windows_scm_setup(request.command=="service-install"?WindowsScmAction::install:WindowsScmAction::remove,executable);
}
Json update_local_autostart(const LocalProjectRequest&,const fs::path&) {
    throw Error("service-update-installer-required","Update the SCM-owned kit through Windows Installer.");
}
}
