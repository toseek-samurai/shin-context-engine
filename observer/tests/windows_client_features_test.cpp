#include "shin/observer/local_client_setup.h"
#include "shin/observer/model_usage.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/observer.h"
#include "shin/windows/file.h"
#include "../tray/windows_download.h"
#include "../tray/windows_installation.h"
#include <windows.h>
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
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
            require(CreateHardLinkW(alias.c_str(),protected_config.c_str(),nullptr)!=FALSE,"hardlink fixture failed");
            const auto linked_review=local_client_connect(fs::path(argv[1]),project,home,client,alias,"");
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
        for(const auto& file:fs::directory_iterator(original_cli.parent_path())) if(file.path().extension()==".dll") {
            shin::windows::ReadFile library(file.path());
            require(images.create(file.path().filename().string(),library.read(32U*1024*1024)),"private DLL fixture failed");
        }
        const auto discovered=windows_command(commands/"shin-ce.exe",{"client-targets","--home",home.string()});
        require(discovered.value("ok",false) && discovered.at("targets").is_array(),"native command runner failed");
        phase="update payload validation";
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
