#include "local_scm_windows.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_registry.h"
#include "shin/windows/transport.h"
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace shin::observer {
extern bool windows_scm_user_runtime;
namespace scm {
namespace {
std::atomic<bool> stopping{false};
SERVICE_STATUS_HANDLE status_handle=nullptr;
std::mutex status_mutex;
SERVICE_STATUS reported{};
void report(DWORD phase,DWORD error=NO_ERROR) {
    std::lock_guard lock(status_mutex);
    reported.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    reported.dwCurrentState=phase;
    reported.dwControlsAccepted=phase==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    reported.dwWin32ExitCode=error;
    reported.dwWaitHint=phase==SERVICE_START_PENDING || phase==SERVICE_STOP_PENDING?60000:0;
    reported.dwCheckPoint=reported.dwWaitHint?reported.dwCheckPoint+1:0;
    if(!SetServiceStatus(status_handle,&reported)) failed("Cannot report host state");
}
DWORD WINAPI control(DWORD code,DWORD,void*,void*) noexcept {
    try {
        if(code==SERVICE_CONTROL_STOP || code==SERVICE_CONTROL_SHUTDOWN) { stopping=true; report(SERVICE_STOP_PENDING); }
        else if(code==SERVICE_CONTROL_INTERROGATE) { std::lock_guard lock(status_mutex); SetServiceStatus(status_handle,&reported); }
        else return ERROR_CALL_NOT_IMPLEMENTED;
        return NO_ERROR;
    } catch(...) { return ERROR_GEN_FAILURE; }
}
std::unique_ptr<Token> session_token(DWORD session) {
    HANDLE raw=nullptr;
    if(!WTSQueryUserToken(session,&raw)) return {};
    Handle original(raw); TOKEN_ELEVATION_TYPE kind{}; DWORD bytes=0;
    if(!GetTokenInformation(original.value,TokenElevationType,&kind,sizeof(kind),&bytes)) failed("Cannot inspect session elevation");
    if(kind==TokenElevationTypeFull) {
        TOKEN_LINKED_TOKEN linked{};
        if(!GetTokenInformation(original.value,TokenLinkedToken,&linked,sizeof(linked),&bytes)) failed("Cannot obtain filtered user token");
        original=Handle(linked.LinkedToken);
    }
    HANDLE copy=nullptr;
    if(!DuplicateTokenEx(original.value,TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY,nullptr,SecurityImpersonation,TokenPrimary,&copy))
        failed("Cannot duplicate session token");
    auto token=std::make_unique<Token>(copy); token->desktop();
    DWORD actual=0;
    if(!GetTokenInformation(token->handle.value,TokenSessionId,&actual,sizeof(actual),&bytes)) failed("Cannot verify user session");
    require(actual==session,"service-account-denied","Filtered token session differs.");
    return token;
}
struct Session { DWORD id; std::unique_ptr<Token> token; };
std::map<std::string,Session> sessions(std::set<DWORD>& unresolved) {
    PWTS_SESSION_INFOW rows=nullptr; DWORD count=0;
    if(!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&rows,&count)) failed("Cannot enumerate interactive sessions");
    struct Free { void* value; ~Free(){WTSFreeMemory(value);} } free{rows};
    require(count<=64,"service-capacity","Too many Windows sessions.");
    std::map<std::string,Session> result;
    for(DWORD i=0;i<count;++i) {
        if(!rows[i].SessionId || (rows[i].State!=WTSActive && rows[i].State!=WTSDisconnected)) continue;
        std::unique_ptr<Token> token;
        try { token=session_token(rows[i].SessionId); }
        catch(const Error& error) { log_failure(error.code.c_str()); }
        if(!token) { unresolved.insert(rows[i].SessionId); continue; }
        const auto sid=token->key();
        const auto prior=result.find(sid);
        if(prior==result.end() || rows[i].SessionId<prior->second.id)
            result.insert_or_assign(sid,Session{rows[i].SessionId,std::move(token)});
    }
    return result;
}
std::wstring stop_name() {
    std::array<unsigned char,16> bytes{};
    require(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,
        "service-manager-failed","Cannot generate worker stop identity.");
    std::wstring result=L"Global\\ShinObserverStop-";
    for(const auto byte:bytes) { result+=L"0123456789abcdef"[byte>>4]; result+=L"0123456789abcdef"[byte&15]; }
    return result;
}
struct Worker {
    Handle job,process,stop;
    DWORD session=0, pid=0;
    explicit Worker(const fs::path& executable,const Session& user) : job(CreateJobObjectW(nullptr,nullptr)),session(user.id) {
        if(!job.value) failed("Cannot create user process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) failed("Cannot bind user job lifetime");
        const auto key=user.token->key();
        const std::wstring sid(key.begin(),key.end());
        const std::wstring sddl=L"O:SYD:P(A;;GA;;;SY)(A;;0x120000;;;"+sid+L")";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)) failed("Cannot protect worker stop event");
        struct Free { void* value; ~Free(){LocalFree(value);} } free{descriptor};
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
        const auto event=stop_name();
        stop=Handle(CreateEventW(&attributes,TRUE,FALSE,event.c_str()));
        require(stop.value && GetLastError()!=ERROR_ALREADY_EXISTS,"service-manager-failed","Worker stop event identity is occupied.");
        void* environment=nullptr;
        if(!CreateEnvironmentBlock(&environment,user.token->handle.value,FALSE)) failed("Cannot build user environment");
        struct Environment { void* value; ~Environment(){DestroyEnvironmentBlock(value);} } env{environment};
        std::wstring command=L"\""+executable.native()+L"\" _scm-user "+event;
        wchar_t desktop[]=L"winsta0\\default";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.lpDesktop=desktop;
        PROCESS_INFORMATION child{};
        // No inherited SYSTEM handles, caller paths, caller arguments or UI.
        if(!CreateProcessAsUserW(user.token->handle.value,executable.c_str(),command.data(),nullptr,nullptr,FALSE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment,executable.parent_path().c_str(),&startup,&child))
            failed("Cannot start isolated user Observer");
        process=Handle(child.hProcess); Handle thread(child.hThread); pid=child.dwProcessId;
        if(!AssignProcessToJobObject(job.value,process.value) || ResumeThread(thread.value)==static_cast<DWORD>(-1)) {
            TerminateProcess(process.value,1); WaitForSingleObject(process.value,5000); failed("Cannot activate owned user job");
        }
    }
    bool running() const { return WaitForSingleObject(process.value,0)==WAIT_TIMEOUT; }
    ~Worker() {
        if(running()) { TerminateJobObject(job.value,1); WaitForSingleObject(process.value,5000); }
    }
    DWORD exit_code() const { DWORD code=1; if(!GetExitCodeProcess(process.value,&code)) failed("Cannot inspect owned user process"); return code; }
    void request_stop() { SetEvent(stop.value); }
};
struct Slot {
    std::unique_ptr<Worker> worker;
    unsigned failures=0;
    bool paused=false;
    Clock::time_point retry{},last_failure{};
    std::string error;
};
class Host {
    fs::path executable;
    std::map<std::string,Slot> users;
    std::map<std::string,Session> current;
public:
    explicit Host(fs::path image):executable(std::move(image)) {}
    ~Host() {
        for(auto& [sid,slot]:users) if(slot.worker) slot.worker->request_stop();
        const auto deadline=Clock::now()+std::chrono::seconds(45);
        while(Clock::now()<deadline) {
            bool alive=false;
            for(auto& [sid,slot]:users) if(slot.worker && slot.worker->running()) alive=true;
            if(!alive) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Destroying our jobs terminates only our remaining process trees.
    }
    void refresh() {
        std::set<DWORD> unresolved;
        current=sessions(unresolved);
        for(auto it=users.begin();it!=users.end();) {
            // A transient token-query failure is not evidence of sign-out.
            // Keep an already owned live worker, but never launch from a stale token.
            const auto& worker=it->second.worker;
            const bool unknown=worker && worker->running() && unresolved.contains(worker->session);
            if(!current.contains(it->first) && !unknown) { if(worker) worker->request_stop(); it=users.erase(it); }
            else ++it;
        }
        for(const auto& [sid,user]:current) {
            if(!users.contains(sid) && users.size()>=16) continue;
            auto& slot=users[sid];
            if(slot.worker && slot.worker->session!=user.id) {
                slot.worker->request_stop(); slot.worker.reset(); slot.paused=false; slot.failures=0;
            }
            if(slot.worker && slot.worker->running()) continue;
            if(slot.worker) {
                const auto code=slot.worker->exit_code(); slot.worker.reset();
                if(!code) slot.paused=true;
                else failure(slot,"service-user-exited");
            }
            if(slot.paused || slot.failures>=4 || Clock::now()<slot.retry) continue;
            try { slot.worker=std::make_unique<Worker>(executable,user); slot.error.clear(); }
            catch(const Error& error) { failure(slot,error.code); }
        }
    }
    Json request(const std::string& sid,const std::string& action) {
        require(action=="status" || action=="start","invalid-arguments","Only status and start are admitted.");
        auto it=users.find(sid);
        if(action=="start" && it!=users.end() && (!it->second.worker || !it->second.worker->running())) {
            auto& slot=it->second; slot.worker.reset(); slot.paused=false; slot.failures=0; slot.retry={};
            const auto session=current.find(sid);
            if(session!=current.end()) {
                try { slot.worker=std::make_unique<Worker>(executable,session->second); slot.error.clear(); }
                catch(const Error& error) { failure(slot,error.code); }
            }
        }
        const bool exists=it!=users.end();
        const bool running=exists && it->second.worker && it->second.worker->running();
        const auto phase=!exists?"no-user-session":running?"running":it->second.paused?"stopped":it->second.failures>=4?"failed":"preparing";
        return {{"schema_version","shin.scm-user.v1"},{"ok",true},{"instance_present",exists},{"service_running",running},
            {"process_id",running?it->second.worker->pid:0},{"worker_state",phase},
            {"error_code",exists?it->second.error:current.contains(sid)?"service-capacity":"service-user-session-required"}};
    }
private:
    void failure(Slot& slot,const std::string& code) {
        if(Clock::now()-slot.last_failure>std::chrono::hours(24)) slot.failures=0;
        ++slot.failures; slot.last_failure=Clock::now(); slot.retry=Clock::now()+std::chrono::seconds(10);
        slot.error=code; log_failure(code.c_str());
    }
};
}
void WINAPI host_main(DWORD,wchar_t**) noexcept {
    try {
        status_handle=RegisterServiceCtrlHandlerExW(name,control,nullptr);
        if(!status_handle) failed("Cannot register SCM host controls");
        report(SERVICE_START_PENDING);
        Token system; require(IsWellKnownSid(system.sid(),WinLocalSystemSid),"service-account-denied","SCM host requires LocalSystem.");
        const auto executable=current_executable_path(); protected_image(executable);
        auto scm=manager(); auto own=open(scm.value,name,SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS);
        require(own.value!=nullptr,"service-install-changed","SCM host is missing."); verify(own.value,executable);
        require(state(own.value).dwProcessId==GetCurrentProcessId(),"service-account-denied","Process is not the SCM host.");
        {
            auto listener=windows::PipeListener::system_broker(endpoint());
            Host host(executable); host.refresh(); report(SERVICE_RUNNING);
            auto refresh=Clock::now()+std::chrono::seconds(1);
            while(!stopping) {
                if(Clock::now()>=refresh) { host.refresh(); refresh=Clock::now()+std::chrono::seconds(1); }
                try {
                    auto client=listener->try_accept();
                    if(client) {
                        const auto deadline=Clock::now()+std::chrono::milliseconds(500);
                        const auto action=client->read_frame(16,deadline,[]{return stopping.load();});
                        const auto result=host.request(client->peer_sid(),action);
                        client->write_frame(result.dump(),4096,deadline);
                        client->finish_reply(deadline,[]{return stopping.load();});
                    }
                } catch(const windows::TransportError&) {} // A refused or abandoned client cannot stop the host.
                  catch(const Error& error) { if(error.code!="invalid-arguments") throw; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            report(SERVICE_STOP_PENDING);
        }
        report(SERVICE_STOPPED);
    } catch(const Error& error) {
        log_failure(error.code.c_str()); if(status_handle) { try {report(SERVICE_STOPPED,ERROR_GEN_FAILURE);} catch(...){} }
    } catch(...) {
        log_failure("service-host-failed"); if(status_handle) { try {report(SERVICE_STOPPED,ERROR_GEN_FAILURE);} catch(...){} }
    }
}
}
int windows_scm_user(const std::string& event_name) {
    using namespace scm;
    Token user; user.desktop();
    const auto executable=current_executable_path(); protected_image(executable);
    auto scm=manager(); auto host=open(scm.value,name,SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS);
    require(host.value!=nullptr,"service-managed-only","SCM host is missing."); verify(host.value,executable);
    const auto parent=state(host.value).dwProcessId;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    require(snapshot.value!=INVALID_HANDLE_VALUE,"service-managed-only","Cannot verify worker parent.");
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry); bool owned=false;
    if(Process32FirstW(snapshot.value,&entry)) do {
        if(entry.th32ProcessID==GetCurrentProcessId()) { owned=parent && entry.th32ParentProcessID==parent; break; }
    } while(Process32NextW(snapshot.value,&entry));
    BOOL in_job=FALSE;
    require(owned && IsProcessInJob(GetCurrentProcess(),nullptr,&in_job) && in_job,
        "service-managed-only","Only an SCM-host-owned job may launch this Observer.");
    require(event_name.starts_with("Global\\ShinObserverStop-") && event_name.size()==56 &&
        event_name.substr(24).find_first_not_of("0123456789abcdef")==std::string::npos,"service-managed-only","Invalid worker stop event.");
    const std::wstring event(event_name.begin(),event_name.end());
    Handle stop(OpenEventW(SYNCHRONIZE|READ_CONTROL,FALSE,event.c_str()));
    require(stop.value!=nullptr,"service-managed-only","SCM stop event is missing.");
    PSID owner=nullptr; PSECURITY_DESCRIPTOR descriptor=nullptr;
    const auto checked=GetSecurityInfo(stop.value,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor);
    const bool system_owned=checked==ERROR_SUCCESS && owner && IsWellKnownSid(owner,WinLocalSystemSid);
    if(descriptor) LocalFree(descriptor);
    require(system_owned,"service-managed-only","Worker stop event is not SYSTEM-owned.");
    LocalProjectRequest request; request.command="serve"; request.home=user.home();
    ensure_private_directory(request.home);
    { const auto lock=lock_local_service_control(request.home); ensure_local_registry(request.home); }
    windows_scm_user_runtime=true;
    (void)serve_local_projects(request,{},executable,[&]{return WaitForSingleObject(stop.value,0)!=WAIT_TIMEOUT;},true,true);
    return 0;
}
}
