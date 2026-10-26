// Compile only the HTTP fixture seam. All envelope verification, child execution,
// ABI handshakes and bootstrap handoff below are the production implementation.
#include "shin/observer/local_updates.h"
#include "shin/observer/updater_bundle.h"
#include "shin/observer/observer.h"
#include <iostream>
namespace {
using namespace shin::observer;
fs::path fixtures;
std::string fixture_fetch(const std::string& url,std::size_t limit,const fs::path& output,
                          const ReleaseUpdateProgressSink& progress,std::uint64_t expected) {
    const auto path=url.starts_with("https://api.github.com/") ? fixtures/"feed.json" :
        url.ends_with(".sha256") ? fixtures/"updater.sha256" : fixtures/"updater.zip";
    const auto bytes=read_text(path,limit,false);
    if(!output.empty()) {if(!write_private_archive(output,bytes))throw Error("fixture-write","Cannot stage fixture.");report_update(progress,{ReleaseUpdateStep::downloading,bytes.size(),expected});return "";}
    return bytes;
}
}
#define SHIN_UPDATE_FETCH_OVERRIDE fixture_fetch
#include "../src/local_update_bootstrap_macos.mm"
int main(int argc,char** argv) {
 @autoreleasepool {
    try {
        if(argc<4)return 2;
        fixtures=fs::canonical(argv[1]);const auto prefix=fs::canonical(argv[2]),home=fs::canonical(argv[3]);
        if(argc==4) {
            Json receipt{{"prefix",prefix.string()},{"prefix_identity",directory_identity(prefix,true)},
                {"home",home.string()},{"home_identity",directory_identity(home,true)}};
            write_private_archive(prefix/".shin-installation.json",receipt.dump());
            std::cout<<"{}\n";return 0;
        }
        if(std::string_view(argv[4])=="pin" && argc==6) {
            const auto stage=fs::canonical(argv[5]),bundle=stage/"payload"/updater_bundle_name;
            (void)verify_updater_bundle(bundle);ensure_private_directory(home/"updates");
            StateStore state(home/"updates",true);
            state.save({{"ok",false},{"phase","review-required"},{"updater",{{"directory",stage.string()},
                {"identity",directory_identity(stage,true)},{"manifest_sha256",digest(read_text(bundle/"shin-updater.json",64U*1024,false))}}}});
            std::cout<<"{}\n";return 0;
        }
        const bool execute=std::string_view(argv[4])=="exec-update-install";
        std::vector<std::string> words{"shin-ce",execute ? "update-install" : argv[4],"--prefix",prefix.string(),"--home",home.string()};
        for(int i=5;i<argc;++i)words.emplace_back(argv[i]);
        std::vector<char*> args;for(auto& word:words)args.push_back(word.data());
        ReleaseUpdateProgressSink observe;if(!execute)observe=[](auto){};
        auto result=local_updates(static_cast<int>(args.size()),args.data(),fs::canonical(getenv("HOME")),observe);
        std::cout<<result.dump()<<'\n';return result.value("ok",false) ? 0 : 1;
    } catch(const Error& error) {std::cerr<<Json{{"code",error.code},{"message",error.what()}}.dump()<<'\n';return 2;}
      catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 3;}
 }
}
