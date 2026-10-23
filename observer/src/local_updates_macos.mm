#import <Foundation/Foundation.h>
#include "shin/observer/local_updates.h"
#include "shin/observer/local_installation.h"
#include "shin/observer/observer.h"
#include "../ui/update_window_macos.h"
#include <chrono>
#include <thread>
#include <set>
#include <iostream>
#include <cstdio>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>

// Separate from service/refresh: no GitHub access occurs in retrieval or indexing.
@interface ShinReleaseHTTP : NSObject<NSURLSessionDataDelegate>
@property(nonatomic) NSUInteger limit;
@property(nonatomic) NSUInteger received;
@property(nonatomic) NSInteger status;
@property(nonatomic) NSUInteger redirects;
@property(nonatomic) BOOL refused;
@property(nonatomic, strong) NSMutableData* bytes;
@property(nonatomic, strong) NSFileHandle* file;
@property(nonatomic, strong) dispatch_semaphore_t done;
@property(nonatomic, copy) void (^progress)(NSUInteger);
@end
@implementation ShinReleaseHTTP
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response
    newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completion {
    (void)session;(void)task;(void)response;
    const BOOL allowed=[request.URL.scheme isEqualToString:@"https"] && !request.URL.user && !request.URL.password &&
        (!request.URL.port || request.URL.port.integerValue==443) &&
        [@[@"api.github.com",@"github.com",@"release-assets.githubusercontent.com",@"objects.githubusercontent.com",@"github-releases.githubusercontent.com"] containsObject:request.URL.host] && ++self.redirects<=3;
    if (!allowed) self.refused=YES;
    completion(allowed ? request : nil);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)session;(void)task;
    self.status=[(NSHTTPURLResponse*)response statusCode];
    if (self.status!=200 || response.expectedContentLength>(int64_t)self.limit) {self.refused=YES;completion(NSURLSessionResponseCancel);}
    else completion(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    (void)session;
    if (data.length>self.limit-self.received) {self.refused=YES;[task cancel];return;}
    self.received+=data.length;
    @try {if (self.file) [self.file writeData:data]; else [self.bytes appendData:data];}
    @catch (NSException*) {self.refused=YES;[task cancel];}
    if (!self.refused && self.progress) self.progress(self.received);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;(void)task;if(error)self.refused=YES;dispatch_semaphore_signal(self.done);
}
@end
namespace shin::observer {
namespace {
NSString* ns(const std::string& s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
void require(bool ok,const char* code,const char* message) {if(!ok)throw Error(code,message);}
std::string fetch(const std::string& url,std::size_t limit,const fs::path& output={},
                  const ReleaseUpdateProgressSink& progress={},std::uint64_t expected=0) {
    ShinReleaseHTTP* receiver=[ShinReleaseHTTP new];receiver.limit=limit;receiver.done=dispatch_semaphore_create(0);receiver.bytes=[NSMutableData data];
    if (progress) { const auto sink=progress;receiver.progress=^(NSUInteger received) { sink({ReleaseUpdateStep::downloading,received,expected}); }; }
    if (!output.empty()) {require(write_private_archive(output,""),"update-download-failed","Cannot create the private download.");receiver.file=[NSFileHandle fileHandleForWritingAtPath:ns(output.string())];require(receiver.file!=nil,"update-download-failed","Cannot open the private download.");}
    auto* config=[NSURLSessionConfiguration ephemeralSessionConfiguration];
    const double seconds=output.empty() ? 12 : 180;
    config.timeoutIntervalForRequest=seconds;config.timeoutIntervalForResource=seconds;
    config.HTTPCookieStorage=nil;config.URLCredentialStorage=nil;config.URLCache=nil;config.requestCachePolicy=NSURLRequestReloadIgnoringLocalCacheData;
    NSURLSession* session=[NSURLSession sessionWithConfiguration:config delegate:receiver delegateQueue:nil];
    NSMutableURLRequest* request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:ns(url)]];
    [request setValue:@"Shin-Context-Engine" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [request setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];
    NSURLSessionDataTask* task=[session dataTaskWithRequest:request];[task resume];
    const auto timedOut=dispatch_semaphore_wait(receiver.done,dispatch_time(DISPATCH_TIME_NOW,(int64_t)((seconds+2)*NSEC_PER_SEC)))!=0;
    if(timedOut) { [task cancel];dispatch_semaphore_wait(receiver.done,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC)); }
    [session invalidateAndCancel];[receiver.file closeFile];
    require(!timedOut && !receiver.refused && receiver.status==200,"update-network-unavailable","GitHub could not be checked or the download failed. The current installation is unchanged.");
    return receiver.bytes.length ? std::string((const char*)receiver.bytes.bytes,receiver.bytes.length) : "";
}
Json run(const fs::path& exe,const std::vector<std::string>& args,int timeout,std::string* text=nullptr,
         const ReleaseUpdateProgressSink& progress={}) {
    NSTask* task=[NSTask new];task.executableURL=[NSURL fileURLWithPath:ns(exe.string())];
    NSMutableArray* argv=[NSMutableArray array];for(const auto& a:args)[argv addObject:ns(a)];task.arguments=argv;
    task.environment=@{@"PATH":@"/usr/bin:/bin",@"LC_ALL":@"C",@"HOME":NSHomeDirectory()};task.standardInput=[NSFileHandle fileHandleWithNullDevice];
    // A file avoids pipe deadlock and inherited-pipe acknowledgement ambiguity.
    char pattern[]="/tmp/shin-update-output.XXXXXXXX";const int fd=mkstemp(pattern);
    require(fd>=0,"update-process-failed","Cannot prepare the updater output.");
    const auto transcript=fs::canonical(pattern);
    NSFileHandle* output=[[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];task.standardOutput=output;task.standardError=output;
    // The child's stdout remains an independent native receipt. Read-only
    // stderr frames are drained while it works, never interpreted as success.
    NSPipe* channel=progress ? [NSPipe pipe] : nil;
    const int progress_fd=channel ? channel.fileHandleForReading.fileDescriptor : -1;
    if(channel) {
        require(::fcntl(progress_fd,F_SETFL,O_NONBLOCK)==0,"update-process-failed","Cannot prepare update observation.");
        task.standardError=channel;
    }
    std::string pending,errors;
    const auto drain=[&] {
        if(progress_fd<0)return;
        char chunk[4096];ssize_t count;std::size_t read=0;
        while(read<64*1024 && (count=::read(progress_fd,chunk,sizeof(chunk)))>0) {
            read+=static_cast<std::size_t>(count);
            pending.append(chunk,static_cast<std::size_t>(count));
            for(auto end=pending.find('\n');end!=std::string::npos;end=pending.find('\n')) {
                const auto line=pending.substr(0,end);pending.erase(0,end+1);
                if(const auto value=parse_update_progress(Json::parse(line,nullptr,false))) {
                    // Only the outer independent readback can finish the update.
                    if(value->step!=ReleaseUpdateStep::completed && value->step!=ReleaseUpdateStep::failed)
                        report_update(progress,*value);
                } else if(errors.size()+line.size()<256*1024) errors+=line+"\n";
            }
            if(pending.size()>8192)pending.clear(); // Malformed observation is bounded, never a cancellation.
        }
    };
    NSError* error=nil;
    if(![task launchAndReturnError:&error]) {::unlink(pattern);throw Error("update-process-failed","Cannot start the native update tool.");}
    if(channel)[channel.fileHandleForWriting closeFile];
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(timeout);
    bool exceeded=false;
    while(task.running) {
        drain();
        if(std::chrono::steady_clock::now()>=until || fs::file_size(transcript)>256U*1024) {exceeded=true;[task terminate];break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if(exceeded) {std::this_thread::sleep_for(std::chrono::milliseconds(200));if(task.running)::kill(task.processIdentifier,SIGKILL);}
    [task waitUntilExit];[output synchronizeFile];drain();errors+=pending;
    std::string bytes;
    try {bytes=read_text(transcript,256U*1024,true);} catch(...) {::unlink(pattern);throw;}
    ::unlink(pattern);
    require(!exceeded,"update-outcome-unknown","The updater did not acknowledge completion. Review installation status before retrying.");
    if(text) {*text=bytes;require(task.terminationStatus==0,"update-archive-invalid","Cannot inspect or extract the release archive.");return Json();}
    if(bytes.empty() && progress)bytes=errors;
    Json result;try{result=parse_json(bytes);}catch(...){throw Error("update-outcome-unknown","The native updater returned no valid completion receipt. Review installation status before retrying.");}
    return result;
}
fs::path own_prefix() {
    const auto exe=fs::canonical(current_executable_path());return exe.parent_path().parent_path();
}
Json installation(const fs::path& prefix,const fs::path& user) {
    std::vector<std::string> a{"shin-ce","installation","--prefix",prefix.string()};std::vector<char*> p;for(auto& s:a)p.push_back(s.data());
    return local_installation((int)p.size(),p.data(),user);
}
Json offer(const std::string& current) {
    return select_local_release(parse_json(fetch("https://api.github.com/repos/toseek-samurai/shin-context-engine/releases?per_page=30",1024U*1024)),current);
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
        if(!enabled)return;
        @try {
            NSPipe* channel=[NSPipe pipe];
            const int descriptor=channel.fileHandleForWriting.fileDescriptor;
            if(::fcntl(descriptor,F_SETFL,O_NONBLOCK)!=0 || ::fcntl(descriptor,F_SETNOSIGPIPE,1)!=0 ||
                ::fcntl(descriptor,F_SETFD,FD_CLOEXEC)!=0)return;
            task_=[NSTask new];task_.executableURL=[NSURL fileURLWithPath:ns((prefix/"Shin Monitor.app/Contents/MacOS/Shin Monitor").string())];
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
Json local_updates(int argc,char** argv,const fs::path& user) {
 @autoreleasepool {
    const std::string command=argv[1];fs::path prefix=fs::absolute(own_prefix()).lexically_normal(),home;
    std::string revision,language,display_version;int parent=0;bool force_progress=false;std::set<std::string> seen;
    for(int i=2;i<argc;++i) {
        const std::string key=argv[i];
        if(key=="--progress" && seen.insert(key).second) {force_progress=true;continue;}
        require((key=="--prefix" || key=="--home" || key=="--revision" || key=="--wait-pid" || key=="--ui-language" || key=="--ui-version") && seen.insert(key).second && i+1<argc,"invalid-arguments","Invalid update option.");
        const std::string value=argv[++i];
        if(key=="--prefix")prefix=fs::absolute(value).lexically_normal();else if(key=="--home")home=fs::absolute(value).lexically_normal();else if(key=="--revision")revision=value;
        else if(key=="--ui-version") {
            require(!value.empty() && value.size()<=64 && value.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-")==std::string::npos,"invalid-arguments","Invalid display version.");display_version=value;
        }
        else if(key=="--ui-language") {require(value=="en" || value=="ru","invalid-arguments","Choose en or ru for the update window.");language=value;}
        else {try{std::size_t used=0;parent=std::stoi(value,&used);require(used==value.size() && parent>1 && parent==getppid(),"invalid-arguments","Only the launching Monitor may be awaited.");}catch(const Error&){throw;}catch(...){throw Error("invalid-arguments","Invalid launching Monitor PID.");}}
    }
    require(!parent || command=="update-install","invalid-arguments","Only an explicit Monitor installation may await its parent.");
    require((language.empty() && display_version.empty()) || parent,"invalid-arguments","Update window language requires the launching Monitor.");
    require(!force_progress || (!parent && (command=="update-install" || command=="update-download")),"invalid-arguments","--progress is available for terminal downloads and updates.");
    TerminalUpdateProgress terminal(force_progress,false,!parent && (command=="update-install" || command=="update-download"));
    terminal.report({ReleaseUpdateStep::preparing});
    const auto installed=installation(prefix,user);require(installed.value("integrity_verified",false),"installation-integrity-failed","Verify the owned installation before checking updates.");
    const auto current=installed.at("version").get<std::string>();
    if(home.empty())home=installed.at("home").get<std::string>();
    require(home==fs::path(installed.at("home").get<std::string>()),"installation-home-conflict","Use the installed kit's recorded data home.");
    if(command=="update-status" || command=="update-acknowledge") {
        const auto directory=home/"updates";
        if(!fs::exists(directory))return {{"ok",true},{"phase","idle"}};
        if(command=="update-status") {
            auto state=StateStore(directory,false).load().value_or(Json{{"ok",true},{"phase","idle"}});
            state["review_revision"]=digest(state.dump());
            state["can_acknowledge"]=!installed.value("recovery_pending",true) && state.value("phase","")!="waiting-for-monitor" && state.value("phase","")!="downloading" && state.value("phase","")!="installing" && state.value("phase","")!="idle" && state.value("phase","")!="reviewed";
            return state;
        }
        StateStore review(directory,true);review.recover_pending();auto previous=review.load();
        require(previous && !revision.empty() && digest(previous->dump())==revision,"update-offer-changed","The saved update outcome changed. Review it again.");
        require(!installed.value("recovery_pending",true),"update-outcome-unknown","Finish the native installation recovery journal before acknowledging the update result.");
        auto state=*previous;
        require(current==state.value("from_version","") || current==state.value("version",""),"update-outcome-unknown","The current installation differs from the saved update outcome.");
        if(state.contains("staging")) {
            const fs::path stage=state.at("staging").get<std::string>();
            require(stage.parent_path()==fs::canonical("/tmp") && stage.filename().string().starts_with("shin-release.") && directory_identity(stage,true)==state.value("stage_identity",""),"update-staging-changed","The saved candidate location changed; preserve it for review.");
            fs::remove_all(stage);state.erase("staging");state.erase("stage_identity");state.erase("candidate");
        }
        state["phase"]="reviewed";state["ok"]=true;review.save(state);return state;
    }
    // The viewer must exit before LaunchServices reopens this bundle.
    RelaunchMonitor relaunch{prefix};
    // Display-only hint from the review. The release revision is still rechecked below.
    UpdateProgressViewer viewer(prefix,display_version,language,parent!=0);
    const auto progress=[terminal_sink=terminal.sink(),viewer_sink=viewer.sink()](ReleaseUpdateProgress value) {
        report_update(terminal_sink,value);report_update(viewer_sink,value);
    };
    report_update(progress,{ReleaseUpdateStep::checking_release});
    auto selected=offer(current);selected["prefix"]=prefix.string();
    if(command=="update-check")return selected;
    require(command=="update-download" || command=="update-install","invalid-command","Unknown update command.");
    require(selected.at("available")==true && !revision.empty() && selected.at("revision")==revision,"update-offer-changed","The release offer changed. Check for updates again before installing.");
    const auto operation=[&](const ReleaseUpdateProgressSink& sink) -> Json {
    const auto report=[&](ReleaseUpdateStep step) {report_update(sink,{step});};
    const auto directory=home/"updates";ensure_private_directory(directory);
    StateStore owner(directory,true);owner.recover_pending();
    if(auto prior=owner.load();prior && prior->value("phase","")!="installed" && prior->value("phase","")!="reviewed" && prior->value("phase","")!="failed")throw Error("update-outcome-unknown","A previous update has no completion receipt. Review its saved candidate and installation status before retrying.");
    Json state{{"ok",true},{"phase","downloading"},{"from_version",current},{"version",selected.at("version")},{"checked_at_ms",now_ms()}};
    owner.save(state);
    char temporary[]="/tmp/shin-release.XXXXXXXX";char* folder=mkdtemp(temporary);require(folder,"update-download-failed","Cannot create a private staging folder.");
    const fs::path stage=fs::canonical(folder);state["staging"]=stage.string();state["stage_identity"]=directory_identity(stage,true);bool installing=false;owner.save(state);
    try {
        if(parent) {
            relaunch.active=true;state["phase"]="waiting-for-monitor";state["helper_pid"]=getpid();owner.save(state);
            std::cout << Json{{"accepted",true},{"pid",getpid()}}.dump() << std::endl;
            (void)freopen("/dev/null","w",stdout);
            report(ReleaseUpdateStep::waiting_for_monitor);
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(::kill(parent,0)==0 && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(::kill(parent,0)!=0,"update-monitor-open","Close Monitor before applying the update.");
            state["phase"]="downloading";owner.save(state);
        }
        const auto archive=stage/selected.at("archive_name").get<std::string>();
        report_update(sink,{ReleaseUpdateStep::downloading,0,selected.at("bytes").get<std::uint64_t>()});
        fetch(selected.at("archive_url"),128U*1024*1024,archive,sink,selected.at("bytes").get<std::uint64_t>());
        report(ReleaseUpdateStep::verifying_archive);
        const auto hash=digest(read_text(archive,128U*1024*1024,true));
        const auto checksum=fetch(selected.at("checksum_url"),512);
        const auto expected=hash+"  "+selected.at("archive_name").get<std::string>();
        require(fs::file_size(archive)==selected.at("bytes").get<std::uint64_t>() && hash==selected.at("sha256").get<std::string>() && (checksum==expected || checksum==expected+"\n"),"update-checksum-mismatch","The downloaded archive did not match the offered SHA-256. Installation was not started.");
        std::string listing;run("/usr/bin/tar",{"-tf",archive.string()},15,&listing);verify_release_listing(listing,selected.at("version"));
        report(ReleaseUpdateStep::extracting);
        ensure_private_directory(stage/"unpacked",true);
        std::string extracted;run("/usr/bin/tar",{"-xf",archive.string(),"-C",(stage/"unpacked").string()},30,&extracted);
        const auto kit=stage/"unpacked"/("shin-local-"+selected.at("version").get<std::string>()+"-macos-arm64");
        report(ReleaseUpdateStep::verifying_kit);
        const auto checked=verify_local_kit(kit);
        require(checked.at("version")==selected.at("version"),"update-release-invalid","The kit version differs from the offered release.");
        state["candidate"]=kit.string();state["phase"]="downloaded";owner.save(state);
        if(command=="update-download")return state;
        installing=true;state["phase"]="installing";owner.save(state);
        report(ReleaseUpdateStep::installing);
        const auto result=run(kit/"bin/shin-ce",{"update","--prefix",prefix.string(),"--progress-json"},600,nullptr,sink);
        state["result"]=result;state["ok"]=result.value("ok",false);state["phase"]=state.at("ok")==true ? "installed" : "review-required";
        owner.save(state);
        if(state.at("ok")==true) {report(ReleaseUpdateStep::confirming);const auto confirmed=installation(prefix,user);require(confirmed.at("version")==selected.at("version") && confirmed.value("integrity_verified",false),"update-outcome-unknown","The installed version did not confirm the update receipt.");state.erase("candidate");state.erase("staging");state.erase("stage_identity");owner.save(state);fs::remove_all(stage);}
    } catch(const std::exception& e) {
        const auto* typed=dynamic_cast<const Error*>(&e);
        state["ok"]=false;state["phase"]=installing ? "review-required" : "failed";state["error"]=typed ? error_json(*typed) : error_json(Error(installing ? "update-outcome-unknown" : "update-preparation-failed","The operation did not complete. Review the saved result before retrying."));state["staging"]=stage.string();owner.save(state);
        if(!installing) {require(directory_identity(stage,true)==state.at("stage_identity").get<std::string>(),"update-staging-changed","Private staging changed; preserve it for review.");fs::remove_all(stage);state.erase("candidate");state.erase("staging");state.erase("stage_identity");owner.save(state);}
    }
    report(state.value("ok",false) && state.value("phase","")=="installed" ? ReleaseUpdateStep::completed : ReleaseUpdateStep::failed);
    return state;
    };
    return operation(progress);
 }
}
} // namespace shin::observer
