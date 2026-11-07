// Test-only process modes; execute the production NSTask reader and Cocoa exit.
@interface NativeCLIHost : ShinApp
@property(nonatomic, copy) NSString* fixtureCLI;
@property(nonatomic, copy) NSString* refusalMarker;
@end
@implementation NativeCLIHost
- (NSString*)nativeCLIPath { return self.fixtureCLI; }
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    // NSApp can deliver this after finishLaunching. Keep the fixture's home,
    // windows and reads independent of the installed Monitor and preferences.
    (void)notification;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    if (!self.refusalMarker) return NSTerminateNow;
    std::ofstream(self.refusalMarker.fileSystemRepresentation) << "refused";
    return NSTerminateCancel;
}
@end

int native_cli_helper(int argc, char** argv) {
    std::string_view mode = argv[argc - 2];
    if (mode.starts_with("popup-")) mode.remove_prefix(6);
    const fs::path receipt = argv[argc - 1];
    const auto parent = getppid();
    if (mode == "project-ready" || mode == "project-error" || mode == "project-service-error") {
        const auto stage = [](const char* value) {
            const auto frame = Json{{"schema_version", "shin.project-progress.v1"}, {"stage", value}}.dump() + "\n";
            std::cerr << frame.substr(0, 3) << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            std::cerr << frame.substr(3) << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        };
        stage("checking-project"); stage("candidate-build"); stage("source-cache");
        if (mode == "project-error") {
            std::cerr << Json{{"ok", false}, {"error", {{"code", "fixture-project-error"}, {"message", "Fixture refused."}}}}.dump() << '\n';
            return 2;
        }
        stage("starting-service"); stage("checking-connection");
        if (mode == "project-service-error") {
            std::cout << Json{{"ok", false}, {"service", {{"ok", true}, {"error", {{"code", "fixture-project-error"}}}}}}.dump() << '\n';
            return 1;
        }
        std::cout << Json{{"ok", true}, {"service_enrolled", true}, {"service", {{"ok", true}}}}.dump() << '\n';
        return 0;
    }
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

int native_cli_host(const char* cli, const char* home, const char* modeText, const char* output) {
    const std::string_view mode = modeText;
    const auto scenario = mode.starts_with("popup-") ? mode.substr(6) : mode;
    // NSApplication and NSWindow keep weak delegate references.
    __attribute__((objc_precise_lifetime)) NativeCLIHost* app = [NativeCLIHost new];
    app.fixtureCLI = diagnostic_text_content(cli);
    app.shinHome = diagnostic_text_content(home);
    if (scenario == "refused-exit") app.refusalMarker = diagnostic_text_content(std::string(output) + ".handoff.refused");
    NSApp.delegate = app;
    if (scenario == "project-ready" || scenario == "project-error" || scenario == "project-service-error") {
        project_management_busy = true;
        [app connectProjectRoot:@"/fixture/project"];
        __block BOOL sawProgress = NO, stayedVisible = YES, controlsLocked = YES;
        NSTimer* sample = [NSTimer scheduledTimerWithTimeInterval:.02 repeats:YES block:^(NSTimer* timer) {
            stayedVisible = stayedVisible && app.setupWindow.visible;
            if (app.setupAdding) {
                sawProgress = sawProgress || app.setupProgressStage >= 1;
                controlsLocked = controlsLocked && !app.setupChoose.enabled &&
                    ![app.setupWindow standardWindowButton:NSWindowCloseButton].enabled;
                return;
            }
            [timer invalidate];
            std::ofstream(output) << Json{{"progress_before_completion", (bool)sawProgress}, {"window_stayed_visible", (bool)stayedVisible},
                {"controls_locked", (bool)controlsLocked}, {"retry_enabled", (bool)app.setupChoose.enabled},
                {"failed", (bool)app.setupPreparationFailed}, {"status", app.setupStatus.stringValue.UTF8String},
                {"progress_mask", app.setupProgressMask}, {"operation_released", !project_management_busy.load()}}.dump();
            [NSApp stop:nil];
            [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
        }];
        [NSApp run]; [sample invalidate]; [app.setupWindow close]; NSApp.delegate = nil;
        return 0;
    }
    if (mode.starts_with("popup-")) {
        monitor = std::make_unique<Monitor>(home);
        app.content = [ShinView new]; app.popover = [NSPopover new];
        app.popover.delegate = app; app.popover.contentViewController = app.content;
        app.popover.animates = NO;
        app.popover.behavior = NSPopoverBehaviorApplicationDefined;
        app.item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
        shin_status_presentation(app.item, [NSImage imageWithSystemSymbolName:@"circle.fill" accessibilityDescription:@"Shin Monitor test"]);
        const auto anchorDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < anchorDeadline) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.02]
                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (event) [NSApp sendEvent:event];
        }
        [app toggle:nil];
    }
    [app runCLI:scenario == "receipt" ? @"update-check" : @"update-install"
        arguments:@[diagnostic_text_content(modeText), diagnostic_text_content(std::string(output) + ".handoff")]
        completion:^(Json result, NSString* failure) {
            std::ofstream(output) << Json{{"result", result}, {"failure", failure ? failure.UTF8String : ""},
                {"popup_visible", (bool)app.popover.shown}}.dump();
            [NSApp stop:nil];
            [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
        }];
    [NSApp run];
    NSApp.delegate = nil;
    [app applicationWillTerminate:[NSNotification notificationWithName:NSApplicationWillTerminateNotification object:NSApp]];
    return 0;
}
