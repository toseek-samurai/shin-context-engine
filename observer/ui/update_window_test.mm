#import <Cocoa/Cocoa.h>
#include "update_window_macos.h"
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>

using namespace shin::observer;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void collect(NSView* view, NSMutableArray<NSTextField*>* labels, NSMutableArray<NSProgressIndicator*>* bars) {
    if ([view isKindOfClass:NSTextField.class]) [labels addObject:(NSTextField*)view];
    if ([view isKindOfClass:NSProgressIndicator.class]) [bars addObject:(NSProgressIndicator*)view];
    for (NSView* child in view.subviews) collect(child, labels, bars);
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            check(argc == 3, "output directory and language required");
            const fs::path output = argv[1];
            const auto language = shin::ui::language_for(argv[2]);
            check(!release_download_percent({ReleaseUpdateStep::installing, 50, 100}), "invented installation percentage");
            check(!release_download_percent({ReleaseUpdateStep::downloading, 50, 0}), "invented unknown-size percentage");
            check(release_download_percent({ReleaseUpdateStep::downloading, 50, 100}) == 50, "download percentage differs from bytes");
            check(release_download_percent({ReleaseUpdateStep::downloading, 101, 100}) == 100, "oversized download overflowed its bar");

            int attempts = 0;
            try {
                run_release_update_window("test", language, [&](const auto&) -> Json {
                    ++attempts; throw std::runtime_error("fixture admission refusal");
                });
                throw std::runtime_error("admission refusal was hidden");
            } catch (const std::runtime_error& error) {
                check(std::string(error.what()) == "fixture admission refusal", "native failure was changed");
            }
            check(attempts == 1 && NSApp == nil, "refused operation was retried or started AppKit before acknowledgement");
            if (!NSScreen.screens.count) { std::cout << "Graphical session unavailable\n"; return 77; }
            fs::create_directories(output);
            ReleaseUpdateProgressSink late_callback;
            attempts = 0;
            const auto result = run_release_update_window("2.0.5-preview", language, [&](const auto& progress) -> Json {
                ++attempts; late_callback = progress;
                progress({ReleaseUpdateStep::waiting_for_monitor});
                auto capture = [&](ReleaseUpdateProgress value, const char* filename) {
                    struct Capture { std::atomic<bool> done{false}; std::string failure; };
                    const auto captured=std::make_shared<Capture>();
                    progress(value);
                    const auto path = output / filename;
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                      try {
                        @try {
                            NSWindow* window = nil;
                            for (NSWindow* candidate in NSApp.windows)
                                if (candidate.visible && [candidate.title isEqualToString:[NSString stringWithUTF8String:shin::ui::tr("Updating Shin")]]) window=candidate;
                            check(window != nil && !(window.styleMask & NSWindowStyleMaskClosable), "progress window missing or offers unsafe close");
                            auto* labels = [NSMutableArray<NSTextField*> array];
                            auto* bars = [NSMutableArray<NSProgressIndicator*> array];
                            collect(window.contentView, labels, bars);
                            check(bars.count == 1, "progress indicator missing");
                            NSString* expected = [NSString stringWithUTF8String:language == shin::ui::Language::ru
                                ? "Обновление Shin до 2.0.5-preview" : "Updating Shin to 2.0.5-preview"];
                            bool title_found = false;
                            for (NSTextField* label in labels) if ([label.stringValue isEqualToString:expected]) title_found = true;
                            check(title_found, "update version or selected language missing");
                            if (value.step == ReleaseUpdateStep::downloading)
                                check(!bars[0].indeterminate && bars[0].doubleValue == 50, "real byte progress not displayed");
                            else check(bars[0].indeterminate, "unmeasured stage displayed a percentage");
                            [window displayIfNeeded];
                            NSBitmapImageRep* bitmap = [window.contentView bitmapImageRepForCachingDisplayInRect:window.contentView.bounds];
                            [window.contentView cacheDisplayInRect:window.contentView.bounds toBitmapImageRep:bitmap];
                            NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                            check([png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES], "progress capture failed");
                        } @catch (NSException* error) {
                            const char* reason=error.reason.UTF8String;
                            captured->failure=reason ? reason : "progress capture exception";
                        }
                      } catch (const std::exception& error) { captured->failure=error.what(); }
                      captured->done=true;
                    });
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!captured->done && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    check(captured->done, "progress window did not stay responsive");
                    check(captured->failure.empty(),captured->failure.c_str());
                };
                capture({ReleaseUpdateStep::downloading, 12 * 1024 * 1024, 24 * 1024 * 1024}, "download.png");
                capture({ReleaseUpdateStep::installing}, "installing.png");
                capture({ReleaseUpdateStep::confirming}, "confirming.png");
                return {{"ok", false}, {"phase", "review-required"}, {"error", {{"code", "fixture-rollback"}}}};
            });
            check(attempts == 1 && !result.at("ok").get<bool>() && result.at("error").at("code") == "fixture-rollback",
                "UI retried or certified a failed native outcome");
            late_callback({ReleaseUpdateStep::downloading, 1, 2}); // No dangling window/stack capture after completion.
            for (NSWindow* window in NSApp.windows) check(!window.visible, "progress window survived completion");
            std::cout << "Real EN/RU progress window, bytes, indeterminate stages and native failure preservation passed\n";
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
