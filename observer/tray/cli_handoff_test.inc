// Test-only process modes; execute the production NSTask reader and Cocoa exit.
@interface NativeCLIHost : ShinApp
@property(nonatomic, copy) NSString* fixtureCLI;
@property(nonatomic, copy) NSString* refusalMarker;
@end
@implementation NativeCLIHost
- (NSString*)nativeCLIPath { return self.fixtureCLI; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    if (!self.refusalMarker) return NSTerminateNow;
    std::ofstream(self.refusalMarker.fileSystemRepresentation) << "refused";
    return NSTerminateCancel;
}
@end

int native_cli_helper(int argc, char** argv) {
    const std::string mode = argv[argc - 2];
    const fs::path receipt = argv[argc - 1];
    const auto parent = getppid();
    if (mode == "oversized") {
        ::signal(SIGTERM, SIG_IGN);
        const std::string chunk(16 * 1024, 'x');
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            std::cout << chunk << std::flush;
            std::cerr << chunk << std::flush;
        }
        return 1;
    }
    if (mode == "stderr-error") {
        std::cerr << Json{{"ok", false}, {"error", {{"code", "update-monitor-open"},
            {"message", "Close Monitor before applying the update."}}}}.dump() << '\n';
        return 2;
    }
    std::cerr << "Downloading the current updater…\n" << std::flush;
    if (mode == "receipt") {
        std::cout << "{\"ok\":true,\"value\":17}\n";
        return 0;
    }
    const auto ack = Json{{"accepted", true}, {"pid", mode == "wrong-pid" ? getpid() + 1000000 : getpid()}}.dump() + "\n";
    if (mode == "stderr-ack") std::cerr << ack;
    else if (mode == "split-ack") {
        std::cout << ack.substr(0, ack.size() / 2) << std::flush;
        std::cerr << "A separate diagnostic between acknowledgement fragments.\n" << std::flush;
        std::cout << ack.substr(ack.size() / 2) << std::flush;
    } else std::cout << ack << std::flush;
    if (mode == "wrong-pid" || mode == "stderr-ack") return 1;
    if (mode == "refused-exit") {
        const auto marker = fs::path(receipt.string() + ".refused");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fs::exists(marker) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!fs::exists(marker)) return 3;
        std::cout << Json{{"ok", false}, {"phase", "failed"}, {"error", {{"code", "update-monitor-open"},
            {"message", "Close Monitor before applying the update."}}}}.dump(2) << '\n';
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (::kill(parent, 0) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool exited = ::kill(parent, 0) != 0;
    std::ofstream(receipt) << Json{{"parent_exited", exited}, {"helper_pid", getpid()}}.dump();
    return exited ? 0 : 1;
}

int native_cli_host(const char* cli, const char* home, const char* mode, const char* output) {
    NativeCLIHost* app = [NativeCLIHost new];
    app.fixtureCLI = diagnostic_text_content(cli);
    app.shinHome = diagnostic_text_content(home);
    if (std::string_view(mode) == "refused-exit") app.refusalMarker = diagnostic_text_content(std::string(output) + ".handoff.refused");
    NSApp.delegate = app;
    [app runCLI:std::string_view(mode) == "receipt" ? @"update-check" : @"update-install"
        arguments:@[diagnostic_text_content(mode), diagnostic_text_content(std::string(output) + ".handoff")]
        completion:^(Json result, NSString* failure) {
            std::ofstream(output) << Json{{"result", result}, {"failure", failure ? failure.UTF8String : ""}}.dump();
            [NSApp stop:nil];
            [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
        }];
    [NSApp run];
    NSApp.delegate = nil;
    return 0;
}
