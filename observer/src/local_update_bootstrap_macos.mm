#import <Foundation/Foundation.h>
#include "shin/observer/local_updates.h"
#include "shin/observer/updater_bundle.h"
#include "shin/observer/observer.h"
#include "../ui/language.h"
#include "../ui/update_progress.h"
#include <chrono>
#include <thread>
#include <set>
#include <iostream>
#include <cstdio>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>
#include "local_update_transport_macos.inc"

namespace shin::observer {
namespace {
// This locator is deliberately independent of kit manifests and native journals.
fs::path bootstrap_home(const fs::path& prefix, const fs::path& requested) {
    require(fs::exists(prefix/".shin-installation.json"),"installation-not-owned","No owned installation exists at this prefix.");
    const auto receipt=parse_json(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true));
    require(receipt.value("prefix","")==prefix.string() && receipt.value("prefix_identity","")==directory_identity(prefix,true),
        "installation-ownership-changed","Installation location changed; use the independent installer for recovery.");
    const fs::path home=receipt.at("home").get<std::string>();
    require(home.is_absolute() && home==fs::canonical(home) && receipt.value("home_identity","")==directory_identity(home,true) &&
        (requested.empty() || fs::absolute(requested).lexically_normal()==home),"installation-home-conflict","Use the installation's recorded data home.");
    return home;
}
fs::path saved_updater(const fs::path& home, const fs::path& prefix) {
    if(fs::exists(home/"updates")) {
        const auto state=StateStore(home/"updates",false).load();
        if(state && state->contains("updater")) {
            const auto& locator=state->at("updater"); const fs::path stage=locator.at("directory").get<std::string>();
            require(stage.parent_path()==fs::canonical("/tmp") && stage.filename().string().starts_with("shin-updater.") &&
                directory_identity(stage,true)==locator.at("identity").get<std::string>(),"update-bootstrap-changed","The retained updater changed; preserve it for review.");
            const auto bundle=stage/"payload"/updater_bundle_name;
            (void)verify_updater_bundle(bundle);
            require(digest(read_text(bundle/"shin-updater.json",64U*1024,false))==locator.at("manifest_sha256").get<std::string>(),
                "update-bootstrap-changed","The retained updater inventory changed.");
            return bundle/"bin/shin-updater";
        }
    }
    (void)verify_updater_bundle(prefix,true);
    return prefix/"bin/shin-updater";
}
void protocol(const fs::path& executable, const std::string& version="") {
    const auto reply=run(executable,{"--protocol"},10);
    require(reply.value("schema_version","")=="shin.updater-protocol.v1" && reply.value("protocol",0)==1 &&
        (version.empty() || reply.value("version","")==version),"update-bootstrap-protocol","The downloaded updater does not support the bootstrap protocol.");
}
}
Json local_updates(int argc,char** argv,const fs::path& user,const ReleaseUpdateProgressSink& observer) {
 @autoreleasepool {
    (void)user;
    const std::string command=argv[1];
    fs::path prefix=fs::canonical(current_executable_path()).parent_path().parent_path(),requested;
    std::string revision;bool force_progress=false;
    std::vector<std::string> forwarded;std::set<std::string> seen;
    for(int i=2;i<argc;++i) {
        const std::string key=argv[i];
        require(seen.insert(key).second,"invalid-arguments","Duplicate bootstrap option.");
        if(key=="--progress") {force_progress=true;forwarded.push_back(key);continue;}
        require((key=="--prefix" || key=="--home" || key=="--revision" || key=="--wait-pid" || key=="--ui-language" || key=="--ui-version") && i+1<argc,
            "invalid-arguments","Invalid bootstrap option.");
        const std::string value=argv[++i];
        if(key=="--prefix") prefix=fs::absolute(value).lexically_normal();
        else if(key=="--home") requested=value;
        else if(key=="--revision") revision=value;
        else {forwarded.push_back(key);forwarded.push_back(value);}
    }
    const auto home=bootstrap_home(prefix,requested);
    const bool read_result=command=="update-status" || command=="update-acknowledge";
    require(command=="update-check" || command=="update-install" || command=="update-download" || read_result,"invalid-command","Unknown update command.");
    TerminalUpdateProgress terminal(force_progress,false,!observer && !read_result && command!="update-check");
    fs::path executable,stage;
    struct Cleanup { fs::path path; ~Cleanup() { if(!path.empty()) {std::error_code error;fs::remove_all(path,error);} } } cleanup;
    std::vector<std::string> arguments{command,"--prefix",prefix.string(),"--home",home.string()};
    if(read_result) {
        executable=saved_updater(home,prefix);protocol(executable);
        if(!revision.empty()) {arguments.push_back("--revision");arguments.push_back(revision);}
    } else {
        auto selected=select_updater_release(parse_json(fetch("https://api.github.com/repos/toseek-samurai/shin-context-engine/releases?per_page=30",1024U*1024)),SHIN_OBSERVER_VERSION);
        const bool available=selected.value("available",false) && selected.value("version","")!=SHIN_OBSERVER_VERSION;
        if(command=="update-check") {
            if(!available)return {{"ok",true},{"available",false},{"current_version",SHIN_OBSERVER_VERSION},{"channel",selected.at("channel")},{"updater_protocol",1}};
            selected["updater_protocol"]=1;return selected;
        }
        require(available && !revision.empty() && selected.value("revision","")==revision,"update-offer-changed","The release offer changed. Check for updates again before installing.");
        char temporary[]="/tmp/shin-updater.XXXXXXXX";const auto created=::mkdtemp(temporary);
        require(created,"update-download-failed","Cannot create the private updater location.");stage=fs::canonical(created);cleanup.path=stage;
        const auto archive=stage/updater_archive_name;
        if(!observer) std::fprintf(stderr,"%s\n",shin::ui::tr("Downloading the current updater…"));
        const auto observe=[observer,&terminal](ReleaseUpdateProgress value) { value.step=ReleaseUpdateStep::preparing;report_update(observer,value);terminal.report(value); };
        fetch(selected.at("archive_url"),updater_archive_limit,archive,observe,selected.at("bytes").get<std::uint64_t>());
        const auto bytes=read_text(archive,updater_archive_limit,true),hash=digest(bytes);
        const auto checksum=fetch(selected.at("checksum_url"),512);
        const auto expected=hash+"  "+updater_archive_name;
        require(bytes.size()==selected.at("bytes") && hash==selected.at("sha256").get<std::string>() && (checksum==expected || checksum==expected+"\n"),"update-checksum-mismatch","Updater checksum mismatch; installation was not started.");
        verify_updater_archive(bytes);ensure_private_directory(stage/"payload",true);
        std::string extracted;run("/usr/bin/tar",{"-xf",archive.string(),"-C",(stage/"payload").string()},30,&extracted);
        const auto bundle=stage/"payload"/updater_bundle_name;
        const auto manifest=verify_updater_bundle(bundle);
        require(manifest.at("version")==selected.at("version"),"update-bootstrap-protocol","Updater version differs from its release.");
        executable=bundle/"bin/shin-updater";protocol(executable,selected.at("version"));
        const auto offer=run(executable,{"update-check","--prefix",prefix.string(),"--home",home.string()},30);
        require(offer.value("ok",false) && offer.value("available",false) && offer.value("version","")==selected.at("version").get<std::string>() && offer.contains("revision"),
            "update-offer-changed","The current updater could not confirm the reviewed release.");
        arguments.push_back("--revision");arguments.push_back(offer.at("revision"));
    }
    arguments.insert(arguments.end(),forwarded.begin(),forwarded.end());
    if(observer) {cleanup.path.clear();arguments.push_back("--progress-json");return run(executable,arguments,900,nullptr,observer);}
    // Keep PID/parent intact for Monitor's acknowledgement and handoff. The fresh
    // updater owns all UI, receipt interpretation and recovery after this point.
    std::vector<std::string> words{executable.string()};words.insert(words.end(),arguments.begin(),arguments.end());
    std::vector<char*> pointers; pointers.reserve(words.size()+1);
    for(auto& word:words)pointers.push_back(word.data());pointers.push_back(nullptr);
    cleanup.path.clear();
    ::execv(executable.c_str(),pointers.data());
    if(!stage.empty()) {std::error_code error;fs::remove_all(stage,error);}
    throw Error("update-process-failed","Could not launch the verified updater. The installed kit is unchanged.");
 }
}
} // namespace shin::observer
