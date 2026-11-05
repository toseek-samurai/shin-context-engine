// Production update orchestration; only public HTTP transport is supplied locally.
#include "shin/observer/local_updates.h"
#include "shin/observer/observer.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <thread>
namespace {
using namespace shin::observer;
fs::path fixtures;
std::string recovery_fetch(const std::string& url,std::size_t limit,const fs::path& output,
                          const ReleaseUpdateProgressSink& progress,std::uint64_t expected) {
    std::ofstream(fixtures/"requests",std::ios::app)<<url<<'\n';
    if(fs::exists(fixtures/"offline"))throw Error("update-network-unavailable","Offline fixture.");
    auto file=fixtures/(url.starts_with("https://api.github.com/") ? "feed.json" : url.ends_with(".sha256") ? "archive.sha256" : "archive.zip");
    auto bytes=read_text(file,limit,false);
    if(url.find("/releases/tags/")!=std::string::npos)bytes=Json::parse(bytes).at(0).dump();
    if(!output.empty()) {
        if(fs::exists(fixtures/"fail-download"))throw Error("update-network-unavailable","Transfer outage fixture.");
        if(!write_private_archive(output,""))throw Error("fixture-write","Cannot stage archive.");
        std::ofstream stream(output,std::ios::binary|std::ios::trunc);
        if(fs::exists(fixtures/"pause-download")) {
            stream.write(bytes.data(),std::min<std::size_t>(1024,bytes.size()));stream.flush();
            std::ofstream(fixtures/"download-paused")<<"ready";
            while(fs::exists(fixtures/"pause-download"))std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        stream.seekp(0);stream.write(bytes.data(),bytes.size());stream.close();
        if(fs::exists(fixtures/"lock-download") && ::chflags(output.c_str(),UF_IMMUTABLE)!=0)
            throw Error("fixture-flags","Cannot protect cleanup fixture.");
        if(!stream)throw Error("fixture-write","Cannot write archive.");report_update(progress,{ReleaseUpdateStep::downloading,bytes.size(),expected});return {};}
    return bytes;
}
}
#define SHIN_UPDATE_FETCH_OVERRIDE recovery_fetch
#include "../src/local_updates_macos.mm"
int main(int argc,char** argv) {
 @autoreleasepool {
    try {
        if(argc<4)return 2;
        fixtures=fs::canonical(argv[1]);
        auto result=run_updater_request(argc-2,argv+2,fs::canonical(getenv("HOME")));
        std::cout<<result.dump()<<'\n';return result.value("ok",false)?0:1;
    } catch(const Error& error) {std::cerr<<Json{{"ok",false},{"error",error_json(error)}}.dump()<<'\n';return 2;}
      catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 3;}
 }
}
