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
NSString* stage_text(ReleaseUpdateStep step) { return ui_text(update_stage_key(step)); }
NSColor* sand_ink() { return [NSColor colorWithCalibratedRed:.914 green:.914 blue:.867 alpha:1]; }
NSColor* sand_muted() { return [NSColor colorWithCalibratedRed:.627 green:.647 blue:.584 alpha:1]; }
NSColor* sand_accent() { return [NSColor colorWithCalibratedRed:.839 green:.776 blue:.635 alpha:1]; }

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
    NSMutableArray<NSTextField*>* rows = [NSMutableArray new];
    @try {
        app = [NSApplication sharedApplication];
        if (!NSScreen.screens.count) @throw [NSException exceptionWithName:@"ShinNoScreen" reason:nil userInfo:nil];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app finishLaunching];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 580, 526)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        window.restorable = NO;
        window.title = ui_text("Updating Shin");
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        window.backgroundColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
        window.contentView.wantsLayer = YES;
        window.contentView.layer.backgroundColor = window.backgroundColor.CGColor;
        NSStackView* stack = [NSStackView new];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading; stack.spacing = 14;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
            [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
            [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:24]]];
        NSTextField* title = [NSTextField labelWithString:version.empty() ? ui_text("Updating Shin") :
            [NSString stringWithFormat:ui_text("Updating Shin to %@"), [NSString stringWithUTF8String:version.c_str()]]];
        title.textColor = sand_ink();
        title.font = [NSFont systemFontOfSize:18 weight:NSFontWeightSemibold];
        stage = [NSTextField labelWithString:stage_text(ReleaseUpdateStep::preparing)];
        stage.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium]; stage.textColor = sand_accent();
        bar = [NSProgressIndicator new]; bar.style = NSProgressIndicatorStyleBar;
        bar.indeterminate = YES; bar.displayedWhenStopped = YES; bar.usesThreadedAnimation = YES; bar.minValue = 0; bar.maxValue = 100;
        bar.accessibilityLabel = ui_text("Update progress");
        [bar.heightAnchor constraintEqualToConstant:14].active = YES;
        detail = [NSTextField labelWithString:@""]; detail.textColor = sand_muted();
        detail.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        NSTextField* footer = [NSTextField wrappingLabelWithString:ui_text("Shin Monitor will reopen when the update finishes.")];
        footer.font = [NSFont systemFontOfSize:12]; footer.textColor = sand_muted();
        for (NSView* view in @[title, stage, bar, detail]) [stack addArrangedSubview:view];
        NSStackView* steps = [NSStackView new]; steps.orientation = NSUserInterfaceLayoutOrientationVertical;
        steps.alignment = NSLayoutAttributeLeading; steps.spacing = 7;
        for (const auto step : update_steps) {
            NSTextField* row = [NSTextField labelWithString:[@"○  " stringByAppendingString:stage_text(step)]];
            row.font = [NSFont systemFontOfSize:12]; row.textColor = sand_muted();
            [rows addObject:row]; [steps addArrangedSubview:row];
        }
        [stack addArrangedSubview:steps]; [stack addArrangedSubview:footer];
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
                    rows[i].textColor = active && failed ? [NSColor colorWithCalibratedRed:.863 green:.635 blue:.596 alpha:1] :
                        active ? sand_accent() : timeline.done[i] ? sand_ink() : sand_muted();
                    rows[i].font = [NSFont systemFontOfSize:12 weight:active ? NSFontWeightSemibold : NSFontWeightRegular];
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
                if (const auto value = parse_update_progress(row)) report(*value);
            }
        }
        return {{"ok", true}}; // End of observation, never an installation receipt.
    });
    return 0;
}
} // namespace shin::observer
