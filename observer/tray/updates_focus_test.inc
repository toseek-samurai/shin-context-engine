// Native AppKit regression harness. Commands and URLs are intercepted; a second
// test-owned process supplies a genuinely different foreground application.
#include <iostream>

@interface UpdateWindowHarness : ShinApp
@property(nonatomic, copy) NSString* pendingCommand;
@property(nonatomic, copy) NSArray<NSString*>* pendingArguments;
@property(nonatomic, copy) void (^pendingReply)(Json, NSString*);
@property(nonatomic, strong) NSURL* openedURL;
@property(nonatomic) BOOL markedPending;
- (void)reply:(const Json&)result failure:(NSString*)failure;
- (void)replaceBackgroundOffer:(const Json&)offer;
@end
@implementation UpdateWindowHarness
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
    for (NSView* control in @[app.updateTitle, app.updateCheckButton, app.updateInstallButton,
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

int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if (!NSScreen.screens.count) return 77;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory]; [NSApp finishLaunching];
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
        try {
            update_require(argc == 3, "expected output directory and EN/RU language");
            shin::ui::set_language(shin::ui::language_for(argv[2]));
            fs::path output = argv[1]; fs::create_directories(output);
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
            std::cout << "Native update review: retained window, foreground handoff, no background focus theft, closed-window suppression, pinned installation and acknowledgement, EN/RU passed\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            [app.updateWindow close]; if (peer.running) [peer terminate];
            if (prior) [prior activateWithOptions:0]; return 1;
        }
        [app.updateWindow close];
        if (peer.running) { [commands.fileHandleForWriting writeData:[@"q" dataUsingEncoding:NSUTF8StringEncoding]]; [peer waitUntilExit]; }
        if (prior) [prior activateWithOptions:0];
        return 0;
    }
}
