#import <Cocoa/Cocoa.h>
#include "update_window_macos.h"
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <memory>
#include <thread>
#include <unistd.h>

@interface ShinUpdateProgressContent : NSView
@end
@implementation ShinUpdateProgressContent
- (BOOL)isFlipped { return YES; }
@end

namespace shin::observer {
namespace {
NSString* ui_text(const char* value) { return [NSString stringWithUTF8String:shin::ui::tr(value)]; }
NSString* stage_text(ReleaseUpdateStep step) { return ui_text(update_stage_key(step)); }
NSColor* sand_ink() { return [NSColor colorWithCalibratedRed:.941 green:.937 blue:.890 alpha:1]; }
NSColor* sand_muted() { return [NSColor colorWithCalibratedRed:.690 green:.710 blue:.651 alpha:1]; }
NSColor* sand_accent() { return [NSColor colorWithCalibratedRed:.894 green:.800 blue:.635 alpha:1]; }
NSColor* update_blue() { return [NSColor colorWithCalibratedRed:.510 green:.753 blue:.941 alpha:1]; }
NSColor* update_green() { return [NSColor colorWithCalibratedRed:.510 green:.831 blue:.608 alpha:1]; }
NSColor* update_red() { return [NSColor colorWithCalibratedRed:.961 green:.549 blue:.514 alpha:1]; }

}

Json run_release_update_window(const std::string& version, shin::ui::Language language,
                              const ReleaseUpdateOperation& operation) {
    shin::ui::set_language(language);
    struct Mailbox {
        std::mutex mutex;
        UpdateTimeline timeline;
        std::chrono::steady_clock::time_point changed = std::chrono::steady_clock::now(), finished_at;
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
                    if (!mailbox->reported || mailbox->timeline.current.step != value.step) mailbox->changed = std::chrono::steady_clock::now();
                    mailbox->timeline.observe(value); mailbox->reported = true;
                });
            } catch (...) { failure = std::current_exception(); }
            std::lock_guard lock(mailbox->mutex); mailbox->finished = true; mailbox->finished_at = std::chrono::steady_clock::now();
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
    NSView* document = nil;
    NSStackView* stack = nil;
    NSMutableArray<NSTextField*>* rows = [NSMutableArray new];
    @try {
        app = [NSApplication sharedApplication];
        if (!NSScreen.screens.count) @throw [NSException exceptionWithName:@"ShinNoScreen" reason:nil userInfo:nil];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app finishLaunching];
        const auto available = NSScreen.mainScreen.visibleFrame.size;
        const auto width = std::min(620.0, std::max(320.0, available.width - 48));
        const auto height = std::min(680.0, std::max(320.0, available.height - 80));
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        window.restorable = NO;
        window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
        window.title = ui_text("Updating Shin");
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        window.backgroundColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
        window.contentView.wantsLayer = YES;
        window.contentView.layer.backgroundColor = window.backgroundColor.CGColor;
        window.contentView.layer.borderColor = sand_accent().CGColor;
        window.contentView.layer.borderWidth = 1;
        // All observed stages stay readable on smaller displays. This scroll
        // surface presents the same operation; it has no update authority.
        NSScrollView* scroll = [NSScrollView new];
        scroll.translatesAutoresizingMaskIntoConstraints = NO;
        scroll.drawsBackground = YES; scroll.backgroundColor = window.backgroundColor;
        scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES;
        document = [[ShinUpdateProgressContent alloc] initWithFrame:NSMakeRect(0, 0, width - 2, 676)];
        document.autoresizingMask = NSViewWidthSizable;
        scroll.documentView = document;
        [window.contentView addSubview:scroll];
        [NSLayoutConstraint activateConstraints:@[
            [scroll.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:1],
            [scroll.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-1],
            [scroll.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:1],
            [scroll.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-1]]];
        stack = [NSStackView new];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading; stack.spacing = 14;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [document addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:document.leadingAnchor constant:24],
            [stack.trailingAnchor constraintEqualToAnchor:document.trailingAnchor constant:-24],
            [stack.topAnchor constraintEqualToAnchor:document.topAnchor constant:24]]];
        NSTextField* title = [NSTextField wrappingLabelWithString:version.empty() ? ui_text("Updating Shin") :
            [NSString stringWithFormat:ui_text("Updating Shin to %@"), [NSString stringWithUTF8String:version.c_str()]]];
        title.textColor = sand_ink();
        title.font = [NSFont systemFontOfSize:23 weight:NSFontWeightMedium];
        stage = [NSTextField wrappingLabelWithString:stage_text(ReleaseUpdateStep::preparing)];
        stage.font = [NSFont systemFontOfSize:17 weight:NSFontWeightMedium]; stage.textColor = update_blue();
        bar = [NSProgressIndicator new]; bar.style = NSProgressIndicatorStyleBar;
        bar.indeterminate = YES; bar.displayedWhenStopped = YES; bar.usesThreadedAnimation = YES; bar.minValue = 0; bar.maxValue = 100;
        bar.accessibilityLabel = ui_text("Update progress");
        [bar.heightAnchor constraintEqualToConstant:12].active = YES;
        detail = [NSTextField wrappingLabelWithString:@""]; detail.textColor = sand_muted();
        detail.font = [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightRegular];
        NSTextField* footer = [NSTextField wrappingLabelWithString:ui_text("Shin Monitor will reopen when the update finishes.")];
        footer.font = [NSFont systemFontOfSize:14]; footer.textColor = sand_muted();
        for (NSView* view in @[title, stage, bar, detail]) [stack addArrangedSubview:view];
        NSStackView* steps = [NSStackView new]; steps.orientation = NSUserInterfaceLayoutOrientationVertical;
        steps.alignment = NSLayoutAttributeLeading; steps.spacing = 7;
        for (const auto step : update_steps) {
            NSTextField* row = [NSTextField wrappingLabelWithString:[@"○  " stringByAppendingString:stage_text(step)]];
            row.font = [NSFont systemFontOfSize:15]; row.textColor = sand_muted();
            [rows addObject:row]; [steps addArrangedSubview:row];
            [row.widthAnchor constraintEqualToAnchor:steps.widthAnchor].active = YES;
        }
        [stack addArrangedSubview:steps]; [stack addArrangedSubview:footer];
        for (NSView* view in @[title, stage, bar, detail, steps, footer])
            [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
        [window.contentView layoutSubtreeIfNeeded];
        document.frame = NSMakeRect(0, 0, scroll.contentSize.width,
            std::max(height - 2, stack.fittingSize.height + 48));
        [document layoutSubtreeIfNeeded];
        [window center]; [app activate]; [window makeKeyAndOrderFront:nil];
        [bar startAnimation:nil]; [window displayIfNeeded];
    } @catch (NSException* error) {
        NSLog(@"Shin update progress window unavailable: %@", error.reason);
        visible = false; // Continue the same operation if the graphical session is unavailable.
    }
    if (visible) {
        __block BOOL drawing = YES;
        NSTimer* timer = [NSTimer timerWithTimeInterval:.05 repeats:YES block:^(NSTimer* tick) {
            UpdateTimeline timeline;
            bool done; std::chrono::steady_clock::time_point changed, finished_at;
            { std::lock_guard lock(mailbox->mutex); done = mailbox->finished; timeline = mailbox->timeline;
                changed = mailbox->changed; finished_at = mailbox->finished_at; }
            const auto current = timeline.current;
            if (done && std::chrono::steady_clock::now() - finished_at >= std::chrono::milliseconds(400)) {
                [tick invalidate]; [app stop:nil];
                [app postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                    modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
                return;
            }
            if (!drawing) return;
            @try {
                stage.stringValue = stage_text(current.step);
                stage.textColor = current.step == ReleaseUpdateStep::failed ? update_red() :
                    current.step == ReleaseUpdateStep::completed ? update_green() : update_blue();
                auto percent = release_download_percent(current);
                if (current.step == ReleaseUpdateStep::staging && current.total)
                    percent = static_cast<double>(std::min(100.0L, 100.0L * current.received / current.total));
                if (percent) {
                    if (bar.indeterminate) [bar stopAnimation:nil];
                    bar.indeterminate = NO; bar.doubleValue = *percent;
                } else if (current.step == ReleaseUpdateStep::completed || current.step == ReleaseUpdateStep::failed) {
                    [bar stopAnimation:nil]; bar.indeterminate = NO;
                    bar.doubleValue = current.step == ReleaseUpdateStep::completed ? 100 : 0;
                } else if (!bar.indeterminate) { bar.indeterminate = YES; [bar startAnimation:nil]; }
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - changed).count();
                char counter[128]; const auto index = update_step_index(current.step);
                if (index < update_steps.size()) std::snprintf(counter, sizeof(counter), shin::ui::tr("Step %zu of %zu · %lld s"),
                    index + 1, update_steps.size(), static_cast<long long>(elapsed));
                else std::snprintf(counter, sizeof(counter), "%lld%s", static_cast<long long>(elapsed), shin::ui::tr(" s"));
                const auto values = update_detail(current);
                const auto summary = values.empty() ? std::string(counter) : values + " · " + counter;
                detail.stringValue = [NSString stringWithUTF8String:summary.c_str()];
                for (std::size_t i = 0; i < update_steps.size(); ++i) {
                    const bool failed = current.step == ReleaseUpdateStep::failed;
                    const bool recovery = current.step == ReleaseUpdateStep::rolling_back;
                    const bool active = index == i || ((failed || recovery) && timeline.active == i);
                    NSString* marker = active && failed ? @"×  " : active && recovery ? @"↶  " :
                        timeline.done[i] ? @"✓  " : active ? @"●  " : current.step == ReleaseUpdateStep::completed ? @"—  " : @"○  ";
                    rows[i].stringValue = [marker stringByAppendingString:stage_text(update_steps[i])];
                    rows[i].textColor = active && failed ? update_red() :
                        active ? update_blue() : timeline.done[i] ? update_green() : sand_muted();
                    rows[i].font = [NSFont systemFontOfSize:15 weight:active ? NSFontWeightMedium : NSFontWeightRegular];
                }
                [document layoutSubtreeIfNeeded];
                document.frame = NSMakeRect(0, 0, document.frame.size.width,
                    std::max(window.contentView.bounds.size.height - 2, stack.fittingSize.height + 48));
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
                if (const auto value = parse_update_progress(row)) report(*value);
            }
        }
        return {{"ok", true}}; // End of observation, never an installation receipt.
    });
    return 0;
}
} // namespace shin::observer
