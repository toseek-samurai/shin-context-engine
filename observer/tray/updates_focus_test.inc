// Native AppKit regression harness. Commands and URLs are intercepted; a second
// test-owned process supplies a genuinely different foreground application.
#include <iostream>
#include <fstream>
#include "cli_handoff_test.inc"

@interface UpdateWindowHarness : ShinApp
@property(nonatomic, copy) NSString* pendingCommand;
@property(nonatomic, copy) NSArray<NSString*>* pendingArguments;
@property(nonatomic, copy) void (^pendingReply)(Json, NSString*);
@property(nonatomic, strong) NSURL* openedURL;
@property(nonatomic) BOOL markedPending;
@property(nonatomic, strong) NSUserDefaults* loginPreferences;
@property(nonatomic) SMAppServiceStatus fakeLoginStatus;
@property(nonatomic) NSUInteger loginChanges;
@property(nonatomic) BOOL loginFailure;
- (void)reply:(const Json&)result failure:(NSString*)failure;
- (void)replaceBackgroundOffer:(const Json&)offer;
@end
@implementation UpdateWindowHarness
- (void)applicationDidFinishLaunching:(NSNotification*)notification { (void)notification; }

- (void)runCLI:(NSString*)command arguments:(NSArray<NSString*>*)arguments completion:(void (^)(Json, NSString*))completion {
    if (self.pendingReply) throw std::runtime_error("concurrent native update commands");
    self.pendingCommand = command; self.pendingArguments = arguments; self.pendingReply = completion;
}
- (void)reply:(const Json&)result failure:(NSString*)failure {
    if (!self.pendingReply) throw std::runtime_error("no pending update command");
    auto reply = self.pendingReply; self.pendingReply = nil; self.pendingCommand = nil;
    reply(result, failure);
}
- (void)replaceBackgroundOffer:(const Json&)offer { _updateOffer = offer; }
- (void)openUpdateReleaseURL:(NSURL*)url { self.openedURL = url; }
- (void)setUpdatePending:(BOOL)pending { self.markedPending = pending; }
- (SMAppServiceStatus)loginStartupStatus { return self.fakeLoginStatus; }
- (NSUserDefaults*)loginStartupPreferences { return self.loginPreferences; }
- (BOOL)setLoginStartupEnabled:(BOOL)enabled error:(NSError**)error {
    ++self.loginChanges;
    if (self.loginFailure) {
        if (error) *error = [NSError errorWithDomain:@"fixture" code:1 userInfo:nil];
        return NO;
    }
    self.fakeLoginStatus = enabled ? SMAppServiceStatusEnabled : SMAppServiceStatusNotRegistered;
    return YES;
}
@end

namespace {
void update_require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class Predicate> void await_update_ui(Predicate predicate, const char* reason) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(reason);
        @autoreleasepool {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.02]
                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (event) [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }
}
void save_update_window(UpdateWindowHarness* app, const fs::path& output, const char* name) {
    [app.updateWindow.contentView layoutSubtreeIfNeeded]; [app.updateWindow displayIfNeeded];
    NSView* body = app.updateWindow.contentView;
    for (NSView* control in @[app.updateTitle, app.updateVersion, app.updateCheckButton, app.updateInstallButton,
            app.updateNotesButton, app.updateCopyButton, app.updateAcknowledgeButton]) {
        if (control.hiddenOrHasHiddenAncestor) continue;
        const auto rect = [control convertRect:control.bounds toView:body];
        update_require(NSContainsRect(body.bounds, rect) && rect.size.height >= 10, "update control clipped outside window");
    }
    update_require(app.updateDetail.frame.size.height > 0 && app.updateDetail.frame.size.width > 100,
                   "update explanation has no readable layout");
    NSBitmapImageRep* bitmap = [body bitmapImageRepForCachingDisplayInRect:body.bounds];
    [body.effectiveAppearance performAsCurrentDrawingAppearance:^{
        [body cacheDisplayInRect:body.bounds toBitmapImageRep:bitmap];
    }];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    update_require(png.length && [png writeToFile:diagnostic_text_content((output / (std::string(name) + ".png")).string()) atomically:YES],
                   "update window snapshot failed");
}
}

int native_update_tests(int argc, char** argv) {
    @autoreleasepool {
        if (argc > 1 && std::string_view(argv[1]) == "--cli-helper") return native_cli_helper(argc, argv);
        [NSApplication sharedApplication];
        if (!NSScreen.screens.count) return 77;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory]; if (!NSApp.running) [NSApp finishLaunching];
        if (argc == 6 && std::string_view(argv[1]) == "--cli-host")
            return native_cli_host(argv[2], argv[3], argv[4], argv[5]);
        if (argc == 2 && std::string_view(argv[1]) == "--focus-peer") {
            NSWindow* peer = [[NSWindow alloc] initWithContentRect:NSMakeRect(20, 20, 320, 160)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            peer.title = @"Update focus test peer"; peer.releasedWhenClosed = NO;
            [NSFileHandle fileHandleWithStandardInput].readabilityHandler = ^(NSFileHandle* input) {
                NSData* data = input.availableData;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (!data.length || ((const char*)data.bytes)[0] == 'q') [NSApp stop:nil];
                    else { [NSApp activateIgnoringOtherApps:YES]; [peer makeKeyAndOrderFront:nil]; }
                    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
                });
            };
            [NSApp run]; [NSFileHandle fileHandleWithStandardInput].readabilityHandler = nil; [peer close]; return 0;
        }
        NSRunningApplication* prior = NSWorkspace.sharedWorkspace.frontmostApplication;
        NSTask* peer = [NSTask new]; NSPipe* commands = [NSPipe pipe];
        UpdateWindowHarness* app = [UpdateWindowHarness new];
        NSString* loginSuite = [@"org.shin.test.login." stringByAppendingString:NSUUID.UUID.UUIDString];
        app.loginPreferences = [[NSUserDefaults alloc] initWithSuiteName:loginSuite];
        try {
            update_require(argc == 3, "expected output directory and EN/RU language");
            shin::ui::set_language(shin::ui::language_for(argv[2]));
            fs::path output = argv[1]; fs::create_directories(output);
            NSAppleEventDescriptor* loginEvent = [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass
                eventID:kAEOpenApplication targetDescriptor:nil returnID:kAutoGenerateReturnID transactionID:kAnyTransactionID];
            update_require(!launched_as_login_item(loginEvent) && !launched_as_login_item(nil), "ordinary launch classified as login startup");
            [loginEvent setParamDescriptor:[NSAppleEventDescriptor descriptorWithEnumCode:keyAELaunchedAsLogInItem] forKeyword:keyAEPropData];
            update_require(launched_as_login_item(loginEvent), "login launch would open an unsolicited dashboard");
            [app configureInitialLoginStartup];
            update_require(app.loginChanges == 0, "development Monitor registered login startup");
            app.installedApplication = YES;
            app.loginFailure = YES;
            [app configureInitialLoginStartup];
            update_require(![app.loginPreferences boolForKey:@"ShinMonitorLoginConfigured"], "failed login registration became configured");
            app.loginFailure = NO;
            [app configureInitialLoginStartup];
            update_require(app.loginChanges == 2 && app.fakeLoginStatus == SMAppServiceStatusEnabled, "installed Monitor did not register login startup");
            app.fakeLoginStatus = SMAppServiceStatusNotRegistered; // User disabled it in System Settings.
            [app configureInitialLoginStartup];
            update_require(app.loginChanges == 2, "Monitor re-enabled a user's disabled login item");
            [app toggleLoginStartup:nil]; [app toggleLoginStartup:nil];
            [app configureInitialLoginStartup];
            update_require(app.loginChanges == 4 && app.fakeLoginStatus == SMAppServiceStatusNotRegistered, "explicit login opt-out was not preserved");
            NSMenuItem* login = [NSMenuItem new]; login.action = @selector(toggleLoginStartup:);
            app.fakeLoginStatus = SMAppServiceStatusRequiresApproval;
            update_require([app validateMenuItem:login] && login.state == NSControlStateValueMixed &&
                [login.title isEqualToString:ui_text("Approve Monitor login startup…")], "pending login approval was reported as enabled");
            app.fakeLoginStatus = SMAppServiceStatusEnabled;
            update_require([app validateMenuItem:login] && login.state == NSControlStateValueOn, "login toggle did not use live OS status");
            app.installedApplication = NO;
            update_require(![app validateMenuItem:login], "development bundle exposed login registration");
            peer.executableURL = [NSURL fileURLWithPath:diagnostic_text_content(current_executable_path().string())];
            peer.arguments = @[@"--focus-peer"]; peer.standardInput = commands;
            NSError* error = nil;
            update_require([peer launchAndReturnError:&error], "could not start focus peer");
            auto other_app = [&] {
                [commands.fileHandleForWriting writeData:[@"f" dataUsingEncoding:NSUTF8StringEncoding]];
                await_update_ui([&] { return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == peer.processIdentifier; },
                                "peer did not become foreground application");
            };
            const Json offer{{"ok", true}, {"available", true}, {"version", "2.0.5-dev.99"},
                {"current_version", "2.0.5-dev.98"},
                {"revision", "reviewed-revision"}, {"release_url", "https://github.com/example/project/releases/tag/v2.0.5-dev.99"}};
            [app beginUpdateCheck:NO];
            update_require(!app.updateWindow && app.pendingReply, "automatic check opened a window");
            [app showUpdateOffer:nil];
            await_update_ui([&] { return NSApp.active && app.updateWindow.keyWindow; }, "manual review did not take focus");
            NSWindow* window = app.updateWindow;
            update_require(app.updateScreen == UpdateScreen::checking, "manual review did not promote background check");
            save_update_window(app, output, "checking");
            [app checkForUpdates:nil];
            update_require(app.updateWindow == window && app.pendingReply, "repeated check replaced the window or request");
            other_app();
            [app reply:offer failure:nil];
            update_require(app.updateWindow == window && window.visible && app.updateScreen == UpdateScreen::offer &&
                NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == peer.processIdentifier,
                "background reply stole focus or lost the window");
            [app showUpdateWindow];
            await_update_ui([&] { return NSApp.active && window.keyWindow; }, "existing review did not regain focus");
            save_update_window(app, output, "available");
            update_require(window.contentView.bounds.size.height <= 360 &&
                [app.updateVersion.stringValue containsString:@"2.0.5-dev.98"] &&
                [app.updateVersion.stringValue containsString:@"2.0.5-dev.99"] &&
                !app.updateCheckButton.keyEquivalent.length && !app.updateInstallButton.keyEquivalent.length,
                "update review lost compact layout, versions or explicit installation");
            [app.updateNotesButton performClick:nil];
            update_require([app.openedURL.absoluteString isEqualToString:diagnostic_text_content(offer.at("release_url").get<std::string>())] &&
                app.updateScreen == UpdateScreen::offer && window.visible, "release notes dismissed review");
            other_app(); // Reproduce the browser taking foreground after Release notes.
            [app showUpdateWindow];
            await_update_ui([&] { return NSApp.active && window.keyWindow; }, "review stayed behind browser");
            [app replaceBackgroundOffer:{{"ok", true}, {"available", true}, {"version", "2.0.5-dev.100"}, {"revision", "new-revision"}}];
            [app.updateInstallButton performClick:nil];
            update_require([app.pendingCommand isEqualToString:@"update-install"] && app.markedPending &&
                [app.pendingArguments containsObject:@"reviewed-revision"] && ![app.pendingArguments containsObject:@"new-revision"] &&
                window.visible && app.updateScreen == UpdateScreen::starting && !app.updateInstallButton.enabled,
                "installation lost visible review, authority or progress handoff");
            save_update_window(app, output, "starting");
            [app installReviewedUpdate:nil];
            other_app();
            [app reply:{{"ok", false}, {"error", {{"code", "update-offer-changed"}}}} failure:nil];
            update_require(window.visible && !app.markedPending && !project_management_busy.load() &&
                app.updateScreen == UpdateScreen::startFailed &&
                NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == peer.processIdentifier,
                "failed install lost window, pending state or foreground ownership");
            save_update_window(app, output, "start-failed");
            [app checkForUpdates:nil]; [app reply:offer failure:nil];
            [app installReviewedUpdate:nil];
            [app reply:{{"ok", false}, {"error", {{"code", "update-monitor-open"},
                {"message", "Close Monitor before applying the update."}}}} failure:nil];
            update_require(!app.updateCopyButton.hidden &&
                [app.updateDetail.stringValue isEqualToString:ui_text("Monitor could not hand over to the updater. Your current installation is unchanged. Restart Monitor and check for updates again.")],
                "actionable update error or diagnostic action was lost");
            save_update_window(app, output, "monitor-open");
            for (int change = 0; change < 2; ++change) {
                shin::ui::set_language(shin::ui::language() == shin::ui::Language::ru ? shin::ui::Language::en : shin::ui::Language::ru);
                [app refreshLanguage:nil];
                update_require(app.updateScreen == UpdateScreen::startFailed && app.updateWindow == window &&
                    [app.updateDetail.stringValue isEqualToString:ui_text("Monitor could not hand over to the updater. Your current installation is unchanged. Restart Monitor and check for updates again.")],
                    "live language switch lost or failed to translate the update error");
            }
            [app showUpdateResult];
            [app reply:{{"ok", false}, {"phase", "failed"}, {"can_acknowledge", true}, {"review_revision", "failed-review"},
                {"error", {{"code", "update-monitor-open"}, {"message", "Close Monitor before applying the update."}}}} failure:nil];
            update_require(!app.updateCopyButton.hidden && !app.updateAcknowledgeButton.hidden &&
                [app.updateDetail.stringValue isEqualToString:ui_text("Monitor could not hand over to the updater. Your current installation is unchanged. Restart Monitor and check for updates again.")],
                "saved failure hid its cause or recovery actions");
            save_update_window(app, output, "saved-monitor-open");
            [app.updateCheckButton performClick:nil]; [window close];
            [app reply:offer failure:nil];
            update_require(!window.visible && app.updateScreen == UpdateScreen::offer, "late check reopened a closed review");
            [app showUpdateResult];
            [app reply:{{"ok", false}, {"phase", "review-required"}, {"can_acknowledge", true}, {"review_revision", "result-revision"}} failure:nil];
            update_require(window.visible && !app.updateCopyButton.hidden && !app.updateAcknowledgeButton.hidden,
                "review result lost recovery actions");
            save_update_window(app, output, "review-required");
            // Preserve the user's pasteboard: prove the button remains visible without clicking Copy here.
            [app.updateAcknowledgeButton performClick:nil];
            update_require([app.pendingCommand isEqualToString:@"update-acknowledge"] &&
                [app.pendingArguments containsObject:@"result-revision"], "acknowledgement lost revision");
            [app reply:{{"ok", false}, {"error", {{"message", "Unfinished operation"}}}} failure:nil];
            update_require(window.visible && app.updateScreen == UpdateScreen::acknowledgeFailed && !app.updateAcknowledgeButton.hidden,
                "failed acknowledgement dismissed recovery");
            [app.updateAcknowledgeButton performClick:nil];
            [app reply:{{"ok", true}, {"phase", "reviewed"}} failure:nil];
            update_require(app.updateScreen == UpdateScreen::resultReady && app.updateCopyButton.hidden &&
                app.updateAcknowledgeButton.hidden && window.visible, "verified result did not settle in same window");
            [app checkForUpdates:nil]; [app reply:{{"ok", true}, {"available", false}} failure:nil];
            save_update_window(app, output, "current");
            [app checkForUpdates:nil]; [app reply:{{"ok", false}} failure:@"offline"];
            save_update_window(app, output, "offline");
            const auto beforeLanguage = app.updateScreen;
            shin::ui::set_language(shin::ui::language() == shin::ui::Language::ru ? shin::ui::Language::en : shin::ui::Language::ru);
            [app refreshLanguage:nil];
            update_require(app.updateScreen == beforeLanguage && app.updateWindow == window &&
                [window.title isEqualToString:ui_text("Shin updates")] &&
                [app.updateTitle.stringValue isEqualToString:ui_text("Could not check for updates")], "language switch reset update review");
            [window close];
            // Exercise the native popup lifecycle and automatic Metal frame delivery.
            // All Monitor reads target this disposable home.
            monitor = std::make_unique<Monitor>(output / "no-service-home");
            app.item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
            NSImage* logo = [NSImage imageWithSystemSymbolName:@"circle.fill" accessibilityDescription:@"Shin Monitor"];
            logo.size = NSMakeSize(20, 20); [logo setTemplate:YES];
            NSImage* badge = shin_status_image(logo, true);
            for (int i = 0; i < 20; ++i) {
                shin_status_presentation(app.item, i % 2 ? logo : badge);
                update_require(app.item.length == NSSquareStatusItemLength && !app.item.button.title.length &&
                    app.item.button.imagePosition == NSImageOnly && app.item.button.image.isTemplate &&
                    [app.item.button.image.accessibilityDescription isEqualToString:logo.accessibilityDescription] &&
                    NSEqualSizes(app.item.button.image.size, NSMakeSize(20, 20)), "update badge changed the status slot");
            }
            app.content = [ShinView new]; app.popover = [NSPopover new];
            app.popover.animates = NO;
            app.popover.behavior = NSPopoverBehaviorApplicationDefined;
            app.popover.delegate = app; app.popover.contentViewController = app.content;
            NSApp.delegate = app;
            // Production defers first presentation by one timer tick while
            // Control Center commits the status-item scene, beyond proxy layout.
            const auto anchorCreated = std::chrono::steady_clock::now();
            await_update_ui([&] { return std::chrono::steady_clock::now() - anchorCreated >= std::chrono::milliseconds(500); },
                "initial status-item presentation did not settle");
            [app toggle:nil];
            // The real timer retries a deferred initial presentation after
            // Control Center attaches the status button to its native window.
            await_update_ui([&] { return app.item.button.window.visible && !app.item.button.hiddenOrHasHiddenAncestor; }, "status item did not acquire an anchor window");
            if (app.initialPresentationPending) [app toggle:nil];
            await_update_ui([&] { return app.popover.shown && app.content.view.window.keyWindow; }, "explicit open did not restore the tray popup");
            update_require(app.item.visible && NSApp.activationPolicy == NSApplicationActivationPolicyAccessory,
                "tray open changed application policy or hid its status item");
            update_require(![app respondsToSelector:NSSelectorFromString(@"showMonitorWindow")] &&
                ![app popoverShouldDetach:app.popover], "standalone dashboard remains available");
            update_require(app.popover.contentViewController == app.content && monitor->visible &&
                !((MTKView*)app.content.view).paused && app.content.view.window.acceptsMouseMovedEvents,
                "popup lost rendering ownership or pointer delivery");
            std::string phase = "initial";
            auto advanceFrames = [&] {
                const auto before = ImGui::GetFrameCount();
                const auto failure = "native Metal frame delivery stalled during " + phase;
                await_update_ui([&] { return ImGui::GetFrameCount() >= before + 4; }, failure.c_str());
            };
            advanceFrames();
            dashboard_view = 1;
            auto tabClick = [&](int code) {
                MTKView* metal = (MTKView*)app.content.view;
                if (app.popover.shown) {
                    auto frame = metal.window.frame;
                    auto settled = std::chrono::steady_clock::now();
                    await_update_ui([&] {
                        if (!NSEqualRects(frame, metal.window.frame)) {
                            frame = metal.window.frame; settled = std::chrono::steady_clock::now();
                        }
                        return std::chrono::steady_clock::now() - settled > std::chrono::milliseconds(150);
                    }, "popover anchor did not settle before pointer delivery");
                }
                advanceFrames();
                const auto tab = std::find_if(ui_actions.begin(), ui_actions.end(), [&](const auto& a) { return a.chart == code; });
                update_require(tab != ui_actions.end(), "navigation tab absent from rendered frame");
                const auto y = tab->origin.y + tab->size.y / 2;
                const NSPoint local = NSMakePoint(tab->origin.x + tab->size.x / 2,
                    metal.isFlipped ? y : metal.bounds.size.height - y);
                const NSPoint location = [metal convertPoint:local toView:nil];
                for (auto type : {NSEventTypeMouseMoved, NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
                    [NSApp postEvent:[NSEvent mouseEventWithType:type location:location modifierFlags:0
                        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:metal.window.windowNumber
                        context:nil eventNumber:1 clickCount:1 pressure:1] atStart:NO];
                const auto failure = std::string(app.popover.shown ? "popup" : "standalone") +
                    " ignored pointer navigation to " + std::to_string(code);
                try { await_update_ui([&] { return dashboard_view == code - 10; }, failure.c_str()); }
                catch (...) {
                    const auto& io = ImGui::GetIO();
                    std::cerr << "Input state: paused=" << metal.paused << " popup=" << app.popover.shown
                        << " key=" << metal.window.keyWindow << " view=" << dashboard_view
                        << " mouse=" << io.MousePos.x << ',' << io.MousePos.y
                        << " size=" << io.DisplaySize.x << ',' << io.DisplaySize.y << " height=" << metal.bounds.size.height
                        << " flipped=" << metal.isFlipped
                        << " down=" << io.MouseDown[0] << '\n';
                    throw;
                }
            };
            tabClick(10); tabClick(13); tabClick(11);
            const auto owner = app.popover.contentViewController;
            for (int cycle = 0; cycle < 6; ++cycle) {
                phase = "reopen-" + std::to_string(cycle);
                [app closePanel:nil];
                await_update_ui([&] { return !app.popover.shown; }, "popup did not close");
                update_require(((MTKView*)app.content.view).paused && !monitor->visible,
                    "closed popup kept rendering or service visibility");
                [app applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
                await_update_ui([&] { return app.popover.shown; }, "app reopen did not restore the popup");
                advanceFrames();
                tabClick(cycle % 2 ? 10 : 11);
                update_require(app.popover.contentViewController == owner &&
                    NSApp.activationPolicy == NSApplicationActivationPolicyAccessory,
                    "popup reopen moved the renderer or introduced Dock access");
            }
            phase = "update-review";
            [app showUpdateWindow];
            update_require(!app.popover.shown, "update review did not close the popup");
            [app.updateWindow close];
            [app applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
            await_update_ui([&] { return app.popover.shown; }, "update review prevented popup reopening");
            advanceFrames(); tabClick(13); tabClick(11);
            // No direct draw calls: detect the stale-frame failure even while
            // paused=false, with a period of idle native rendering between clicks.
            phase = "idle";
            for (int sample = 0; sample < 12; ++sample) advanceFrames();
            phase = "focus";
            other_app();
            await_update_ui([&] { return !app.popover.shown; }, "popup survived focus loss");
            [app applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
            await_update_ui([&] { return app.popover.shown; }, "focus change prevented popup reopening");
            advanceFrames(); tabClick(10); tabClick(11);
            phase = "escape";
            [NSApp postEvent:[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
                timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:app.content.view.window.windowNumber
                context:nil characters:@"\033" charactersIgnoringModifiers:@"\033" isARepeat:NO keyCode:53] atStart:NO];
            await_update_ui([&] { return !app.popover.shown; }, "Escape did not close the popup");
            [app applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
            await_update_ui([&] { return app.popover.shown; }, "Escape prevented popup reopening");
            advanceFrames(); tabClick(10); tabClick(11);
            phase = "compact";
            const NSSize originalSize = app.popover.contentSize;
            [app toggleCompact:nil]; advanceFrames();
            update_require(!NSEqualSizes(app.popover.contentSize, originalSize), "popup ignored Compact view");
            [app toggleCompact:nil]; advanceFrames();
            update_require(NSEqualSizes(app.popover.contentSize, originalSize), "popup did not restore its size");
            [app popoverDidClose:[NSNotification notificationWithName:NSPopoverDidCloseNotification object:app.popover]];
            update_require(!((MTKView*)app.content.view).paused && monitor->visible,
                "late close notification paused a reopened popup");
            advanceFrames();
            [app closePanel:nil];
            [NSStatusBar.systemStatusBar removeStatusItem:app.item]; app.item = nil;
            [app applicationWillTerminate:[NSNotification notificationWithName:NSApplicationWillTerminateNotification object:NSApp]];
            update_require(!monitor && !app.popover.shown && ((MTKView*)app.content.view).paused,
                "termination left a live presentation without its model");
            [app popoverDidClose:[NSNotification notificationWithName:NSPopoverDidCloseNotification object:app.popover]];
            std::cout << "Native update review: retained window, foreground handoff, no background focus theft, closed-window suppression, pinned installation and acknowledgement, EN/RU passed\n";
            std::cout << "Tray popup: fixed status width, single content owner, native frame delivery, pointer navigation, reopen, focus, update review and compact sizing passed\n";
            std::cout << "Termination: popup teardown and late close notification passed\n";
            std::cout << "Login startup: installed-only registration, failure retry, opt-out and live approval status passed\n";
            [app.loginPreferences removePersistentDomainForName:loginSuite];
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            NSLog(@"Anchor frame %@ button=%@ visibleRect=%@ visible=%d hidden=%d popup size=%@", NSStringFromRect(app.item.button.window.frame), NSStringFromRect(app.item.button.bounds), NSStringFromRect(app.item.button.visibleRect), app.item.button.window.visible, app.item.button.hiddenOrHasHiddenAncestor, NSStringFromSize(app.popover.contentSize));
            std::cerr << "Popup state: shown=" << app.popover.shown << " pending=" << app.initialPresentationPending
                << " anchor=" << (app.item.button.window != nil) << " key=" << app.content.view.window.keyWindow
                << " active=" << NSApp.active << " paused=" << ((MTKView*)app.content.view).paused << '\n';
            [app.updateWindow close]; if (peer.running) [peer terminate];
            if (prior) [prior activateWithOptions:0]; return 1;
        }
        [app.updateWindow close];
        if (peer.running) { [commands.fileHandleForWriting writeData:[@"q" dataUsingEncoding:NSUTF8StringEncoding]]; [peer waitUntilExit]; }
        if (prior) [prior activateWithOptions:0];
        return 0;
    }
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc > 1 && std::string_view(argv[1]).starts_with("--")) return native_update_tests(argc, argv);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        __block int result = 1;
        // Run under the real NSApplication loop. A hand-pumped, not-running
        // application does not exercise the normal status-item/popup lifecycle.
        [NSTimer scheduledTimerWithTimeInterval:.1 repeats:NO block:^(NSTimer*) {
            result = native_update_tests(argc, argv);
            [NSApp stop:nil];
            [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
        }];
        [NSApp run];
        return result;
    }
}
