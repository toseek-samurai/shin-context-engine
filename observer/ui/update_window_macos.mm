#import <Cocoa/Cocoa.h>
#include "update_window_macos.h"
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <memory>
#include <thread>
#include <unistd.h>

namespace shin::observer {
namespace {
NSString* ui_text(const char* value) { return [NSString stringWithUTF8String:shin::ui::tr(value)]; }
NSString* stage_text(ReleaseUpdateStep step) {
    switch (step) {
        case ReleaseUpdateStep::preparing: return ui_text("Preparing the update…");
        case ReleaseUpdateStep::waiting_for_monitor: return ui_text("Closing Shin Monitor…");
        case ReleaseUpdateStep::downloading: return ui_text("Downloading the update…");
        case ReleaseUpdateStep::verifying_archive: return ui_text("Verifying the download…");
        case ReleaseUpdateStep::extracting: return ui_text("Unpacking the update…");
        case ReleaseUpdateStep::verifying_kit: return ui_text("Checking the new kit…");
        case ReleaseUpdateStep::installing: return ui_text("Installing and restarting Shin…");
        case ReleaseUpdateStep::confirming: return ui_text("Confirming the installed version…");
    }
    return ui_text("Preparing the update…");
}
NSString* megabytes(std::uint64_t bytes) {
    char buffer[64]; std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(bytes) / (1024 * 1024));
    return [NSString stringWithUTF8String:shin::ui::decimal(buffer).c_str()];
}
}

Json run_release_update_window(const std::string& version, shin::ui::Language language,
                              const ReleaseUpdateOperation& operation) {
    shin::ui::set_language(language);
    struct Mailbox {
        std::mutex mutex;
        ReleaseUpdateProgress progress;
        bool reported = false, finished = false;
    };
    // Producers retain only this mailbox, never a window or a stack reference.
    auto mailbox = std::make_shared<Mailbox>();
    Json result;
    std::exception_ptr failure;
    std::jthread worker([&] {
        @autoreleasepool {
            try {
                result = operation([mailbox](ReleaseUpdateProgress value) {
                    std::lock_guard lock(mailbox->mutex);
                    mailbox->progress = value; mailbox->reported = true;
                });
            } catch (...) { failure = std::current_exception(); }
            std::lock_guard lock(mailbox->mutex); mailbox->finished = true;
        }
    });
    // Do not initialize a GUI for a refused/empty progress stream.
    for (;;) {
        { std::lock_guard lock(mailbox->mutex); if (mailbox->reported || mailbox->finished) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool already_finished;
    { std::lock_guard lock(mailbox->mutex); already_finished = mailbox->finished; }
    if (already_finished) {
        worker.join();
        if (failure) std::rethrow_exception(failure);
        return result;
    }
    bool visible = true;
    NSApplication* app = nil;
    NSWindow* window = nil;
    NSTextField* stage = nil;
    NSTextField* detail = nil;
    NSProgressIndicator* bar = nil;
    @try {
        app = [NSApplication sharedApplication];
        if (!NSScreen.screens.count) @throw [NSException exceptionWithName:@"ShinNoScreen" reason:nil userInfo:nil];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app finishLaunching];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 230)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        window.restorable = NO;
        window.title = ui_text("Updating Shin");
        NSStackView* stack = [NSStackView new];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading; stack.spacing = 14;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
            [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
            [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:24]]];
        NSTextField* title = [NSTextField labelWithString:[NSString stringWithFormat:ui_text("Updating Shin to %@"),
            [NSString stringWithUTF8String:version.c_str()]]];
        title.font = [NSFont systemFontOfSize:18 weight:NSFontWeightSemibold];
        stage = [NSTextField labelWithString:stage_text(ReleaseUpdateStep::preparing)];
        stage.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
        bar = [NSProgressIndicator new]; bar.style = NSProgressIndicatorStyleBar;
        bar.indeterminate = YES; bar.displayedWhenStopped = YES; bar.usesThreadedAnimation = YES; bar.minValue = 0; bar.maxValue = 100;
        bar.accessibilityLabel = ui_text("Update progress");
        [bar.heightAnchor constraintEqualToConstant:14].active = YES;
        detail = [NSTextField labelWithString:@""]; detail.textColor = NSColor.secondaryLabelColor;
        detail.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        NSTextField* footer = [NSTextField wrappingLabelWithString:ui_text("Shin Monitor will reopen when the update finishes.")];
        footer.font = [NSFont systemFontOfSize:12]; footer.textColor = NSColor.secondaryLabelColor;
        for (NSView* view in @[title, stage, bar, detail, footer]) [stack addArrangedSubview:view];
        [bar.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
        [window center]; [window makeKeyAndOrderFront:nil]; [app activate];
        [bar startAnimation:nil]; [window displayIfNeeded];
    } @catch (NSException* error) {
        NSLog(@"Shin update progress window unavailable: %@", error.reason);
        visible = false; // Continue the same operation if the graphical session is unavailable.
    }
    if (visible) {
        __block BOOL drawing = YES;
        NSTimer* timer = [NSTimer timerWithTimeInterval:.05 repeats:YES block:^(NSTimer* tick) {
            ReleaseUpdateProgress current;
            bool done;
            { std::lock_guard lock(mailbox->mutex); done = mailbox->finished; current = mailbox->progress; }
            if (done) {
                [tick invalidate]; [app stop:nil];
                [app postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                    modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
                return;
            }
            if (!drawing) return;
            @try {
                stage.stringValue = stage_text(current.step);
                if (const auto percent = release_download_percent(current)) {
                    if (bar.indeterminate) [bar stopAnimation:nil];
                    bar.indeterminate = NO; bar.doubleValue = *percent;
                    detail.stringValue = [NSString stringWithFormat:ui_text("%@ / %@ MB · %d%%"),
                        megabytes(current.received), megabytes(current.total), static_cast<int>(*percent)];
                } else {
                    if (!bar.indeterminate) { bar.indeterminate = YES; [bar startAnimation:nil]; }
                    detail.stringValue = @"";
                }
            } @catch (NSException*) { drawing = NO; }
        }];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
        @try { [app run]; } @catch (NSException*) { /* The reader still completes independently. */ }
        [timer invalidate];
    }
    worker.join();
    @try { [bar stopAnimation:nil]; [window close]; } @catch (NSException*) { /* Native outcome remains authoritative. */ }
    if (failure) std::rethrow_exception(failure);
    return result;
}

int release_update_progress_viewer(const std::string& version, shin::ui::Language language) {
    (void)run_release_update_window(version, language, [&](const ReleaseUpdateProgressSink& report) -> Json {
        std::string pending;
        char bytes[4096];
        for (;;) {
            const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            pending.append(bytes, static_cast<std::size_t>(count));
            if (pending.size() > 8192) break;
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                const auto row = Json::parse(pending.substr(0, end), nullptr, false);
                pending.erase(0, end + 1);
                if (!row.is_object() || row.size() != 3 || !row.contains("step") || !row["step"].is_number_unsigned() ||
                    !row.contains("received") || !row["received"].is_number_unsigned() ||
                    !row.contains("total") || !row["total"].is_number_unsigned()) continue;
                const auto step = row["step"].get<std::uint64_t>();
                if (step <= static_cast<unsigned>(ReleaseUpdateStep::confirming))
                    report({static_cast<ReleaseUpdateStep>(step), row["received"], row["total"]});
            }
        }
        return {{"ok", true}}; // End of observation, never an installation receipt.
    });
    return 0;
}
} // namespace shin::observer
