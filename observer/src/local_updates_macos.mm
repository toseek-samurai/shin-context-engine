#import <Foundation/Foundation.h>
#include "shin/observer/local_updates.h"
#include "shin/observer/local_installation.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/observer.h"
#include "../ui/update_window_macos.h"
#include <chrono>
#include <thread>
#include <set>
#include <map>
#include <iostream>
#include <cstdio>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>

#include "local_update_transport_macos.inc"
#include "shin/observer/updater_bundle.h"
namespace shin::observer {
namespace {
fs::path own_prefix() {
    const auto exe=fs::canonical(current_executable_path());return exe.parent_path().parent_path();
}
Json installation(const fs::path& prefix,const fs::path& user) {
    std::vector<std::string> a{"shin-ce","installation","--prefix",prefix.string()};std::vector<char*> p;for(auto& s:a)p.push_back(s.data());
    auto result=local_installation((int)p.size(),p.data(),user);
    result["manifest_sha256"]=parse_json(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)).at("manifest_sha256");
    return result;
}
void describe_affected_projects(Json& state,const fs::path& home) noexcept {
    try {
        std::map<std::string,std::string> names;
        for(const auto& project:registered_local_projects(home)) {
            auto name=project.root.filename().string();
            if(name.size()>256)name="Project";
            names.emplace(digest(project.root.string()),std::move(name));
        }
        Json affected=Json::array();std::set<std::string> seen;
        const auto visit=[&](const auto& walk,const Json& node)->void {
            if(affected.size()>=16)return;
            if(node.is_object()) {
                const auto id=node.value("id",Json());
                if(id.is_string()) {
                    const auto found=names.find(id.get<std::string>());
                    if(found!=names.end() && seen.insert(found->first).second) {
                        std::string reason="project-not-ready";
                        const auto reasons=node.value("reasons",Json());
                        if(reasons.is_array() && !reasons.empty() && reasons[0].is_string())reason=reasons[0].get<std::string>();
                        if(reason.size()>128)reason="project-not-ready";
                        affected.push_back({{"name",found->second},{"reason",reason}});
                    }
                }
                for(const auto& item:node.items())walk(walk,item.value());
            } else if(node.is_array())for(const auto& item:node)walk(walk,item);
        };
        for(const auto* key:{"result","error","project_preparation"})if(state.contains(key))visit(visit,state.at(key));
        state["affected_projects"]=std::move(affected);
    } catch(...) { /* Local display labels are not installation authority. */ }
}
bool acknowledgeable_update(const Json& state) {
    const auto phase=state.value("phase","");
    return phase=="installed" || phase=="failed" || phase=="review-required" || phase=="downloaded" || phase=="retryable";
}
std::string update_review_revision(const Json& state,const Json& installed,const fs::path& prefix) {
    // A repaired/newer installation may supersede the saved attempt. Its exact
    // verified receipt is part of the explicit review; a changed kit cannot
    // inherit an acknowledgement made for a different installation.
    return digest(Json{{"state",state},{"installation",installed},
        {"receipt_sha256",digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true))}}.dump());
}
// Classify only after an independent, integrity-checked native readback. A
// known project refusal is not an interrupted installation transaction.
std::string update_disposition(const Json& state, const Json& installed, const std::string& receipt, bool updater_active=false) {
    if (!installed.value("integrity_verified",false)) return "review-required";
    if (installed.value("recovery_pending",true)) return "recovery-required";
    const auto phase=state.value("phase","");
    if(phase=="idle")return "idle";
    if(phase=="reviewed" && state.value("acknowledged_version",installed.value("version",""))==installed.value("version",""))return "reviewed";
    if(phase=="installed" && state.value("ok",false) && state.value("version","")==installed.value("version",""))return "installed";
    if (installed.value("version","")==state.value("version", "") && state.contains("target_manifest_sha256") &&
        installed.value("manifest_sha256","")==state.at("target_manifest_sha256").get<std::string>()) return "installed";
    // A successful terminal attempt cannot describe a later independently
    // verified kit. Never infer this from version alone or for an active writer.
    if (!updater_active && phase=="installed" && state.value("ok",false) &&
        !installed.value("manifest_sha256","").empty() && !state.value("target_manifest_sha256","").empty() &&
        installed.value("manifest_sha256","")!=state.value("target_manifest_sha256","") &&
        release_is_newer(installed.value("version",""),state.value("version","")) &&
        release_is_newer(installed.value("version",""),state.value("from_version",""))) return "superseded";
    if (installed.value("version","")!=state.value("from_version", "")) return "review-required";
    const auto result=state.value("result",Json::object());
    const auto error=result.value("error",state.value("error",Json::object()));
    const auto code=error.is_object() ? error.value("code","") : std::string{};
    const std::set<std::string> transfer_failures{"update-network-unavailable","update-download-failed","update-process-failed","update-monitor-open"};
    if(phase=="failed" && transfer_failures.contains(code) && !receipt.empty() &&
        state.value("before_receipt_sha256","")==receipt)return "retryable";
    const std::set<std::string> refused{"installation-project-not-ready","installation-update-rolled-back",
        "project-busy","binding-changed","service-busy","writer-busy","update-monitor-open"};
    if (refused.contains(code)) return "retryable";
    // With no valid child receipt, only the exact original installation receipt
    // and an idle native journal establish that replacing it is safe to retry.
    if ((code.empty() || code=="update-outcome-unknown") &&
        state.value("before_receipt_sha256","")==receipt && !receipt.empty()) return "retryable";
    return "review-required";
}
fs::path retained_stage(const Json& state) {
    if(!state.contains("staging"))return {};
    const fs::path stage=state.at("staging").get<std::string>();
    require(stage.parent_path()==fs::canonical("/tmp") && stage.filename().string().starts_with("shin-release."),
        "update-staging-changed","The saved candidate location is invalid.");
    if(!fs::exists(fs::symlink_status(stage)))return {};
    require(!fs::is_symlink(fs::symlink_status(stage)) && directory_identity(stage,true)==state.value("stage_identity",""),
        "update-staging-changed","The saved candidate location changed; preserve it for review.");
    return stage;
}
fs::path verified_retained_kit(const Json& state,const Json& selected) {
    const auto stage=retained_stage(state);
    if(stage.empty() || state.value("version","")!=selected.value("version", ""))return {};
    // A process may die midway through a transfer. Such bytes were never
    // admitted as a candidate and must not be mistaken for verified-kit tampering.
    if(!state.contains("candidate") && !state.contains("target_manifest_sha256"))return {};
    const auto archive=stage/selected.at("archive_name").get<std::string>();
    if(!fs::exists(archive))return {};
    require(!fs::is_symlink(fs::symlink_status(archive)) && fs::file_size(archive)==selected.at("bytes").get<std::uint64_t>() &&
        digest(read_text(archive,128U*1024*1024,true))==selected.at("sha256").get<std::string>(),
        "update-checksum-mismatch","The saved archive changed; installation was not started.");
    const auto name="shin-local-"+selected.at("version").get<std::string>()+"-macos-arm64";
    const auto kit=stage/"unpacked"/name;
    if(!fs::exists(kit))return {};
    require(!fs::is_symlink(fs::symlink_status(kit)) && fs::canonical(kit)==kit,
        "update-staging-changed","The saved kit location changed.");
    std::string original;
    run("/usr/bin/tar",{"-xOf",archive.string(),name+"/shin-kit.json"},15,&original);
    require(digest(original)==digest(read_text(kit/"shin-kit.json",kMaximumStateBytes,false)),
        "update-staging-changed","The saved kit inventory differs from its verified archive.");
    const auto checked=verify_local_kit(kit);
    require(checked.at("version")==selected.at("version"),"update-release-invalid","Saved kit version changed.");
    return kit;
}
bool cleanup_candidate(Json& state) {
    try {
        const auto stage=retained_stage(state);
        if(!stage.empty()) {
            std::error_code error;fs::remove_all(stage,error);
            if(error) {state["cleanup_warning"]="update-cache-cleanup-pending";return false;}
        }
        state.erase("candidate");state.erase("staging");state.erase("stage_identity");state.erase("cleanup_warning");
        return true;
    } catch(const Error& error) {state["cleanup_warning"]=error.code;return false;}
      catch(const std::exception&) {state["cleanup_warning"]="update-cache-cleanup-pending";return false;}
}
Json saved_offer(const Json& state) {
    const auto& saved=state.at("release");
    const auto version=state.at("version").get<std::string>();
    const auto name=saved.at("archive_name").get<std::string>();
    const Json release{{"draft",false},{"prerelease",version.find('-')!=std::string::npos},
        {"tag_name","v"+version},{"html_url",saved.at("release_url")},
        {"assets",Json::array({
            {{"name",name},{"state","uploaded"},{"digest","sha256:"+saved.at("sha256").get<std::string>()},
             {"size",saved.at("bytes")},{"browser_download_url",saved.at("archive_url")}},
            {{"name",name+".sha256"},{"state","uploaded"},{"size",128},{"browser_download_url",saved.at("checksum_url")}}
        })}};
    auto checked=select_local_release(Json::array({release}),state.at("from_version"));
    require(checked.value("available",false) && checked.value("version","")==version,
        "update-release-invalid","The saved release descriptor is invalid.");
    return checked;
}
Json exact_offer(const Json& state) {
    const auto version=state.at("version").get<std::string>();
    require(!version.empty() && version.size()<=64 && version.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-")==std::string::npos,
        "update-release-invalid","Invalid saved release version.");
    if(state.contains("release")) {
        auto selected=saved_offer(state);
        if(!verified_retained_kit(state,selected).empty())return selected;
    }
    const auto release=parse_release_feed("["+fetch("https://api.github.com/repos/toseek-samurai/shin-context-engine/releases/tags/v"+version,kMaximumReleaseFeedBytes)+"]");
    auto selected=select_local_release(release,state.at("from_version"));
    require(selected.value("available",false) && selected.value("version","")==version,
        "update-release-invalid","The exact saved release is no longer available.");
    if(state.contains("release"))require(selected.at("sha256")==state.at("release").at("sha256") &&
        selected.at("bytes")==state.at("release").at("bytes"),"update-release-invalid","The saved release artifact changed.");
    return selected;
}
Json offer(const std::string& current) {
    return select_local_release(parse_release_feed(fetch("https://api.github.com/repos/toseek-samurai/shin-context-engine/releases?per_page=30",kMaximumReleaseFeedBytes)),current);
}
fs::path download_release_kit(const Json& selected,const fs::path& stage,const ReleaseUpdateProgressSink& sink) {
    const auto archive=stage/selected.at("archive_name").get<std::string>();
    report_update(sink,{ReleaseUpdateStep::downloading,0,selected.at("bytes").get<std::uint64_t>()});
    fetch(selected.at("archive_url"),128U*1024*1024,archive,sink,selected.at("bytes").get<std::uint64_t>());
    report_update(sink,{ReleaseUpdateStep::verifying_archive});
    const auto hash=digest(read_text(archive,128U*1024*1024,true));
    const auto checksum=fetch(selected.at("checksum_url"),512);
    const auto expected=hash+"  "+selected.at("archive_name").get<std::string>();
    require(fs::file_size(archive)==selected.at("bytes").get<std::uint64_t>() && hash==selected.at("sha256").get<std::string>() && (checksum==expected || checksum==expected+"\n"),"update-checksum-mismatch","The downloaded archive did not match the offered SHA-256. Installation was not started.");
    std::string listing;run("/usr/bin/tar",{"-tf",archive.string()},15,&listing);verify_release_listing(listing,selected.at("version"));
    report_update(sink,{ReleaseUpdateStep::extracting});
    ensure_private_directory(stage/"unpacked",true);
    std::string extracted;run("/usr/bin/tar",{"-xf",archive.string(),"-C",(stage/"unpacked").string()},30,&extracted);
    const auto kit=stage/"unpacked"/("shin-local-"+selected.at("version").get<std::string>()+"-macos-arm64");
    report_update(sink,{ReleaseUpdateStep::verifying_kit});
    const auto checked=verify_local_kit(kit);
    require(checked.at("version")==selected.at("version"),"update-release-invalid","The kit version differs from the offered release.");
    return kit;
}
class UpdateProgressViewer {
    struct Pipe {
        std::mutex mutex;
        NSFileHandle* output=nil;
        int descriptor=-1;
        ReleaseUpdateStep last=ReleaseUpdateStep::preparing;
        std::chrono::steady_clock::time_point written{};
    };
    std::shared_ptr<Pipe> pipe_;
    NSTask* task_=nil;
public:
    UpdateProgressViewer(const fs::path& prefix,const std::string& version,const std::string& language,bool enabled) {
        (void)prefix;
        if(!enabled)return;
        @try {
            NSPipe* channel=[NSPipe pipe];
            const int descriptor=channel.fileHandleForWriting.fileDescriptor;
            if(::fcntl(descriptor,F_SETFL,O_NONBLOCK)!=0 || ::fcntl(descriptor,F_SETNOSIGPIPE,1)!=0 ||
                ::fcntl(descriptor,F_SETFD,FD_CLOEXEC)!=0)return;
            task_=[NSTask new];task_.executableURL=[NSURL fileURLWithPath:ns(current_executable_path().string())];
            task_.arguments=@[@"--update-progress",ns(version),ns(language.empty() ? "en" : language)];
            task_.standardInput=channel;task_.standardOutput=[NSFileHandle fileHandleWithNullDevice];task_.standardError=[NSFileHandle fileHandleWithNullDevice];
            if(![task_ launchAndReturnError:nullptr])return;
            [channel.fileHandleForReading closeFile];
            pipe_=std::make_shared<Pipe>();pipe_->output=channel.fileHandleForWriting;pipe_->descriptor=descriptor;
            sink()({ReleaseUpdateStep::preparing});
        } @catch(NSException*) { /* Progress is optional; native acknowledgement and outcome remain authoritative. */ }
    }
    ReleaseUpdateProgressSink sink() const {
        if(!pipe_)return {};
        return [pipe=pipe_](ReleaseUpdateProgress value) {
            try {
                std::lock_guard lock(pipe->mutex);
                if(pipe->descriptor<0)return;
                const auto now=std::chrono::steady_clock::now();
                if(value.step==ReleaseUpdateStep::downloading && pipe->last==value.step && value.received<value.total &&
                    now-pipe->written<std::chrono::milliseconds(100))return;
                const auto line=update_progress_frame(value).dump()+"\n";
                // A slow/closed window must never block or abort the updater.
                (void)::write(pipe->descriptor,line.data(),line.size());
                pipe->last=value.step;pipe->written=now;
            } catch(...) { /* Observation cannot alter update behavior. */ }
        };
    }
    ~UpdateProgressViewer() {
        @try {
            if(pipe_) {std::lock_guard lock(pipe_->mutex);pipe_->descriptor=-1;[pipe_->output closeFile];}
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            while(task_.running && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if(task_.running) {
                [task_ terminate]; // Only this helper's own read-only viewer.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if(task_.running)(void)::kill(task_.processIdentifier,SIGKILL);
            }
            if(task_)[task_ waitUntilExit];
        } @catch(NSException*) { /* Native outcome is already saved. */ }
    }
};
struct RelaunchMonitor {
    fs::path prefix; bool active=false;
    ~RelaunchMonitor() {
        if(!active)return;
        @try {
            NSTask* task=[NSTask new];task.executableURL=[NSURL fileURLWithPath:@"/usr/bin/open"];
            task.arguments=@[ns((prefix/"Shin Monitor.app").string())];
            task.standardOutput=[NSFileHandle fileHandleWithNullDevice];task.standardError=[NSFileHandle fileHandleWithNullDevice];
            [task launchAndReturnError:nullptr];
        } @catch(NSException*) { /* The durable outcome remains available to the next explicit launch. */ }
    }
};

}
Json run_updater_request(int argc,char** argv,const fs::path& user,const ReleaseUpdateProgressSink& observer) {
 @autoreleasepool {
    const std::string command=argv[1];fs::path prefix=user/".local/opt/shin-local",home;
    std::string revision,language,display_version;int parent=0;bool force_progress=false,json_progress=false;std::set<std::string> seen;
    for(int i=2;i<argc;++i) {
        const std::string key=argv[i];
        if((key=="--progress" || key=="--progress-json") && seen.insert(key).second) {require(!force_progress && !json_progress,"invalid-arguments","Choose one progress format.");force_progress=key=="--progress";json_progress=!force_progress;continue;}
        require((key=="--prefix" || key=="--home" || key=="--revision" || key=="--wait-pid" || key=="--ui-language" || key=="--ui-version") && seen.insert(key).second && i+1<argc,"invalid-arguments","Invalid update option.");
        const std::string value=argv[++i];
        if(key=="--prefix")prefix=fs::absolute(value).lexically_normal();else if(key=="--home")home=fs::absolute(value).lexically_normal();else if(key=="--revision")revision=value;
        else if(key=="--ui-version") {
            require(!value.empty() && value.size()<=64 && value.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-")==std::string::npos,"invalid-arguments","Invalid display version.");display_version=value;
        }
        else if(key=="--ui-language") {require(value=="en" || value=="ru","invalid-arguments","Choose en or ru for the update window.");language=value;}
        else {try{std::size_t used=0;parent=std::stoi(value,&used);require(used==value.size() && parent>1 && parent==getppid(),"invalid-arguments","Only the launching Monitor may be awaited.");}catch(const Error&){throw;}catch(...){throw Error("invalid-arguments","Invalid launching Monitor PID.");}}
    }
    require(!parent || command=="update-install" || command=="update-resume","invalid-arguments","Only an explicit Monitor installation may await its parent.");
    require((language.empty() && display_version.empty()) || parent,"invalid-arguments","Update window language requires the launching Monitor.");
    require(!(force_progress || json_progress) || (!parent && (command=="update-install" || command=="update-resume" || command=="update-download" || command=="update-acknowledge")),"invalid-arguments","--progress is available for terminal downloads and updates.");
    TerminalUpdateProgress terminal(force_progress,json_progress,!observer && !parent && (command=="update-install" || command=="update-resume" || command=="update-download"));
    terminal.report({ReleaseUpdateStep::preparing});
    const auto installed=installation(prefix,user);require(installed.value("integrity_verified",false),"installation-integrity-failed","Verify the owned installation before checking updates.");
    const auto current=installed.at("version").get<std::string>();
    if(home.empty())home=installed.at("home").get<std::string>();
    require(home==fs::path(installed.at("home").get<std::string>()),"installation-home-conflict","Use the installed kit's recorded data home.");
    if(command=="update-status" || command=="update-acknowledge") {
        const auto directory=home/"updates";
        if(!fs::exists(directory))return {{"ok",true},{"phase","idle"}};
        if(command=="update-status") {
            StateStore saved(directory,false);
            auto state=saved.load().value_or(Json{{"ok",true},{"phase","idle"}});
            state["review_revision"]=update_review_revision(state,installed,prefix);
            describe_affected_projects(state,home);
            state["current_version"]=current;state["installation_verified"]=true;
            state["native_recovery_pending"]=installed.value("recovery_pending",true);
            const bool terminal=!saved.writer_present();
            state["updater_active"]=!terminal;
            const auto disposition=update_disposition(state,installed,digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)),!terminal);
            state["can_retry"]=terminal && disposition=="retryable";
            state["can_resume"]=terminal && disposition=="recovery-required" && state.contains("version");
            state["outcome"]=disposition;
            if(disposition=="installed" || disposition=="superseded")state["ok"]=true;
            else if(disposition=="recovery-required" || disposition=="retryable")state["ok"]=false;
            state["can_acknowledge"]=!installed.value("recovery_pending",true) && !saved.writer_present() &&
                (acknowledgeable_update(state) || disposition=="installed" || disposition=="retryable");
            return state;
        }
        StateStore review(directory,true);review.recover_pending();auto previous=review.load();
        require(!installed.value("recovery_pending",true),"update-outcome-unknown","Finish the native installation recovery journal before acknowledging the update result.");
        auto installation_guard=lock_local_installation(prefix/"bin/shin-ce");
        const auto confirmed=installation(prefix,user);
        require(!confirmed.value("recovery_pending",true),"update-outcome-unknown","Finish the native installation recovery journal before acknowledging the update result.");
        require(previous && !revision.empty() && update_review_revision(*previous,confirmed,prefix)==revision,"update-offer-changed","The saved result or current installation changed. Review it again.");
        auto state=*previous;
        require(acknowledgeable_update(state) || update_disposition(state,confirmed,digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)))=="installed" ||
            update_disposition(state,confirmed,digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)))=="retryable","update-outcome-unknown","The previous updater has no terminal result. Review its installation recovery before acknowledging it.");
        if(state.contains("staging")) {
            const fs::path stage=state.at("staging").get<std::string>();
            require(stage.parent_path()==fs::canonical("/tmp") && stage.filename().string().starts_with("shin-release."),"update-staging-changed","The saved candidate location is invalid; preserve it for review.");
            if(fs::exists(fs::symlink_status(stage))) {
                require(!fs::is_symlink(fs::symlink_status(stage)) && directory_identity(stage,true)==state.value("stage_identity",""),"update-staging-changed","The saved candidate location changed; preserve it for review.");
                fs::remove_all(stage);
            }
            state.erase("staging");state.erase("stage_identity");state.erase("candidate");
        }
        state.erase("updater");state["phase"]="reviewed";state["ok"]=true;
        state["acknowledged_version"]=confirmed.at("version");review.save(state);return state;
    }
    // The viewer must exit before LaunchServices reopens this bundle.
    RelaunchMonitor relaunch{prefix};
    // Display-only hint from the review. The release revision is still rechecked below.
    UpdateProgressViewer viewer(prefix,display_version,language,parent!=0);
    const auto progress=[terminal_sink=terminal.sink(),viewer_sink=viewer.sink(),observer](ReleaseUpdateProgress value) {
        report_update(terminal_sink,value);report_update(viewer_sink,value);report_update(observer,value);
    };
    report_update(progress,{ReleaseUpdateStep::checking_release});
    Json resume_state;
    if(command=="update-resume") {
        StateStore saved(home/"updates",false);resume_state=saved.load().value_or(Json::object());
        require(!saved.writer_present() && !revision.empty() && update_review_revision(resume_state,installed,prefix)==revision,
            "update-offer-changed","The saved operation changed; review it again.");
    }
    auto selected=command=="update-resume" ? exact_offer(resume_state) : offer(current);selected["prefix"]=prefix.string();
    if(command=="update-check")return selected;
    require(command=="update-download" || command=="update-install" || command=="update-resume","invalid-command","Unknown update command.");
    require(selected.at("available")==true && !revision.empty() && (command=="update-resume" || selected.at("revision")==revision),"update-offer-changed","The release offer changed. Check for updates again before installing.");
    const auto operation=[&](const ReleaseUpdateProgressSink& sink) -> Json {
    const auto directory=home/"updates";ensure_private_directory(directory);
    StateStore owner(directory,true);owner.recover_pending();
    const auto prior=owner.load();
    Json confirmed;
    {
        // Stable installations can be fenced through their verified payload.
        // Recovery inspection itself refuses any live native operation; the
        // exact candidate rechecks its journal under its exclusive operation lock.
        auto fence=installed.value("recovery_pending",true) ? std::unique_ptr<LocalInstallationGuard>{} :
            lock_local_installation(prefix/"bin/shin-ce");
        confirmed=installation(prefix,user);
        require(confirmed.value("version","")==installed.value("version","") &&
            confirmed.value("manifest_sha256","")==installed.value("manifest_sha256",""),
            "update-installation-changed","The installed kit changed while the release was reviewed.");
        if(confirmed.value("recovery_pending",false))require(command=="update-resume" && prior &&
            confirmed.value("target_version",prior->value("version", ""))==prior->value("version", ""),
            "update-outcome-unknown","Resume the exact pending installation before starting another update.");
        if(command=="update-resume")require(prior && *prior==resume_state &&
            update_review_revision(*prior,confirmed,prefix)==revision,"update-offer-changed","The saved operation changed.");
        if(prior && prior->value("phase","")!="installed" && prior->value("phase","")!="reviewed" && prior->value("phase","")!="failed") {
            const auto disposition=update_disposition(*prior,confirmed,digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)));
            require(disposition=="installed" || disposition=="retryable" || (command=="update-resume" && disposition=="recovery-required"),
                "update-outcome-unknown","The previous operation needs recovery. Review its saved result before continuing.");
        }
    }
    fs::path reusable=prior ? verified_retained_kit(*prior,selected) : fs::path{};
    Json state{{"ok",true},{"phase","downloading"},{"from_version",prior && command=="update-resume" ? prior->at("from_version") : Json(current)},
        {"version",selected.at("version")},{"release",selected},{"checked_at_ms",now_ms()},
        {"before_receipt_sha256",digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true))},
        {"timeline",Json::array()}};
    if(prior && prior->contains("result"))state["previous_result"]=prior->at("result");
    const auto record=[&](ReleaseUpdateProgress value) {
        auto& timeline=state["timeline"];
        const auto step=update_progress_frame(value).at("step");
        if(timeline.empty() || timeline.back().at("step")!=step) {
            if(timeline.size()==64)timeline.erase(timeline.begin());
            timeline.push_back({{"step",step},{"at_ms",now_ms()}});owner.save(state);
        }
        report_update(sink,value);
    };
    const auto report=[&](ReleaseUpdateStep step) {record({step});};
    const auto self=own_prefix();
    if(self.filename()==updater_bundle_name && self.parent_path().filename()=="payload" && self.parent_path().parent_path().filename().string().starts_with("shin-updater.")) {
        const auto stage=self.parent_path().parent_path();
        state["updater"]={{"directory",stage.string()},{"identity",directory_identity(stage,true)},{"manifest_sha256",digest(read_text(self/"shin-updater.json",64U*1024,false))}};
    }
    // A newer explicit offer supersedes a completed, non-mutating refusal. Its
    // prior diagnostic is retained, while its owned obsolete archive is removed.
    if(prior && !confirmed.value("recovery_pending",true) && (prior->value("version","")!=selected.value("version","") ||
         (reusable.empty() && !prior->contains("candidate") && !prior->contains("target_manifest_sha256"))) &&
        (prior->value("phase","")=="installed" || prior->value("phase","")=="reviewed" || prior->value("phase","")=="failed" ||
         update_disposition(*prior,confirmed,digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)))=="retryable")) {
        const auto obsolete=retained_stage(*prior);
        if(!obsolete.empty())fs::remove_all(obsolete);
    }
    owner.save(state);
    // The old independent engine is no longer recovery authority once this
    // verified helper owns the new outcome. Never remove our own running bundle.
    if(prior && prior->contains("updater") && !confirmed.value("recovery_pending",true)) {
        try {
            const auto& locator=prior->at("updater");
            const fs::path old=locator.at("directory").get<std::string>();
            const auto bundle=old/"payload"/updater_bundle_name;
            if(old.parent_path()==fs::canonical("/tmp") && old.filename().string().starts_with("shin-updater.") &&
                bundle!=self && fs::exists(fs::symlink_status(old)) && !fs::is_symlink(fs::symlink_status(old)) &&
                directory_identity(old,true)==locator.at("identity").get<std::string>()) {
                (void)verify_updater_bundle(bundle);
                if(digest(read_text(bundle/"shin-updater.json",64U*1024,false))==locator.at("manifest_sha256").get<std::string>())
                    fs::remove_all(old);
            }
        } catch(...) { /* Optional obsolete-cache cleanup never changes installation authority. */ }
    }
    fs::path stage;
    if(!reusable.empty())stage=retained_stage(*prior);
    else {
        char temporary[]="/tmp/shin-release.XXXXXXXX";char* folder=mkdtemp(temporary);
        require(folder,"update-download-failed","Cannot create a private staging folder.");stage=fs::canonical(folder);
    }
    state["staging"]=stage.string();state["stage_identity"]=directory_identity(stage,true);bool installing=false;owner.save(state);
    try {
        if(parent) {
            relaunch.active=true;state["phase"]="waiting-for-monitor";state["helper_pid"]=getpid();owner.save(state);
            std::cout << Json{{"accepted",true},{"pid",getpid()}}.dump() << std::endl;
            report(ReleaseUpdateStep::waiting_for_monitor);
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(::kill(parent,0)==0 && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(::kill(parent,0)!=0,"update-monitor-open","Close Monitor before applying the update.");
            (void)freopen("/dev/null","w",stdout);
            state["phase"]="downloading";owner.save(state);
        }
        const auto kit=reusable.empty() ? download_release_kit(selected,stage,record) : reusable;
        if(confirmed.contains("target_manifest_sha256"))require(digest(read_text(kit/"shin-kit.json",kMaximumStateBytes,false))==confirmed.at("target_manifest_sha256").get<std::string>(),
            "update-release-invalid","The recovered archive differs from the journaled target.");
        state["candidate"]=kit.string();state["target_manifest_sha256"]=digest(read_text(kit/"shin-kit.json",kMaximumStateBytes,false));state["phase"]="downloaded";owner.save(state);
        if(command=="update-download")return state;
        installing=true;state["phase"]="installing";owner.save(state);
        report(ReleaseUpdateStep::installing);
        const auto result=run(kit/"bin/shin-ce",{"update","--prefix",prefix.string(),"--progress-json"},0,nullptr,record);
        state["result"]=result;
        const auto after=installation(prefix,user);
        const auto disposition=update_disposition(state,after,digest(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)));
        state["ok"]=disposition=="installed";state["phase"]=disposition;
        for(const auto* field:{"projects_pending","project_preparation","project_diagnostic","all_projects_ready"})if(result.contains(field))state[field]=result.at(field);
        owner.save(state);
        if(state.at("ok")==true) {
            report(ReleaseUpdateStep::confirming);const auto confirmed=installation(prefix,user);
            require(confirmed.at("version")==selected.at("version") && confirmed.value("integrity_verified",false),
                "update-outcome-unknown","The installed version did not confirm the update receipt.");
            // Cache removal is optional after independent installation proof.
            // Retain the complete locator on failure so later cleanup stays safe.
            (void)cleanup_candidate(state);state.erase("updater");owner.save(state);
        }
    } catch(const std::exception& e) {
        const auto* typed=dynamic_cast<const Error*>(&e);
        state["ok"]=false;state["phase"]=installing ? "review-required" : "failed";state["error"]=typed ? error_json(*typed) : error_json(Error(installing ? "update-outcome-unknown" : "update-preparation-failed","The operation did not complete. Review the saved result before retrying."));state["staging"]=stage.string();owner.save(state);
        if(!installing) {require(directory_identity(stage,true)==state.at("stage_identity").get<std::string>(),"update-staging-changed","Private staging changed; preserve it for review.");fs::remove_all(stage);state.erase("candidate");state.erase("staging");state.erase("stage_identity");if(!confirmed.value("recovery_pending",false))state.erase("updater");owner.save(state);}
    }
    describe_affected_projects(state,home);owner.save(state);
    report(state.value("ok",false) && state.value("phase","")=="installed" ? ReleaseUpdateStep::completed : ReleaseUpdateStep::failed);
    return state;
    };
    return operation(progress);
 }
}
Json install_local_release(int argc,char** argv,const fs::path& user) {
 @autoreleasepool {
    require(argc>=4 && std::string_view(argv[2])=="--kit","invalid-arguments","Provide the complete extracted kit.");
    const fs::path kit=fs::canonical(argv[3]);
    const auto checked=verify_local_kit(kit);
    require(checked.at("version")==SHIN_OBSERVER_VERSION,"update-release-invalid","Updater and local kit versions differ.");
    fs::path prefix=user/".local/opt/shin-local";
    std::vector<std::string> forwarded;
    for(int i=4;i<argc;++i) {
        const std::string key=argv[i];
        if(key=="--adopt-existing") {forwarded.push_back(key);continue;}
        require((key=="--prefix" || key=="--home" || key=="--bin-dir") && i+1<argc,"invalid-arguments","Invalid independent installer option.");
        const std::string value=argv[++i];forwarded.push_back(key);forwarded.push_back(value);
        if(key=="--prefix")prefix=fs::absolute(value).lexically_normal();
    }
    const bool updating=fs::exists(prefix) || fs::is_symlink(prefix);
    // The native updater resolves an existing installation's recorded paths.
    // Only a first installation receives the bootstrap's selected defaults.
    for(const auto& [key,path]:std::array<std::pair<std::string,fs::path>,3>{{{"--prefix",prefix},{"--home",user/".shin-ce"},{"--bin-dir",user/".local/bin"}}})
        if((!updating || key=="--prefix") && std::find(forwarded.begin(),forwarded.end(),key)==forwarded.end()) {forwarded.push_back(key);forwarded.push_back(path.string());}
    std::vector<std::string> options{updating ? "update" : "install"};
    options.insert(options.end(),forwarded.begin(),forwarded.end());
    TerminalUpdateProgress terminal(true);const auto sink=terminal.sink();
    if(options.front()=="update")options.push_back("--progress-json");
    return run(kit/"bin/shin-ce",options,0,nullptr,sink);
 }
}

} // namespace shin::observer
