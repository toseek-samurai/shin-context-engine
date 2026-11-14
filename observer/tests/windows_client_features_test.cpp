#include "shin/observer/local_client_setup.h"
#include "shin/observer/model_usage.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/observer.h"
#include "shin/windows/file.h"
#include "../ui/project_progress.h"
#include "../tray/windows_download.h"
#include "../tray/windows_installation.h"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <sqlite3.h>
#include <array>
#include <cstring>
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
#include <vector>
#include "../../tests/core/windows_private_acl.h"
using namespace shin::observer;
namespace {
std::wstring wide(const std::string& value) {return windows_update::wide_url(value);}
#include "../tray/windows_command.h"
}
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void refused(F action) {try {action();}catch(const Error&){return;}throw std::runtime_error("unsafe operation succeeded");}
void wait_for_fixture_service_stop(const fs::path& home) {
    // service-stop acknowledges the request before workers, collectors and the
    // endpoint have finished shutting down. Model MSI's completed stop before
    // changing the old binding or trying preparation with the replacement CLI.
    StateStore owner(home/"service",false);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(owner.writer_present()&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(!owner.writer_present(),"fixture service did not finish shutdown");
}
struct InstallerAcl {
    std::unique_ptr<void,decltype(&LocalFree)> descriptor{nullptr,LocalFree};
    PACL acl=nullptr;PSID owner=nullptr;SECURITY_DESCRIPTOR_CONTROL control{};
    explicit InstallerAcl(const fs::path& path) {
        const auto file=CreateFileW(path.c_str(),READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(file!=INVALID_HANDLE_VALUE,"cannot inspect installer ACL fixture");
        PSECURITY_DESCRIPTOR raw=nullptr;
        const auto status=GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
            &owner,nullptr,&acl,nullptr,&raw);
        CloseHandle(file);descriptor.reset(raw);
        require(status==ERROR_SUCCESS&&owner&&acl,"installer ACL was not available");
        DWORD revision=0;
        require(GetSecurityDescriptorControl(raw,&control,&revision)!=FALSE,"cannot inspect installer ACL protection");
    }
    ACCESS_MASK effective(PSID sid) const {
        TRUSTEE_W trustee{};BuildTrusteeWithSidW(&trustee,sid);ACCESS_MASK rights=0;
        require(GetEffectiveRightsFromAclW(acl,&trustee,&rights)==ERROR_SUCCESS,"cannot inspect installer effective rights");
        return rights;
    }
    ACCESS_MASK effective(WELL_KNOWN_SID_TYPE type) const {
        std::array<unsigned char,SECURITY_MAX_SID_SIZE> sid{};DWORD bytes=static_cast<DWORD>(sid.size());
        require(CreateWellKnownSid(type,nullptr,sid.data(),&bytes)!=FALSE,"cannot create installer principal fixture");
        return effective(sid.data());
    }
    std::wstring spelling() const {
        // SetNamedSecurityInfo may mark even a protected DACL auto-inherited.
        // Ignore only that OS bookkeeping bit; preserve owner, ACEs and every
        // other control bit when comparing the restored security policy.
        std::vector<unsigned char> copy(GetSecurityDescriptorLength(descriptor.get()));
        require(!copy.empty(),"cannot copy installer ACL fixture");
        std::memcpy(copy.data(),descriptor.get(),copy.size());
        require(SetSecurityDescriptorControl(copy.data(),SE_DACL_AUTO_INHERITED,0)!=FALSE,
            "cannot normalize installer ACL metadata");
        LPWSTR text=nullptr;
        require(ConvertSecurityDescriptorToStringSecurityDescriptorW(copy.data(),SDDL_REVISION_1,
            OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&text,nullptr)!=FALSE,"cannot encode installer ACL fixture");
        std::unique_ptr<void,decltype(&LocalFree)> cleanup{text,LocalFree};return text;
    }
};
template<class F> void installer_access_refused(F action) {
    bool denied=false;try {action();}catch(const std::exception&) {denied=true;}
    require(denied,"unsafe installer read grant succeeded");
}
void check_installer_system_read_access(const fs::path& root) {
    const auto folder=root/"installer-system-read";ensure_private_directory(folder);
    const auto payload=folder/"package.msi",sibling=folder/"private-state.json";
    const std::string body="independent installer payload";
    write_user_archive(payload,body);write_user_archive(sibling,"private synthetic operation");
    const auto original=InstallerAcl(payload).spelling(),directory_acl=InstallerAcl(folder).spelling(),sibling_acl=InstallerAcl(sibling).spelling();
    require(InstallerAcl(payload).effective(WinLocalSystemSid)==0,"fixture already grants SYSTEM access");
    {
        windows_update::InstallerReadAccess access(payload);
        const InstallerAcl shared(payload);
        require((shared.control&SE_DACL_PROTECTED)!=0&&shared.acl->AceCount==2,"installer ACL is inherited or grants an extra principal");
        require(shared.effective(shared.owner)==FILE_ALL_ACCESS,"installer grant changed owner access");
        require(shared.effective(WinLocalSystemSid)==FILE_GENERIC_READ,"SYSTEM must receive precisely read access");
        require(shared.effective(WinWorldSid)==0,"installer grant exposed the payload to other users");
        require(InstallerAcl(folder).spelling()==directory_acl&&InstallerAcl(sibling).spelling()==sibling_acl,"installer grant widened neighboring permissions");
        const auto writer=CreateFileW(payload.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        const auto write_error=GetLastError();if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
        require(writer==INVALID_HANDLE_VALUE&&write_error==ERROR_SHARING_VIOLATION,"verified installer was writable while handed off");
        require(!MoveFileExW(payload.c_str(),(folder/"replaced.msi").c_str(),0)&&GetLastError()==ERROR_SHARING_VIOLATION,
            "verified installer could be replaced while handed off");
    }
    require(InstallerAcl(payload).spelling()==original,"scope completion did not restore the original installer ACL");
    {
        windows_update::InstallerReadAccess access(payload);
        require(access.restore(),"explicit installer permission restoration failed");
        require(access.restore(),"installer permission restoration is not idempotent");
        require(InstallerAcl(payload).spelling()==original,"explicit restoration left SYSTEM access behind");
    }
    require(read_text(payload,body.size(),true)==body,"installer handoff changed payload bytes or private ownership");
}
void check_installer_system_read_access_restores_after_exception(const fs::path& root) {
    const auto payload=root/"interrupted-installer.msi";write_user_archive(payload,"independent interrupted payload");
    const auto original=InstallerAcl(payload).spelling();bool interrupted=false;
    try {
        windows_update::InstallerReadAccess access(payload);
        require(InstallerAcl(payload).effective(WinLocalSystemSid)==FILE_GENERIC_READ,"interruption fixture never granted SYSTEM read access");
        interrupted=true;throw std::runtime_error("synthetic installer interruption");
    }catch(const std::runtime_error&) {}
    require(interrupted,"installer interruption did not reach the protected scope");
    require(InstallerAcl(payload).spelling()==original,"exception exit left installer permissions widened");
}
void installer_fixture_junction(const fs::path& link,const fs::path& target) {
    std::array<wchar_t,32768> system{};const auto length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
    require(length>0&&length<system.size(),"fixture system directory unavailable");
    const auto executable=fs::path(system.data())/"cmd.exe";
    auto command=L"cmd.exe /d /c mklink /J \""+link.wstring()+L"\" \""+target.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,
        "cannot launch installer reparse fixture");
    const auto waited=WaitForSingleObject(child.hProcess,10000);DWORD code=1;
    if(waited!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,1);
    GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hThread);CloseHandle(child.hProcess);
    require(waited==WAIT_OBJECT_0&&code==0,"installer reparse fixture failed");
}
void check_installer_system_read_access_rejects_unsafe_payloads(const fs::path& root) {
    const auto folder=root/"installer-refusals";ensure_private_directory(folder);
    const auto payload=folder/"package.msi";write_user_archive(payload,"independent refusal payload");
    const auto original=InstallerAcl(payload).spelling();
    installer_access_refused([&]{windows_update::InstallerReadAccess access(folder/"missing.msi");});
    require(!fs::exists(folder/"missing.msi"),"missing installer refusal created a file");
    const auto folder_acl=InstallerAcl(folder).spelling();
    installer_access_refused([&]{windows_update::InstallerReadAccess access(folder);});
    require(InstallerAcl(folder).spelling()==folder_acl,"directory refusal changed permissions");
    const auto alias=folder/"hardlink.msi";
    require(CreateHardLinkW(alias.c_str(),payload.c_str(),nullptr)!=FALSE,"installer hardlink fixture failed");
    installer_access_refused([&]{windows_update::InstallerReadAccess access(alias);});
    require(InstallerAcl(payload).spelling()==original,"hardlink refusal changed the shared file ACL");
    require(DeleteFileW(alias.c_str())!=FALSE,"installer hardlink fixture cleanup failed");
    const auto junction=root/"installer-junction";installer_fixture_junction(junction,folder);
    installer_access_refused([&]{windows_update::InstallerReadAccess access(junction/"package.msi");});
    require(InstallerAcl(payload).spelling()==original,"reparse refusal changed the target ACL");
    require(RemoveDirectoryW(junction.c_str())!=FALSE,"installer reparse fixture cleanup failed");
    fixture_private_acl(payload,true);
    const auto shared=InstallerAcl(payload).spelling();
    installer_access_refused([&]{windows_update::InstallerReadAccess access(payload);});
    require(InstallerAcl(payload).spelling()==shared,"non-private installer refusal rewrote existing permissions");
    fixture_private_acl(payload);
    require(read_text(payload,128,true)=="independent refusal payload","installer refusals changed the original payload");
}
void check_failed_update_review_policy() {
    const auto check=[](const Json& receipt,bool expected) {
        const auto original=receipt.dump();
        require(windows_update::update_can_retry_without_install(receipt)==expected,"unexpected retry classification for saved update");
        require(receipt.dump()==original,"retry classification mutated the saved receipt");
    };
    for(const auto* phase:{"reviewed","checking","downloading","verifying","waiting-monitor"})check(Json{{"phase",phase}},true);
    for(const auto* code:{"update-uac-cancelled","update-offer-changed","update-download-invalid","update-helper-launch-failed","update-monitor-running"})
        check(Json{{"phase","review-required"},{"error_code",code}},true);
    check(Json{{"phase","review-required"},{"error_code","update-msi-failed"},{"installer_exit_code",1619}},true);
    for(const auto code:{0,1603,1618,1620,3010,1641})
        check(Json{{"phase","review-required"},{"error_code","update-msi-failed"},{"installer_exit_code",code}},false);
    for(const auto* phase:{"installing","preparing-projects","completed","acknowledged","unknown",""})
        check(Json{{"phase",phase},{"error_code","update-msi-failed"},{"installer_exit_code",1619}},false);
    check(Json{{"phase","review-required"},{"error_code","unknown"},{"installer_exit_code",1619}},false);
    check(Json{{"phase","review-required"},{"error_code","update-msi-failed"}},false);
    for(const auto& code:Json::array({"1619",1619.0,true,nullptr}))
        check(Json{{"phase","review-required"},{"error_code","update-msi-failed"},{"installer_exit_code",code}},false);
    for(const auto& malformed:Json::array({nullptr,Json::array(),Json::object(),Json{{"phase",7}},Json{{"phase",nullptr}},
        Json{{"phase","review-required"},{"error_code",7},{"installer_exit_code",1619}}}))check(malformed,false);
}
int main(int argc,char** argv) {
    const char* phase="client configuration";
    try {
        const bool installed_preflight=argc==4&&std::string_view(argv[1])=="--installed-update-preflight";
        require(argc==3||installed_preflight,"pass matching CLI and profiles");
        HANDLE access_token=nullptr;TOKEN_ELEVATION elevation{};DWORD token_size=0;
        require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&access_token)!=FALSE,"cannot inspect fixture token");
        const bool inspected=GetTokenInformation(access_token,TokenElevation,&elevation,sizeof(elevation),&token_size)!=FALSE;
        CloseHandle(access_token);
        require(inspected&&!elevation.TokenIsElevated,"run client/history and post-MSI fixtures as an unelevated user");
        if(installed_preflight) {
            const fs::path prefix(argv[2]),folder(argv[3]);
            windows_update::verify_registered_msi_installation(prefix);
            auto report=windows_update::stage_update_helper(prefix,folder);
            report["ok"]=true;report["msi_root_verified"]=true;
            std::cout<<report.dump()<<'\n';return 0;
        }
        const auto root=fs::temp_directory_path()/("shin client features-"+digest(std::to_string(GetCurrentProcessId())+std::to_string(now_ms())).substr(0,20));
        ensure_private_directory(root,true);
        struct Cleanup {fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}} cleanup{root};
        const auto project=root/"project",home=root/"data";ensure_private_directory(project);ensure_private_directory(home);
        const std::pair<const char*,const char*> fixtures[]{
            {"codex","# preserve\nmodel=\"keep\"\n[mcp_servers.other]\ncommand=\"other\"\n"},
            {"opencode","{/* preserve */\"model\":\"keep\",\"mcp\":{\"other\":{\"type\":\"local\",\"command\":[\"other\"]}}}"},
            {"command-code","{\"model\":\"keep\",\"mcpServers\":{\"other\":{\"command\":\"other\"}}}"},
            {"hermes","# preserve\nmodel: keep\nmcp_servers:\n  other:\n    command: other\n"}};
        for(const auto& [client,original]:fixtures) {
            const auto folder=root/client;ensure_private_directory(folder);const auto config=folder/"settings";
            write_user_archive(config,original);
            auto preview=local_client_connect(fs::path(argv[1]),project,home,client,config,"");
            require(read_text(config,kMaximumStateBytes,false)==original,"preview modified settings");
            const auto applied=local_client_connect(fs::path(argv[1]),project,home,client,config,preview.at("revision"));
            require(applied.at("client_modified")==true,"settings were not connected");
            const auto content=read_text(config,kMaximumStateBytes,false);
            require(content.find("keep")!=std::string::npos&&content.find("other")!=std::string::npos,"unrelated settings lost");
            require(read_text(applied.at("backup").get<std::string>(),kMaximumStateBytes,true)==original,"backup changed");
            require(!local_client_connect(fs::path(argv[1]),project,home,client,config,"").at("changed").get<bool>(),"connection not idempotent");
            const auto protected_config=folder/"linked-source",alias=folder/"hardlink";
            write_user_archive(protected_config,original);
            write_user_archive(alias,original);
            const auto linked_review=local_client_connect(fs::path(argv[1]),project,home,client,alias,"");
            require(DeleteFileW(alias.c_str())!=FALSE,"reviewed config replacement failed");
            require(CreateHardLinkW(alias.c_str(),protected_config.c_str(),nullptr)!=FALSE,"hardlink fixture failed");
            refused([&]{local_client_connect(fs::path(argv[1]),project,home,client,alias,"");});
            refused([&]{local_client_connect(fs::path(argv[1]),project,home,client,alias,linked_review.at("revision"));});
            require(DeleteFileW(alias.c_str())!=FALSE,"hardlink fixture cleanup failed");
            require(read_text(protected_config,kMaximumStateBytes,false)==original,"hardlink refusal changed settings");
            const auto next=folder/"new-settings";
            preview=local_client_connect(fs::path(argv[1]),project,home,client,next,"");
            write_user_archive(next," ");
            refused([&]{local_client_connect(fs::path(argv[1]),project,home,client,next,preview.at("revision"));});
            require(read_text(next,kMaximumStateBytes,false)==" ","stale review overwrote settings");
        }
        // WSL-transferred build folders can grant a second Windows account write
        // access. Exercise the runner from an owner-private disposable image;
        // keep its production executable-owner check intact.
        phase="native command runner";
        const auto commands=root/"commands";ensure_private_directory(commands);
        shin::windows::Directory images(commands,shin::windows::Directory::Access::private_owner);
        const fs::path original_cli(argv[1]);shin::windows::ReadFile binary(original_cli);
        require(images.create("shin-ce.exe",binary.read(64U*1024*1024)),"private command fixture failed");
        shin::windows::ReadFile mcp(original_cli.parent_path()/"shin-mcp.exe");
        require(images.create("shin-mcp.exe",mcp.read(64U*1024*1024)),"private MCP fixture failed");
        for(const auto& file:fs::directory_iterator(original_cli.parent_path())) if(file.path().extension()==".dll") {
            shin::windows::ReadFile library(file.path());
            require(images.create(file.path().filename().string(),library.read(32U*1024*1024)),"private DLL fixture failed");
        }
        const auto discovered=windows_command(commands/"shin-ce.exe",{"client-targets","--home",home.string()});
        require(discovered.value("ok",false) && discovered.at("targets").is_array(),"native command runner failed");
        const auto codex_settings=root/"batch codex.toml", opencode_settings=root/"batch opencode.json";
        write_user_archive(codex_settings,"model=\"keep\"\n");
        write_user_archive(opencode_settings,"{}\n");
        Json targets=Json::array({{{"client","codex"},{"config",codex_settings.string()}},
                                  {{"client","opencode"},{"config",opencode_settings.string()}}});
        std::vector<std::string> args{"client-connect-batch","--home",home.string(),"--root",project.string()};
        // More than a Windows command line can hold, with no stdin pipe backpressure.
        const auto preview_batch=windows_command(commands/"shin-ce.exe",args,std::chrono::seconds(10),{},nullptr,
            targets.dump()+std::string(35000,' '));
        require(preview_batch.at("ok")==true && preview_batch.at("results").size()==2,"batch review lost a selected agent");
        for (std::size_t i=0;i<targets.size();++i) targets[i]["revision"]=preview_batch.at("results").at(i).at("revision");
        const auto revised=local_client_connect(commands/"shin-ce.exe",project,home,"opencode",opencode_settings,"");
        local_client_connect(commands/"shin-ce.exe",project,home,"opencode",opencode_settings,revised.at("revision"));
        const auto edited=read_text(opencode_settings,kMaximumStateBytes,false);
        args.push_back("--apply");
        const auto partial=windows_command(commands/"shin-ce.exe",args,std::chrono::seconds(10),{},nullptr,targets.dump());
        require(partial.at("ok")==false && partial.at("results").at(0).at("client_modified")==true &&
            partial.at("results").at(1).at("error").at("code")=="client-config-changed" &&
            read_text(opencode_settings,kMaximumStateBytes,false)==edited,"batch application lost partial success or overwrote an edited file");
        phase="update payload validation";
        check_installer_system_read_access(root);
        check_installer_system_read_access_restores_after_exception(root);
        check_installer_system_read_access_rejects_unsafe_payloads(root);
        check_failed_update_review_policy();
        windows_update::verify_msi_install_root(commands,commands);
        windows_update::verify_msi_install_root(commands,fs::path(commands.wstring()+L"\\"));
        windows_update::verify_msi_install_root(commands,fs::path(commands.wstring()+L"/"));
        refused([&]{windows_update::verify_msi_install_root(commands,project);});
        refused([&]{windows_update::verify_msi_install_root(commands,commands/L".");});
        refused([&]{windows_update::verify_msi_install_root(commands,commands/L".."/L"commands");});
        bool restore_pending=false;std::string restore_order;
        const auto verify_monitor=[&]{restore_order+='V';};
        const auto launch_monitor=[&]{restore_order+='L';};
        windows_update::restore_update_monitor(restore_pending,verify_monitor,launch_monitor);
        require(restore_order.empty(),"Monitor restarted before handoff");
        restore_pending=true;
        refused([&]{windows_update::restore_update_monitor(restore_pending,[]{throw Error("update-outcome-unknown","Synthetic invalid installation");},launch_monitor);});
        require(restore_pending&&restore_order.empty(),"unverified Monitor was launched");
        refused([&]{windows_update::restore_update_monitor(restore_pending,verify_monitor,[]{throw Error("monitor-launch-failed","Synthetic launch failure");});});
        require(restore_pending&&restore_order=="V","failed Monitor launch was treated as complete");
        restore_order.clear();
        windows_update::restore_update_monitor(restore_pending,verify_monitor,launch_monitor);
        windows_update::restore_update_monitor(restore_pending,verify_monitor,launch_monitor);
        require(!restore_pending&&restore_order=="VL","Monitor recovery was repeated or bypassed verification");
        const std::string body="independent synthetic installer";
        Json offer{{"bytes",body.size()},{"sha256",digest(body)},{"archive_name","test.msi"}};
        windows_update::verify(offer,body,digest(body)+"  test.msi\r\n");
        refused([&]{windows_update::verify(offer,body+"x",digest(body)+"  test.msi");});
        refused([&]{windows_update::verify(offer,body,digest(body)+"  other.msi");});
        refused([&]{windows_update::verify(offer,body,"invalid  test.msi");});
        refused([&]{windows_update::download(L"http://github.com/unsafe",32);});
        refused([&]{windows_update::download(L"https://example.test/unsafe",32);});
        refused([&]{windows_update::download(L"https://user:pass@github.com/unsafe",32);});
        Json inventory{{"schema_version","shin.local-kit.v1"},{"version","synthetic"},{"files",Json::array()}};
        for(const auto* name:{"shin-ce.exe","shin-context.exe","shin-observer.exe","shin-mcp.exe","shin-tray.exe","shin-windows-updater.exe"})
            inventory["files"].push_back({{"path",std::string("bin/")+name},{"size",1},{"sha256",digest("x")}});
        windows_update::validate_inventory(inventory,"synthetic");
        refused([&]{windows_update::validate_inventory(inventory,"other-version");});
        auto invalid=inventory;invalid["files"].push_back(inventory["files"][0]);
        refused([&]{windows_update::validate_inventory(invalid,"synthetic");});
        for(const auto* path:{"../escape","C:/outside","bin/file:stream","bin/file.","bin/file ","bin/SHIN-CE.EXE"}) {
            invalid=inventory;invalid["files"].push_back({{"path",path},{"size",1},{"sha256",digest("x")}});
            refused([&]{windows_update::validate_inventory(invalid,"synthetic");});
        }
        invalid=inventory;invalid["files"].erase(0);
        refused([&]{windows_update::validate_inventory(invalid,"synthetic");});
        invalid=inventory;invalid["files"][0]["size"]=128ULL*1024*1024+1;
        refused([&]{windows_update::validate_inventory(invalid,"synthetic");});
        invalid=inventory;for(auto& file:invalid["files"]) file["size"]=128ULL*1024*1024;
        refused([&]{windows_update::validate_inventory(invalid,"synthetic");});
        phase="live history collection";
        const auto sessions=root/"sessions",archives=root/"archives";ensure_private_directory(sessions);ensure_private_directory(archives);
        const auto log=sessions/"rollout.jsonl",database=root/"state_5.sqlite";
        const std::int64_t now=now_ms();
        const auto seconds=static_cast<std::time_t>(now/1000);std::tm utc{};gmtime_s(&utc,&seconds);
        char timestamp[32]{};require(std::strftime(timestamp,sizeof(timestamp),"%Y-%m-%dT%H:%M:%S.000Z",&utc)>0,"usage timestamp fixture");
        Json usage{{"input_tokens",100},{"output_tokens",20},{"cached_input_tokens",60},{"reasoning_output_tokens",5},{"total_tokens",120}};
        const auto meta=Json{{"type","session_meta"},{"payload",{{"id","one"}}}};
        const auto context=Json{{"type","turn_context"},{"payload",{{"model","synthetic-model"}}}};
        const auto token=Json{{"timestamp",timestamp},{"type","event_msg"},{"payload",{{"type","token_count"},{"info",{{"total_token_usage",usage},{"last_token_usage",usage}}}}}};
        write_user_archive(log,meta.dump()+"\n"+context.dump()+"\n"+token.dump()+"\n"+token.dump()+"\n");
        { sqlite3* raw=nullptr;require(sqlite3_open(database.string().c_str(),&raw)==SQLITE_OK,"sqlite open");
          std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
          require(sqlite3_exec(raw,"CREATE TABLE threads(id TEXT,rollout_path TEXT,cwd TEXT,created_at_ms INTEGER,created_at INTEGER,updated_at_ms INTEGER,updated_at INTEGER)",nullptr,nullptr,nullptr)==SQLITE_OK,"sqlite schema");
          sqlite3_stmt* insert=nullptr;require(sqlite3_prepare_v2(raw,"INSERT INTO threads VALUES('one',?1,?2,0,0,?3,0)",-1,&insert,nullptr)==SQLITE_OK,"sqlite prepare");
          std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(insert,sqlite3_finalize);
          const auto path=log.string();auto cwd=project.generic_string();cwd.replace(cwd.size()-7,7,"PROJECT");sqlite3_bind_text(insert,1,path.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(insert,2,cwd.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(insert,3,now);
          require(sqlite3_step(insert)==SQLITE_DONE,"sqlite insert"); }
        Json config{{"schema_version","shin.usage-collector.v1"},{"projects",Json::array({{{"id","synthetic"},{"name","Project"},{"paths",Json::array({project.string()})}}})},
          {"sources",{{"codex",{{"database",database.string()},{"sessions",sessions.string()},{"archives",archives.string()}}},
            {"opencode",{{"database",(root/"missing-open.db").string()}}},{"hermes",{{"database",(root/"missing-hermes.db").string()}}}}}};
        struct Writer { HANDLE value;~Writer(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);} } writer{
            CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
        require(writer.value!=INVALID_HANDLE_VALUE,"live history writer fixture");
        bool immutable_refused=false;
        try {shin::windows::ReadFile source(log);} catch(const std::exception&) {immutable_refused=true;}
        require(immutable_refused,"Core immutable sharing was weakened");
        {shin::windows::ReadFile source(log,shin::windows::ReadFile::Sharing::concurrent_writer);
         require(!MoveFileExW(log.c_str(),(root/"replaced.jsonl").c_str(),0),"history file could be replaced while pinned");}
        sqlite3* live_raw=nullptr;require(sqlite3_open(database.string().c_str(),&live_raw)==SQLITE_OK,"live database fixture");
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> live(live_raw,sqlite3_close);
        require(sqlite3_exec(live_raw,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK,"live database writer lease");
        const auto first=collect_model_usage(config,now),second=collect_model_usage(config,now+1);
        if(first.at("sources").at("codex").at("state")!="available")
            std::cerr<<"Codex fixture source: "<<first.at("sources").at("codex").dump()<<'\n';
        require(first.at("sources").at("codex").at("state")=="available","Windows Codex source unavailable");
        require(first.at("buckets").size()==1&&first.at("buckets").at(0).at("records")==1,"Windows usage dedup failed");
        require(first.at("buckets")==second.at("buckets"),"Windows repeat counts changed");
        phase="enrolled project collection";
        const auto runtime_home=root/"runtime-data";
        struct Service {fs::path cli,home;~Service(){try {windows_command(cli,{"service-stop","--home",home.string()});wait_for_fixture_service_stop(home);}catch(...) {}}} service{commands/"shin-ce.exe",runtime_home};
        write_user_archive(project/"sample.cpp","int syntheticContext() { return 7; }\n");
        require(SetEnvironmentVariableW(L"CODEX_HOME",root.c_str())!=FALSE,"usage environment fixture");
        require(SetEnvironmentVariableW(L"XDG_DATA_HOME",(root/"absent-data").c_str())!=FALSE,"usage environment fixture");
        require(SetEnvironmentVariableW(L"HERMES_HOME",(root/"absent-hermes").c_str())!=FALSE,"usage environment fixture");
        const auto profiles=root/"profiles";ensure_private_directory(profiles);
        const auto profile_text=read_text(fs::canonical(fs::path(argv[2]))/"generic-source.json",kMaximumStateBytes,false);
        write_user_archive(profiles/"generic-source.json",profile_text);
        const auto added=windows_command(service.cli,{"project-add","--home",runtime_home.string(),"--root",project.string(),"--profile-dir",profiles.string()});
        require(added.value("ok",false),"Windows project onboarding failed");
        bool collected=false;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        while(std::chrono::steady_clock::now()<until) {
            try {
                const auto state=parse_json(read_text(runtime_home/"usage/state.json",kMaximumStateBytes,true));
                const auto& snapshot=state.at("snapshot");
                if(snapshot.at("sources").at("codex").at("state")=="available" && snapshot.at("buckets").size()==1) {collected=true;break;}
            }catch(...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(!collected) try {
            const auto health=parse_json(read_text(runtime_home/"usage-health/state.json",kMaximumStateBytes,true));
            std::cerr<<"usage health: "<<health.value("error","")<<'\n';
        }catch(...) {}
        require(collected,"Windows project enrollment did not start automatic collection");
        require(windows_command(service.cli,{"service-stop","--home",runtime_home.string()}).value("ok",false),"fixture service stop failed");
        wait_for_fixture_service_stop(runtime_home);
        const auto workspace=runtime_home/"projects"/digest(project.string());
        Json original_binding;
        {StateStore state(workspace,true);state.recover_pending();original_binding=state.load().value();
         auto previous=original_binding;previous["core_version"]="2.0.5-dev.12";state.save(previous);}
        phase="post-MSI Core preparation";
        const auto prepared=windows_command(service.cli,{"prepare-projects","--home",runtime_home.string()});
        if(!prepared.value("ok",false)) std::cerr<<"Core preparation result: "<<prepared.dump()<<'\n';
        require(prepared.value("ok",false)&&prepared.at("projects").size()==1,"post-MSI Core preparation failed");
        {StateStore state(workspace,false);const auto binding=state.load().value();
         require(binding.at("core_version")==SHIN_VERSION,"Core version was not transferred");
         for(const auto* key:{"root","root_identity","workspace_identity","profile_config_hash","profile_dir","generator_version"})
             require(binding.at(key)==original_binding.at(key),"post-MSI preparation changed project authority or policy");}
        require(windows_command(service.cli,{"prepare-projects","--home",runtime_home.string(),"--check"}).value("ok",false),"read-only completion check failed");
        LocalProjectRequest stale;stale.command="init";stale.root=project;stale.home=runtime_home;
        stale.maintenance_lifecycle_revision=local_project_lifecycle(workspace).revision+1;
        refused([&]{local_project(stale);});
        phase="policy refusal and detach";
        const auto blocked_root=root/"blocked-project",blocked_profiles=root/"blocked-profiles";
        ensure_private_directory(blocked_root);ensure_private_directory(blocked_profiles);
        write_user_archive(blocked_root/"sample.cpp","int blockedSample() { return 3; }\n");
        const auto blocked_profile=blocked_profiles/"generic-source.json";write_user_archive(blocked_profile,profile_text);
        require(windows_command(service.cli,{"project-add","--home",runtime_home.string(),"--root",blocked_root.string(),"--profile-dir",blocked_profiles.string()}).value("ok",false),"second fixture enrollment failed");
        replace_user_archive(blocked_profile,profile_text,profile_text+"\n",directory_identity(blocked_profiles));
        const auto denied=windows_command(service.cli,{"prepare-projects","--home",runtime_home.string()});
        require(!denied.value("ok",true),"changed policy was silently approved");
        bool healthy=false,refusal=false;
        for(const auto& row:denied.at("projects")) {
            if(row.at("root")==project.string()) healthy=row.value("ok",false);
            if(row.at("root")==blocked_root.string()) refusal=!row.value("ok",true)&&row.value("error_code","")=="profile-policy-revision-conflict";
        }
        require(healthy&&refusal,"partial preparation lost the good project or policy refusal");
        require(read_text(blocked_profile,kMaximumStateBytes,false)==profile_text+"\n","preparation rewrote a pending profile");
        replace_user_archive(blocked_profile,profile_text+"\n",profile_text,directory_identity(blocked_profiles));
        require(windows_command(service.cli,{"detach","--home",runtime_home.string(),"--root",blocked_root.string()}).value("ok",false),"fixture detach failed");
        const auto recovered=windows_command(service.cli,{"prepare-projects","--home",runtime_home.string()});
        require(recovered.value("ok",false)&&recovered.at("projects").size()==1,"preparation re-enrolled a detached project");
        std::cout<<"Windows client preservation, active history, post-MSI Core transition, strict policy refusal and detach checks passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<phase<<": "<<error.what()<<'\n';return 1;}
}
