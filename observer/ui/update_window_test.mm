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
                    // Start observing immediately, including before the UI's
                    // first paint. A fixed sleep races cold AppKit startup.
                    dispatch_async(dispatch_get_main_queue(), ^{
                      NSTimer* observer = [NSTimer timerWithTimeInterval:.01 repeats:YES block:^(NSTimer* tick) {
                      if (captured->done) { [tick invalidate]; return; }
                      try {
                        @try {
                            NSWindow* window = nil;
                            for (NSWindow* candidate in NSApp.windows)
                                if (candidate.visible && [candidate.title isEqualToString:[NSString stringWithUTF8String:shin::ui::tr("Updating Shin")]]) window=candidate;
                            if (!window) return;
                            check(!(window.styleMask & NSWindowStyleMaskClosable), "progress window offers unsafe close");
                            auto* labels = [NSMutableArray<NSTextField*> array];
                            auto* bars = [NSMutableArray<NSProgressIndicator*> array];
                            collect(window.contentView, labels, bars);
                            check(bars.count == 1, "progress indicator missing");
                            NSString* expected = [NSString stringWithUTF8String:language == shin::ui::Language::ru
                                ? "Обновление Shin до 2.0.5-preview" : "Updating Shin to 2.0.5-preview"];
                            bool title_found = false;
                            for (NSTextField* label in labels) if ([label.stringValue isEqualToString:expected]) title_found = true;
                            check(title_found, "update version or selected language missing");
                            const char* stage_key = update_stage_key(value.step);
                            NSString* expected_stage = [NSString stringWithUTF8String:shin::ui::tr(stage_key)];
                            bool stage_found = false;
                            for (NSTextField* label in labels) if ([label.stringValue isEqualToString:expected_stage]) stage_found = true;
                            if (!stage_found) return;
                            if (value.step == ReleaseUpdateStep::downloading || value.step == ReleaseUpdateStep::staging) {
                                if (bars[0].indeterminate || bars[0].doubleValue != 50) return;
                            } else if (value.step == ReleaseUpdateStep::failed) {
                                if (bars[0].indeterminate || bars[0].doubleValue != 0) return;
                            } else if (!bars[0].indeterminate) return;
                            for (const auto planned : update_steps) {
                                bool found = false;
                                for (NSTextField* label in labels) {
                                    if ([label.stringValue hasSuffix:[NSString stringWithUTF8String:shin::ui::tr(update_stage_key(planned))]] &&
                                        ([label.stringValue hasPrefix:@"○  "] || [label.stringValue hasPrefix:@"●  "] || [label.stringValue hasPrefix:@"✓  "] || [label.stringValue hasPrefix:@"×  "] || [label.stringValue hasPrefix:@"↶  "])) found = true;
                                    check(NSContainsRect(window.contentView.bounds, [label convertRect:label.bounds toView:window.contentView]), "update label clipped outside window");
                                }
                                check(found, "full update plan missing");
                            }
                            if (value.step == ReleaseUpdateStep::checking_projects) {
                                bool counts = false, completed = false;
                                for (NSTextField* label in labels) {
                                    if ([label.stringValue containsString:@"2 / 3"]) counts = true;
                                    if ([label.stringValue isEqualToString:[@"✓  " stringByAppendingString:[NSString stringWithUTF8String:shin::ui::tr("Verifying the download…")]]]) completed = true;
                                }
                                check(counts && completed, "project counts or fast completed stages lost");
                            }
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
                      [tick invalidate];
                      captured->done=true;
                      }];
                      [NSRunLoop.mainRunLoop addTimer:observer forMode:NSRunLoopCommonModes];
                    });
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!captured->done && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (!captured->done) {
                        captured->done=true; // Retire a delayed observer without stack/UI captures.
                        throw std::runtime_error("progress window did not display the requested stage and byte state within five seconds");
                    }
                    check(captured->failure.empty(),captured->failure.c_str());
                };
                capture({ReleaseUpdateStep::checking_release}, "preparing.png");
                capture({ReleaseUpdateStep::downloading, 12 * 1024 * 1024, 24 * 1024 * 1024}, "download.png");
                // Several stages can finish between GUI timer ticks.
                for (const auto step : {ReleaseUpdateStep::verifying_archive, ReleaseUpdateStep::extracting, ReleaseUpdateStep::verifying_kit}) progress({step});
                progress({ReleaseUpdateStep::qualifying_projects, 3, 3});
                capture({ReleaseUpdateStep::staging, 5, 10}, "staging.png");
                progress({ReleaseUpdateStep::stopping_service}); progress({ReleaseUpdateStep::switching_kit});
                capture({ReleaseUpdateStep::starting_service}, "starting.png");
                capture({ReleaseUpdateStep::checking_projects, 2, 3}, "projects.png");
                capture({ReleaseUpdateStep::rolling_back}, "rollback.png");
                capture({ReleaseUpdateStep::confirming}, "confirming.png");
                capture({ReleaseUpdateStep::failed}, "failed.png");
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
