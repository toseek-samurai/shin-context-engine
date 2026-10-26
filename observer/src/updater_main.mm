#import <Foundation/Foundation.h>
#include "shin/observer/local_updates.h"
#include "shin/observer/updater_bundle.h"
#include "shin/observer/observer.h"
#include "../ui/update_window_macos.h"
#include <cstdio>
#include <iostream>
extern char** environ;
using namespace shin::observer;
namespace {
int updater_main(int argc,char** argv) {
 @autoreleasepool {
    fs::path temporary;
    auto cleanup=[&] {
        if(temporary.empty())return;
        // A fresh engine owns this locator only after it has taken update
        // authority. Preserve it whenever the saved outcome still references it.
        bool retained=false;
        for(int i=2;i+1<argc;++i)if(std::string_view(argv[i])=="--home") {
            const fs::path updates=fs::path(argv[i+1])/"updates";
            if(fs::exists(updates)) {
                try {const auto state=StateStore(updates,false).load();retained=state && state->contains("updater") && state->at("updater").value("directory","")==temporary.string();}
                catch(...) {retained=true;}
            }
        }
        if(!retained) {std::error_code error;fs::remove_all(temporary,error);}
    };
    try {
        if(argc==2 && std::string_view(argv[1])=="--protocol") {
            std::cout<<Json{{"schema_version","shin.updater-protocol.v1"},{"protocol",1},{"version",SHIN_OBSERVER_VERSION}}.dump()<<'\n';return 0;
        }
        if(argc==4 && std::string_view(argv[1])=="--update-progress")
            return release_update_progress_viewer(argv[2],shin::ui::language_for(argv[3]));
        if(argc<2)throw Error("invalid-arguments","Choose an updater operation.");
        const auto bundle=fs::canonical(current_executable_path()).parent_path().parent_path();
        const auto manifest=verify_updater_bundle(bundle,bundle.filename()!=updater_bundle_name);
        if(manifest.at("version")!=SHIN_OBSERVER_VERSION)throw Error("update-bootstrap-protocol","Updater executable and envelope versions differ.");
        const auto stage=bundle.parent_path().parent_path();
        const std::string command=argv[1];
        const bool applying=command=="update-install" || command=="update-download" || command=="update-acknowledge";
        if(applying && stage.parent_path()==fs::canonical("/tmp") && stage.filename().string().starts_with("shin-updater."))temporary=stage;
        std::string account;
        for(auto** item=environ;item && *item;++item)if(std::string_view(*item).starts_with("HOME="))account=*item+5;
        if(account.empty())throw Error("home-unavailable","HOME is required for the updater.");
        auto result=std::string_view(argv[1])=="install-local" ? install_local_release(argc,argv,fs::canonical(account)) :
            run_updater_request(argc,argv,fs::canonical(account));
        result["updater_version"]=SHIN_OBSERVER_VERSION;result["tool_version"]=SHIN_OBSERVER_VERSION;result["tool"]="shin-ce";
        std::cout<<result.dump(2)<<'\n';cleanup();return result.value("ok",false) ? 0 : 1;
    } catch(const Error& error) {
        std::cerr<<Json{{"ok",false},{"error",error_json(error)},{"tool","shin-ce"},{"schema_version","shin.ce-response.v1"}}.dump()<<'\n';cleanup();return 2;
    } catch(const std::exception&) {
        std::cerr<<"{\"ok\":false,\"error\":{\"code\":\"update-outcome-unknown\",\"message\":\"Updater could not confirm completion. Review the saved result.\"}}\n";cleanup();return 2;
    }
 }
}

} // namespace
int main(int argc,char** argv) {
    try {return updater_main(argc,argv);}
    catch(...) {
        std::fputs("{\"ok\":false,\"error\":{\"code\":\"update-outcome-unknown\",\"message\":\"Updater could not confirm completion. Review the saved result.\"}}\n",stderr);
        return 2;
    }
}
