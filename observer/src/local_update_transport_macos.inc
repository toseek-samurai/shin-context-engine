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
#ifndef SHIN_UPDATE_FETCH_OVERRIDE
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
    NSString* address=ns(url);
    if(!address)throw Error("update-release-invalid","Invalid release URL encoding.");
    NSURL* endpoint=[NSURL URLWithString:address];
    if(!endpoint)throw Error("update-release-invalid","Invalid release URL.");
    NSMutableURLRequest* request=[NSMutableURLRequest requestWithURL:endpoint];
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
#else
std::string fetch(const std::string& url,std::size_t limit,const fs::path& output={},
                  const ReleaseUpdateProgressSink& progress={},std::uint64_t expected=0) {
    return SHIN_UPDATE_FETCH_OVERRIDE(url,limit,output,progress,expected);
}
#endif
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
} // namespace
} // namespace shin::observer
