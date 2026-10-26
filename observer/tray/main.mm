#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <ImageIO/ImageIO.h>
#import <os/log.h>
#include <climits>
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <ctime>
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"
#include "monitor_view.h"
#include "../ui/update_window_macos.h"

namespace {
// Clang recognizes this adapter as returning text from the shared EN/RU catalog.
__attribute__((annotate("returns_localized_nsstring")))
NSString* ui_text(const char* message) {
    NSString* translated = [NSString stringWithUTF8String:shin::ui::tr(message)];
    if (translated) return translated;
    NSString* fallback = [NSString stringWithUTF8String:message];
    return fallback ? fallback : @"Shin";
}
#include "native_language.h"

NSString* storage_bytes(std::uint64_t bytes) {
    if (!bytes) return ui_text("0 B");
    if (bytes < 1024) return [NSString stringWithFormat:ui_text("%llu B"), static_cast<unsigned long long>(bytes)];
    const bool mib = bytes >= 1048576;
    char number[64];
    std::snprintf(number, sizeof(number), "%.1f", static_cast<double>(bytes) / (mib ? 1048576 : 1024));
    return [NSString stringWithFormat:@"%@ %@", [NSString stringWithUTF8String:shin::ui::decimal(number).c_str()], mib ? ui_text("MiB") : ui_text("KiB")];
}

std::string cli_error_code(const Json& value) {
    for (const auto* key : {"error_code", "code", "reason_code"})
        if (value.contains(key) && value.at(key).is_string()) return value.at(key).get<std::string>();
    if (value.contains("error")) {
        const auto& error = value.at("error");
        if (error.is_string()) return error.get<std::string>();
        if (error.is_object() && error.contains("code") && error.at("code").is_string()) return error.at("code").get<std::string>();
    }
    return {};
}
std::uint64_t json_uint(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return 0;
    const auto& value = object.at(key);
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number > 0 ? static_cast<std::uint64_t>(number) : 0;
    }
    return 0;
}
NSString* friendly_cleanup_error(const std::string& code) {
    if (code == "storage-plan-changed" || code == "storage-clean-revision-changed" || code == "storage-revision-conflict")
        return ui_text("Caches changed after the preview. Refresh the inventory before cleaning again.");
    if (code == "storage-project-busy" || code == "storage-clean-project-active")
        return ui_text("The project was reconnected or is busy. Its search caches are preserved.");
    if (code == "source-cache-busy") return ui_text("The search cache is in use. Shin preserved it.");
    if (code == "storage-cache-unverified" || code == "storage-clean-cache-unverified" || code == "storage-cache-identity-changed")
        return ui_text("The Shin search cache failed its integrity or ownership check.");
    if (code == "storage-binding-changed") return ui_text("The project binding changed after the preview. Refresh the inventory before cleaning again.");
    if (code == "unsafe-project-storage") return ui_text("The folder or project location failed its safety check.");
    if (code == "storage-quota-exceeded") return ui_text("Cleanup stopped because of a storage limit.");
    if (code == "storage-free-space-low") return ui_text("Cleanup stopped to preserve the free-space reserve.");
    if (code == "storage-inventory-invalid") return ui_text("The cache inventory failed validation; unverified data was preserved.");
    if (code == "storage-inventory-limit") return ui_text("There are too many caches for one safe operation.");
    if (code == "storage-unavailable") return ui_text("Shin storage could not be checked right now.");
    return ui_text("Shin stopped cleanup. Some search caches may remain.");
}
NSString* friendly_project_storage_error(const std::string& code) {
    if (code == "storage-quota-exceeded")
        return ui_text("Shin reached its service storage budget. Open Shin → Storage to review caches outside projects.");
    if (code == "storage-free-space-low")
        return ui_text("There is not enough free disk space for Shin. Open Shin → Storage to review the reserve and caches outside projects.");
    return ui_text("The folder could not be connected. Check that it is an accessible project folder and try again.");
}
NSString* last_cleanup_status(const Json& status) {
    std::string error;
    if (status.contains("last_cleanup_error")) {
        const auto& value = status.at("last_cleanup_error");
        if (value.is_string()) error = value.get<std::string>();
        else if (value.is_object()) {
            error = cli_error_code(value);
            if (error.empty()) error = "storage-unavailable";
        }
    }
    const auto at_ms = json_uint(status, "last_cleanup_at_ms");
    const auto bytes = json_uint(status, "last_cleanup_bytes");
    NSString* when = @"";
    if (at_ms && at_ms <= static_cast<std::uint64_t>(LLONG_MAX)) {
        NSDateFormatter* formatter = [[NSDateFormatter alloc] init];
        formatter.locale = [NSLocale localeWithLocaleIdentifier:shin::ui::language() == shin::ui::Language::ru ? @"ru_RU" : @"en_US"];
        formatter.dateStyle = NSDateFormatterShortStyle; formatter.timeStyle = NSDateFormatterShortStyle;
        when = [NSString stringWithFormat:@" · %@", [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:static_cast<double>(at_ms) / 1000.0]]];
    }
    if (!error.empty()) {
        if (bytes) return [NSString stringWithFormat:ui_text("Last cleanup: partially completed, freed %@; %@%@"), storage_bytes(bytes), friendly_cleanup_error(error), when];
        return [NSString stringWithFormat:ui_text("Last cleanup did not complete — %@%@"), friendly_cleanup_error(error), when];
    }
    if (!at_ms && !bytes) return ui_text("Last cleanup: not run yet");
    if (!bytes) return [NSString stringWithFormat:ui_text("Last cleanup: no caches to remove%@"), when];
    return [NSString stringWithFormat:ui_text("Last cleanup: freed %@ of Shin search caches outside projects%@"), storage_bytes(bytes), when];
}
NSString* cleanup_result_message(const Json& result) {
    const auto bytes = json_uint(result, "reclaimed_bytes");
    const auto code = cli_error_code(result);
    if (result.value("ok", false))
        return [NSString stringWithFormat:ui_text("Freed %@ of Shin search caches outside projects. Sources, indexes, history and settings were preserved."), storage_bytes(bytes)];
    if (bytes)
        return [NSString stringWithFormat:ui_text("Cleanup partially completed: freed %@. Some caches remain. %@"), storage_bytes(bytes), friendly_cleanup_error(code)];
    return [NSString stringWithFormat:ui_text("Cleanup did not complete. %@"), friendly_cleanup_error(code)];
}
void release_project_operation() { project_management_busy.store(false); }
std::string platform_system_language() {
    for (NSString* preferred in NSLocale.preferredLanguages)
        if (shin::ui::supported_language(preferred.UTF8String)) return preferred.UTF8String;
    return "en";
}
void platform_save_language(int preference) {
#if defined(SHIN_TRAY_LAYOUT_TEST)
    (void)preference;
#else
    [[NSUserDefaults standardUserDefaults] setInteger:preference forKey:@"ShinLanguage"];
#endif
    // Apply native labels after this ImGui frame, on the same UI thread.
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp sendAction:@selector(refreshLanguage:) to:NSApp.delegate from:nil]; });
}
[[maybe_unused]] void initialize_language() {
    apply_language_preference(static_cast<int>([[NSUserDefaults standardUserDefaults] integerForKey:@"ShinLanguage"]));
}
void platform_save_view(int view) {
#if defined(SHIN_TRAY_LAYOUT_TEST)
    (void)view;
#else
    [[NSUserDefaults standardUserDefaults] setInteger:view forKey:@"ShinView"];
#endif
}
std::string platform_font() { return "/System/Library/Fonts/SFNS.ttf"; }
bool platform_reduce_motion() { return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion; }
std::string platform_version() {
    NSString* value = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return value.UTF8String ? value.UTF8String : "unknown";
}
bool platform_copy(const std::string& value) {
    NSString* text = [NSString stringWithUTF8String:value.c_str()];
    if (!text) return false;
    [NSPasteboard.generalPasteboard clearContents];
    return [NSPasteboard.generalPasteboard setString:text forType:NSPasteboardTypeString];
}
void platform_reveal(const std::string& value) {
    NSString* path = [NSString stringWithUTF8String:value.c_str()];
    if (path) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path isDirectory:YES]]];
}
void platform_action(int code) {
    switch (code) {
    case 39: [NSApp sendAction:@selector(showDiagnosticReport:) to:NSApp.delegate from:nil]; break;
    case 40: [NSApp sendAction:@selector(chooseUsageProject:) to:NSApp.delegate from:nil]; break;
    case 43: [NSApp sendAction:@selector(chooseUsageFile:) to:NSApp.delegate from:nil]; break;
    case 44: [NSApp sendAction:@selector(choosePeriod:) to:NSApp.delegate from:nil]; break;
    case 62: [NSApp sendAction:@selector(chooseTimingTraffic:) to:NSApp.delegate from:nil]; break;
    case 66: [NSApp sendAction:@selector(chooseTimingScope:) to:NSApp.delegate from:nil]; break;
    case 86:
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp sendAction:@selector(showUpdateOffer:) to:NSApp.delegate from:nil]; });
        break;
    case 85:
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp sendAction:@selector(showSetup:) to:NSApp.delegate from:nil]; });
        break;
    case 80: case 83: case 84: {
        SEL action = code == 80 ? @selector(addProjectFolder:) : code == 83 ? @selector(openStorageWindow:) : @selector(showProjectManagementMenu:);
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp sendAction:action to:NSApp.delegate from:nil]; });
        break;
    }
    default: break;
    }
}
std::array<id<MTLTexture>, static_cast<std::size_t>(Icon::count)> icon_textures;
id<MTLTexture> brand_texture;

void load_icons(id<MTLDevice> device) {
    const std::array names{@"checkmark.circle", @"arrow.triangle.2.circlepath", @"clock",
        @"wrench.and.screwdriver", @"exclamationmark.triangle", @"moon.zzz",
        @"pause.circle", @"wifi.slash", @"questionmark.circle", @"clock.arrow.circlepath",
        @"arrow.left.arrow.right", @"memorychip", @"doc.on.doc", @"folder", @"chevron.left", @"doc.text.magnifyingglass", @"ellipsis"};
    constexpr std::size_t pixels = 48;
    for (std::size_t i = 0; i < names.size(); ++i) {
        NSImage* image = [NSImage imageWithSystemSymbolName:names[i] accessibilityDescription:nil];
        if (!image) image = [NSImage imageWithSystemSymbolName:@"questionmark.circle" accessibilityDescription:nil];
        image = [image imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:20 weight:NSFontWeightMedium]];
        NSRect proposed = i == static_cast<std::size_t>(Icon::more)
            ? NSMakeRect(0, 0, image.size.width, image.size.height) : NSMakeRect(0, 0, pixels, pixels);
        CGImageRef source = [image CGImageForProposedRect:&proposed context:nil hints:nil];
        if (!source) continue;
        std::array<unsigned char, pixels * pixels * 4> bytes{};
        CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(bytes.data(), pixels, pixels, 8, pixels * 4,
            color_space, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(color_space);
        if (!context) continue;
        const auto width = static_cast<double>(CGImageGetWidth(source)), height = static_cast<double>(CGImageGetHeight(source));
        const double scale = 44 / std::max(width, height);
        CGContextDrawImage(context, CGRectMake((pixels - width * scale) / 2, (pixels - height * scale) / 2,
            width * scale, height * scale), source);
        CGContextRelease(context);
        for (std::size_t p = 0; p < bytes.size(); p += 4) bytes[p] = bytes[p + 1] = bytes[p + 2] = 255;
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:pixels height:pixels mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        auto texture = [device newTextureWithDescriptor:descriptor];
        [texture replaceRegion:MTLRegionMake2D(0, 0, pixels, pixels) mipmapLevel:0 withBytes:bytes.data() bytesPerRow:pixels * 4];
        icon_textures[i] = texture;
    }
    NSURL* url = [NSBundle.mainBundle URLForResource:@"shin-logo" withExtension:@"png"];
#if defined(SHIN_TRAY_LAYOUT_TEST)
    url = [NSURL fileURLWithPath:@SHIN_TRAY_TEST_LOGO];
#endif
    CGImageSourceRef source = url ? CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr) : nullptr;
    if (!source) return;
    NSDictionary* options = @{(__bridge NSString*)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
        (__bridge NSString*)kCGImageSourceThumbnailMaxPixelSize:@64,
        (__bridge NSString*)kCGImageSourceShouldCacheImmediately:@YES};
    CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
    CFRelease(source);
    if (!image) return;
    std::array<unsigned char, 64 * 64 * 4> bytes{};
    auto space = CGColorSpaceCreateDeviceRGB();
    auto bitmap = CGBitmapContextCreate(bytes.data(), 64, 64, 8, 64 * 4, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (bitmap) {
        CGContextDrawImage(bitmap, CGRectMake(0, 0, 64, 64), image);
        CGContextRelease(bitmap);
        for (std::size_t p = 0; p < bytes.size(); p += 4) bytes[p] = bytes[p + 1] = bytes[p + 2] = 255;
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:64 height:64 mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        brand_texture = [device newTextureWithDescriptor:descriptor];
        [brand_texture replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:bytes.data() bytesPerRow:64 * 4];
    }
    CGImageRelease(image);
}

void draw_brand(ImVec2 origin, float size) {
    if (brand_texture) ImGui::GetWindowDrawList()->AddImage(
        ImTextureRef(reinterpret_cast<ImTextureID>(brand_texture)), origin, {origin.x + size, origin.y + size},
        {0, 0}, {1, 1}, ImGui::GetColorU32(sand));
}

void draw_icon(Icon icon, ImVec2 origin, float size, ImVec4 color) {
    const auto texture = icon_textures[static_cast<std::size_t>(icon)];
    if (!texture) return;
    const ImTextureID id = reinterpret_cast<ImTextureID>(texture);
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(id), origin, {origin.x + size, origin.y + size},
        {0, 0}, {1, 1}, ImGui::GetColorU32(color));
}

NSImage* tray_logo() {
    NSURL* url = [NSBundle.mainBundle URLForResource:@"ShinTrayTemplate" withExtension:@"png"];
    CGImageSourceRef source = url ? CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr) : nullptr;
    if (!source) return nil;
    // Decode only a tiny thumbnail, never the full brand artwork into tray RAM.
    NSDictionary* options = @{(__bridge NSString*)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
        (__bridge NSString*)kCGImageSourceThumbnailMaxPixelSize:@64,
        (__bridge NSString*)kCGImageSourceShouldCacheImmediately:@YES};
    CGImageRef thumbnail = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
    CFRelease(source);
    if (!thumbnail) return nil;
    NSImage* image = [[NSImage alloc] initWithCGImage:thumbnail size:NSMakeSize(20, 20)];
    CGImageRelease(thumbnail);
    [image setTemplate:YES];
    image.accessibilityDescription = NSLocalizedString(@"Shin Monitor", nil);
    return image;
}

NSString* diagnostic_text_content(std::string_view value) {
    NSString* text = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    return text ? text : ui_text("Cannot read the report text: invalid UTF-8 encoding.");
}

NSString* update_failure_text(const Json& result, NSString* failure) {
    if (failure.length) return failure;
    if (result.is_object()) {
        const auto error = result.find("error");
        if (error != result.end() && error->is_object()) {
            if (error->value("code", Json()) == "update-offer-changed")
                return ui_text("The release offer changed. Check for updates again before installing.");
            const auto message = error->find("message");
            if (message != error->end() && message->is_string() && !message->get_ref<const std::string&>().empty())
                return diagnostic_text_content(message->get_ref<const std::string&>());
        }
    }
    return ui_text("The release or installation could not be confirmed. Check again before retrying; the current kit is preserved.");
}
} // namespace

enum class UpdateScreen { idle, checking, offer, current, checkFailed, starting, startFailed,
                          resultLoading, resultReady, acknowledging, acknowledgeFailed };

@interface ShinAction : NSAccessibilityElement
@property(nonatomic, copy) void (^activate)(void);
@end
@implementation ShinAction
- (BOOL)accessibilityPerformPress { if (self.activate) self.activate(); return YES; }
@end

@interface ShinView : NSViewController<MTKViewDelegate>
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) NSMutableDictionary<NSString*, ShinAction*>* actionElements;
@end

static std::unique_ptr<Monitor> monitor;

@implementation ShinView
- (void)cancelOperation:(id)sender {
    (void)sender;
    [NSApp sendAction:@selector(closePanel:) to:NSApp.delegate from:nil];
}
- (void)loadView {
    self.device = MTLCreateSystemDefaultDevice();
    if (!self.device) { [NSApp terminate:nil]; return; }
    load_icons(self.device);
    self.queue = [self.device newCommandQueue];
    MTKView* view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 432, 620) device:self.device];
    view.delegate = self;
    view.preferredFramesPerSecond = 20;
    view.paused = YES;
    self.view = view;
    self.actionElements = [NSMutableDictionary dictionary];
    view.accessibilityElement = YES;
    view.accessibilityRole = NSAccessibilityGroupRole;
    configure_imgui();
    ImGui_ImplMetal_Init(self.device);
    ImGui_ImplOSX_Init(view);
}
- (void)drawInMTKView:(MTKView*)view {
    if (!view.window.isVisible || !monitor) return;
    const auto snapshot = monitor->snapshot();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(view.bounds.size.width), static_cast<float>(view.bounds.size.height)};
    const float scale = static_cast<float>(view.window.screen.backingScaleFactor);
    io.DisplayFramebufferScale = {scale, scale};
    auto pass = view.currentRenderPassDescriptor;
    auto drawable = view.currentDrawable;
    if (!pass || !drawable) return;
    auto command = [self.queue commandBuffer];
    ImGui_ImplMetal_NewFrame(pass);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();
    try { render(snapshot); } catch (...) { ImGui::EndFrame(); return; }
    const NSInteger frame_rate = monitor_motion.active ? 60 : 20;
    if (view.preferredFramesPerSecond != frame_rate) view.preferredFramesPerSecond = frame_rate;
    NSMutableArray* accessible = [NSMutableArray array];
    NSMutableSet* activeKeys = [NSMutableSet set];
    for (const auto& action : ui_actions) {
        NSString* key = [NSString stringWithUTF8String:(action.project + ":" + std::to_string(action.chart)).c_str()];
        if (!key) continue;
        [activeKeys addObject:key];
        ShinAction* element = self.actionElements[key];
        if (!element) {
            element = [[ShinAction alloc] init];
            self.actionElements[key] = element;
        }
        element.accessibilityParent = view;
        element.accessibilityRole = action.informational ? NSAccessibilityStaticTextRole : NSAccessibilityButtonRole;
        element.accessibilityEnabled = action.enabled;
        element.accessibilityLabel = [NSString stringWithUTF8String:action.label.c_str()];
        const NSRect local = NSMakeRect(action.origin.x, view.bounds.size.height - action.origin.y - action.size.y,
                                       action.size.x, action.size.y);
        element.accessibilityFrameInParentSpace = local;
        const auto project = action.project;
        const int chart = action.chart;
        const bool enabled = action.enabled;
        const bool informational = action.informational;
        element.activate = ^{
            if (!enabled || informational) return;
            if (chart >= 0) {
                if (project.empty() || project == selected) activate(chart);
            } else select_project(project);
        };
        [accessible addObject:element];
    }
    for (NSString* key in self.actionElements.allKeys)
        if (![activeKeys containsObject:key]) [self.actionElements removeObjectForKey:key];
    view.accessibilityChildren = accessible;
    ImGui::Render();
    pass.colorAttachments[0].clearColor = MTLClearColorMake(background.x, background.y, background.z, 1);
    auto encoder = [command renderCommandEncoderWithDescriptor:pass];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command, encoder);
    [encoder endEncoding];
    [command presentDrawable:drawable];
    [command commit];
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }
@end

@interface ShinApp : NSObject<NSApplicationDelegate, NSPopoverDelegate, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate> {
    Json _storageStatus;
    Json _updateOffer;
    Json _reviewedUpdateOffer;
    Json _updateResult;
}
@property(nonatomic, strong) NSStatusItem* item;
@property(nonatomic, strong) NSPopover* popover;
@property(nonatomic, strong) ShinView* content;
@property(nonatomic, strong) NSWindow* diagnosticWindow;
@property(nonatomic, copy) NSString* diagnosticPayload;
@property(nonatomic, strong) NSTimer* timer;
@property(nonatomic, strong) id outsideMonitor;
@property(nonatomic, strong) id escapeMonitor;
@property(nonatomic, copy) NSString* statusGeometry;
@property(nonatomic, copy) NSString* shinHome;
@property(nonatomic, strong) NSWindow* storageWindow;
@property(nonatomic, strong) NSTextField* storageText;
@property(nonatomic, strong) NSTableView* storageTable;
@property(nonatomic, copy) NSArray<NSDictionary<NSString*,NSString*>*>* storageRows;
@property(nonatomic) NSUInteger storageRequestEpoch;
@property(nonatomic, strong) NSButton* storageAutoClean;
@property(nonatomic, strong) NSButton* storageClean;
@property(nonatomic) BOOL storageAutoCleanEnabled;
@property(nonatomic) NSUInteger statusSamples;
@property(nonatomic, strong) NSWindow* setupWindow;
@property(nonatomic, strong) NSTextField* setupProject;
@property(nonatomic, strong) NSTextField* setupStatus;
@property(nonatomic, strong) NSButton* setupCheck;
@property(nonatomic, strong) NSButton* setupChoose;
@property(nonatomic, strong) NSButton* setupLogin;
@property(nonatomic, copy) NSString* setupRoot;
@property(nonatomic) NSUInteger setupEpoch;
@property(nonatomic, strong) NSStackView* setupAgents;
@property(nonatomic, strong) NSButton* setupConnect;
@property(nonatomic, strong) NSMutableArray<NSMutableDictionary*>* setupTargets;
@property(nonatomic, strong) NSMutableArray<NSButton*>* setupTargetButtons;
@property(nonatomic, strong) NSMutableArray<NSButton*>* setupFileButtons;
@property(nonatomic, strong) NSMutableArray<NSMutableDictionary*>* setupPlans;
@property(nonatomic, strong) NSMutableString* setupReport;
@property(nonatomic, copy) NSString* setupConnectRoot;
@property(nonatomic) BOOL setupConnecting;
@property(nonatomic) BOOL updateChecking;
@property(nonatomic) NSUInteger updateWindowEpoch;
@property(nonatomic) NSUInteger updateCheckEpoch;
@property(nonatomic) UpdateScreen updateScreen;
@property(nonatomic) BOOL updateActionBusy;
@property(nonatomic, copy) NSString* updateFailure;
@property(nonatomic, strong) NSWindow* updateWindow;
@property(nonatomic, strong) NSTextField* updateTitle;
@property(nonatomic, strong) NSTextField* updateDetail;
@property(nonatomic, strong) NSProgressIndicator* updateSpinner;
@property(nonatomic, strong) NSButton* updateCheckButton;
@property(nonatomic, strong) NSButton* updateInstallButton;
@property(nonatomic, strong) NSButton* updateNotesButton;
@property(nonatomic, strong) NSButton* updateCopyButton;
@property(nonatomic, strong) NSButton* updateAcknowledgeButton;
@property(nonatomic, strong) NSDate* lastUpdateCheck;
@property(nonatomic, strong) NSDate* nextUpdateCheck;
- (void)checkForUpdates:(id)sender;
- (void)showUpdateOffer:(id)sender;
- (void)presentUpdateOffer;
- (void)beginUpdateCheck:(BOOL)manual;
- (void)toggleAutomaticUpdates:(id)sender;
- (void)showUpdateResult;
- (void)showLastUpdateResult:(id)sender;
- (void)ensureUpdateWindow;
- (void)showUpdateWindow;
- (void)renderUpdateWindow;
- (void)installReviewedUpdate:(id)sender;
- (void)openUpdateReleaseNotes:(id)sender;
- (void)openUpdateReleaseURL:(NSURL*)url;
- (void)copyUpdateResult:(id)sender;
- (void)acknowledgeUpdateResult:(id)sender;
- (void)setUpdatePending:(BOOL)pending;
- (void)refreshLanguage:(id)sender;
- (void)toggle:(id)sender;
- (void)showDiagnosticReport:(id)sender;
- (void)copyDiagnosticReport:(id)sender;
- (void)toggleCompact:(id)sender;
- (void)selectView:(NSMenuItem*)sender;
- (void)chooseUsageFile:(id)sender;
- (void)chooseUsageProject:(id)sender;
- (void)selectUsageProject:(NSMenuItem*)sender;
- (void)choosePeriod:(id)sender;
- (void)chooseTimingTraffic:(id)sender;
- (void)chooseTimingScope:(id)sender;
- (void)showSetup:(id)sender;
- (void)showAgentSetup:(id)sender;
- (void)beginSetupProject:(id)sender;
- (void)refreshSetup:(id)sender;
- (void)selectSetupTarget:(id)sender;
- (void)chooseSetupConfig:(NSButton*)sender;
- (void)connectSetupAgents:(id)sender;
- (void)continueSetupConnection:(NSUInteger)index applying:(BOOL)applying;
- (void)checkSetup:(id)sender;
- (void)enableSetupLogin:(id)sender;
- (void)addProjectFolder:(id)sender;
- (void)disconnectProject:(id)sender;
- (void)disconnectAndCleanProject:(id)sender;
- (void)showProjectManagementMenu:(id)sender;
- (void)beginProjectDisconnect:(id)sender;
- (void)beginProjectDisconnectAndClean:(id)sender;
- (void)openStorageWindow:(id)sender;
- (void)ensureStorageWindow;
- (void)layoutStorageColumns;
- (void)applyStorageStatus:(const Json&)status;
- (void)refreshStorage:(id)sender;
- (void)toggleStorageAutoClean:(id)sender;
- (void)cleanStorage:(id)sender;
- (void)runCLI:(NSString*)command arguments:(NSArray<NSString*>*)arguments completion:(void (^)(Json, NSString*))completion;
- (void)closePanel:(id)sender;
- (void)updatePanelSize;
- (void)activatedApplication:(NSNotification*)notification;
@end

@implementation ShinApp
#include "setup_window.h"
#include "updates_window.h"
- (void)showProjectManagementMenu:(id)sender {
    (void)sender;
    if (diagnostic_root.empty()) return;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:ui_text("Project actions")];
    NSMenuItem* connect = [menu addItemWithTitle:ui_text("Connect coding agent…") action:@selector(showAgentSetup:) keyEquivalent:@""];
    connect.target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* detach = [menu addItemWithTitle:ui_text("Disconnect") action:@selector(beginProjectDisconnect:) keyEquivalent:@""];
    detach.target = self;
    NSMenuItem* cleanup = [menu addItemWithTitle:ui_text("Disconnect and clean…") action:@selector(beginProjectDisconnectAndClean:) keyEquivalent:@""];
    cleanup.target = self;
    if (project_management_busy.load() || !monitor || !monitor->snapshot().connected) {
        detach.enabled = NO; cleanup.enabled = NO;
    }
    const auto action = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& item) { return item.chart == 84; });
    const NSPoint point = action == ui_actions.end() ? NSMakePoint(16, 40) :
        NSMakePoint(action->origin.x, self.content.view.bounds.size.height - action->origin.y);
    [menu popUpMenuPositioningItem:nil atLocation:point inView:self.content.view];
}
- (void)beginProjectDisconnect:(id)sender {
    (void)sender;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    [self disconnectProject:nil];
}
- (void)beginProjectDisconnectAndClean:(id)sender {
    (void)sender;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    [self disconnectAndCleanProject:nil];
}

- (void)runCLI:(NSString*)command arguments:(NSArray<NSString*>*)arguments completion:(void (^)(Json, NSString*))completion {
    if (!self.shinHome.length) { completion(Json::object(), ui_text("Cannot locate the Shin folder.")); return; }
    const auto executable = current_executable_path();
    const auto canonical = fs::canonical(executable);
    const auto prefix = canonical.parent_path().filename() == "MacOS"
        ? canonical.parent_path().parent_path().parent_path().parent_path()
        : canonical.parent_path().parent_path();
    const auto cli_path = prefix / "bin" / "shin-ce";
    NSString* cli = [NSString stringWithUTF8String:cli_path.c_str()];
    if (!cli || ![NSFileManager.defaultManager isExecutableFileAtPath:cli]) {
        completion(Json::object(), ui_text("shin-ce is missing from the installed kit."));
        return;
    }
    NSArray<NSString*>* argv = [@[command, @"--home", self.shinHome] arrayByAddingObjectsFromArray:arguments];
    const auto timeout = [command isEqualToString:@"update-install"] ? std::chrono::seconds(360) :
        [command isEqualToString:@"update-check"] ? std::chrono::seconds(20) : [command isEqualToString:@"project-add"] ? std::chrono::seconds(120) :
        [command isEqualToString:@"storage-clean"] ? std::chrono::seconds(90) :
        [command isEqualToString:@"detach"] ? std::chrono::seconds(90) : std::chrono::seconds(15);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            Json result = Json::object();
            NSString* failure = nil;
            @try {
                NSTask* task = [[NSTask alloc] init];
                task.executableURL = [NSURL fileURLWithPath:cli];
                task.arguments = argv;
                NSPipe* pipe = [NSPipe pipe];
                task.standardOutput = pipe;
                task.standardError = pipe;
                NSError* launchError = nil;
                if (![task launchAndReturnError:&launchError]) {
                    failure = launchError.localizedDescription.length ? launchError.localizedDescription : ui_text("Cannot start shin-ce.");
                } else {
                    NSMutableData* bytes = [NSMutableData data];
                    NSFileHandle* reader = pipe.fileHandleForReading;
                    BOOL oversized = NO;
                    BOOL timedOut = NO, pipeClosed = NO, sentKill = NO;
                    const auto deadline = std::chrono::steady_clock::now() + timeout;
                    const auto stop_at = deadline - std::chrono::seconds(1) - std::chrono::milliseconds(100);
                    auto terminate_at = std::chrono::steady_clock::time_point{};
                    const int descriptor = reader.fileDescriptor;
                    std::array<unsigned char, 16 * 1024> buffer{};
                    while (!pipeClosed || task.isRunning) {
                        const auto now = std::chrono::steady_clock::now();
                        if (!timedOut && !oversized && now >= stop_at && (!pipeClosed || task.isRunning)) {
                            timedOut = YES; terminate_at = now;
                            if (task.isRunning) [task terminate];
                            else { [reader closeFile]; pipeClosed = YES; }
                        } else if (task.isRunning && (timedOut || oversized) && !sentKill &&
                            now - terminate_at >= std::chrono::seconds(1)) {
                            // Kill only the NSTask child PID; never find or signal a process by name.
                            (void)::kill(static_cast<pid_t>(task.processIdentifier), SIGKILL);
                            sentKill = YES;
                        } else if (!task.isRunning && !pipeClosed && (timedOut || oversized) &&
                            now - terminate_at >= std::chrono::seconds(1)) {
                            [reader closeFile]; pipeClosed = YES;
                        }
                        pollfd poll_descriptor{descriptor, static_cast<short>(pipeClosed ? 0 : POLLIN | POLLHUP), 0};
                        const int ready = ::poll(pipeClosed ? nullptr : &poll_descriptor, pipeClosed ? 0 : 1, 100);
                        if (ready < 0 && errno == EINTR) continue;
                        if (ready < 0) {
                            failure = ui_text("Could not wait for the shin-ce response.");
                            if (!timedOut) { timedOut = YES; terminate_at = std::chrono::steady_clock::now(); }
                            if (task.isRunning) [task terminate];
                            continue;
                        }
                        if (!pipeClosed && ready > 0 && (poll_descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
                            const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
                            if (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN)) pipeClosed = YES;
                            else if (count > 0) {
                                const auto length = static_cast<NSUInteger>(count);
                                if (bytes.length + length > 1024 * 1024) {
                                    oversized = YES; terminate_at = std::chrono::steady_clock::now();
                                    if (task.isRunning) [task terminate];
                                } else if (!oversized) [bytes appendBytes:buffer.data() length:length];
                                if ([command isEqualToString:@"update-install"] && !oversized) {
                                    const auto ack=Json::parse(std::string((const char*)bytes.bytes,bytes.length),nullptr,false);
                                    if(ack.is_object() && ack.value("accepted",false) && ack.value("pid",0)==task.processIdentifier) {
                                        [reader closeFile];
                                        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
                                        return; // Native helper waits for this Monitor PID, records the outcome and reopens it.
                                    }
                                }
                            }
                        }
                    }
                    [task waitUntilExit];
                    if (timedOut) failure = [NSString stringWithFormat:ui_text("The shin-ce command exceeded its time limit (%lld seconds) and was stopped."), static_cast<long long>(timeout.count())];
                    else if (oversized) failure = ui_text("The shin-ce response exceeded its size limit.");
                    else {
                        NSString* output = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
                        if (output.length) {
                            const char* text = output.UTF8String;
                            if (text) result = Json::parse(text, nullptr, false);
                        }
                        if (result.is_discarded() || !result.is_object()) {
                            result = Json::object();
                            failure = task.terminationStatus == 0 ? ui_text("shin-ce returned an invalid response.") :
                                [NSString stringWithFormat:ui_text("shin-ce exited with code %d."), task.terminationStatus];
                        }
                    }
                }
            } @catch (NSException* exception) {
                failure = exception.reason.length ? exception.reason : ui_text("Could not run the shin-ce command.");
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                if (completion) completion(result, failure);
            });
        }
    });
}

- (void)addProjectFolder:(id)sender {
    (void)sender;
    [self closePanel:nil];
    NSOpenPanel* picker = [NSOpenPanel openPanel];
    live_text(picker, @"title", "Add a project folder"); picker.prompt = ui_text("Connect");
    picker.canChooseDirectories = YES; picker.canChooseFiles = NO; picker.canCreateDirectories = NO;
    const auto response = [picker runModal];
    NSString* root = picker.URL.path;
    if (response != NSModalResponseOK || !root) { release_project_operation(); return; }
    self.setupChoose.enabled = NO;
    live_text(self.setupStatus, @"stringValue", "Preparing project… This can take a moment on the first import.");
    [self runCLI:@"project-add" arguments:@[@"--root", root] completion:^(Json result, NSString* failure) {
        const bool serviceFailed = result.contains("service") && result.at("service").is_object() &&
            result.at("service").contains("ok") && result.at("service").at("ok").is_boolean() && !result.at("service").at("ok").get<bool>();
        const bool added = !failure && result.value("ok", false) && !serviceFailed;
        const Json& errorResult = serviceFailed ? result.at("service") : result;
        NSString* message = failure;
        if (!message && added) message = ui_text("The project is connected. Model statistics are collected automatically in the background. Choose agents to use Shin in chats.");
        if (!message) {
            const auto code = cli_error_code(errorResult);
            if (code == "existing-project-adoption")
                message = ui_text("This folder already has Shin settings. Connection stopped; settings, caches and history were preserved.");
            else if (code == "unsafe-project-storage")
                message = ui_text("The folder or its location failed a safety check. Choose a regular project folder owned by your user.");
            else message = friendly_project_storage_error(code);
        }
        release_project_operation();
        self.setupChoose.enabled = YES;
        if (added) {
            self.setupRoot = root;
            [self showSetup:nil];
            live_text(self.setupStatus, @"stringValue", "Project added. Select the agents that should have access.");
            return;
        }
        self.setupStatus.stringValue = message;
        NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = added ? ui_text("Project connected") : ui_text("Project not connected");
        alert.informativeText = message; [alert runModal]; release_project_operation();
    }];
}

- (void)disconnectProject:(id)sender {
    (void)sender;
    if (diagnostic_root.empty()) { release_project_operation(); return; }
    NSString* root = [NSString stringWithUTF8String:diagnostic_root.c_str()];
    if (!root) { release_project_operation(); return; }
    [self closePanel:nil];
    [self runCLI:@"detach" arguments:@[@"--root", root] completion:^(Json result, NSString* failure) {
        const bool ok = !failure && result.value("ok", false);
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = ok ? ui_text("Project disconnected") : ui_text("Could not disconnect the project");
        alert.informativeText = failure.length ? failure : (ok ? ui_text("The project was removed from observation. Sources, indexes, history and Shin settings were preserved.") :
            ui_text("Shin did not confirm disconnection. Project data was not cleaned."));
        [alert runModal]; release_project_operation();
    }];
}

- (void)disconnectAndCleanProject:(id)sender {
    (void)sender;
    if (diagnostic_root.empty()) { release_project_operation(); return; }
    NSString* root = [NSString stringWithUTF8String:diagnostic_root.c_str()];
    if (!root) { release_project_operation(); return; }
    [self closePanel:nil];
    [self runCLI:@"detach" arguments:@[@"--root", root] completion:^(Json detached, NSString* failure) {
        if (failure || !detached.value("ok", false)) {
            NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = ui_text("Project not disconnected");
            alert.informativeText = failure.length ? failure : ui_text("Shin did not confirm disconnection. Cleanup was not run."); [alert runModal]; release_project_operation(); return;
        }
        [self runCLI:@"storage-status" arguments:@[@"--root", root] completion:^(Json status, NSString* statusFailure) {
            if (statusFailure || !status.value("ok", false)) {
                NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = ui_text("Project disconnected; caches preserved");
                alert.informativeText = statusFailure.length ? statusFailure : ui_text("Could not obtain a safe cleanup preview. Caches were preserved."); [alert runModal]; release_project_operation(); return;
            }
            const auto bytes = status.value("reclaimable_bytes", 0ULL);
            if (!bytes) {
                NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = ui_text("Project disconnected");
                alert.informativeText = ui_text("The disconnected project has no Shin search caches to clean. Sources, indexes, history and settings were preserved."); [alert runModal]; release_project_operation(); return;
            }
            const auto revision = status.value("revision", std::string{});
            NSString* revisionText = [NSString stringWithUTF8String:revision.c_str()];
            NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = ui_text("Project disconnected. Clean its cache?");
            alert.informativeText = [NSString stringWithFormat:ui_text("Disconnection allows reclaiming up to %@ of Shin search caches outside projects. Sources, indexes, history and Shin settings will be preserved."), storage_bytes(bytes)];
            [alert addButtonWithTitle:ui_text("Clean cache")]; [alert addButtonWithTitle:ui_text("Keep cache")];
            if ([alert runModal] != NSAlertFirstButtonReturn) { release_project_operation(); return; }
            [self runCLI:@"storage-clean" arguments:@[@"--root", root, @"--revision", revisionText ? revisionText : @""] completion:^(Json cleaned, NSString* cleanFailure) {
                NSAlert* resultAlert = [[NSAlert alloc] init];
                const bool cleanedOK = !cleanFailure && cleaned.value("ok", false);
                resultAlert.messageText = cleanedOK ? ui_text("Search cache cleaned") : ui_text("Cleanup stopped");
                resultAlert.informativeText = cleanFailure.length ? cleanFailure : cleanup_result_message(cleaned);
                [resultAlert runModal]; release_project_operation();
            }];
        }];
    }];
}

- (void)refreshLanguage:(id)sender {
    (void)sender;
    refresh_setup_status(self.setupStatus);
    refresh_native_captions();
    [self renderUpdateWindow];
    if (_storageStatus.is_object()) {
        const auto policyState = self.storageAutoClean.state;
        const bool policyEnabled = self.storageAutoClean.enabled, cleanEnabled = self.storageClean.enabled;
        [self applyStorageStatus:_storageStatus];
        self.storageAutoClean.state = policyState;
        self.storageAutoClean.enabled = policyEnabled; self.storageClean.enabled = cleanEnabled;
    }
    [self.setupWindow.contentView layoutSubtreeIfNeeded];
}

- (void)ensureStorageWindow {
    if (self.storageWindow) return;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 580)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    live_text(window, @"title", "Shin storage"); window.releasedWhenClosed = NO; window.delegate = self;
    window.contentMinSize = NSMakeSize(720, 500);
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    NSTextField* text = live_label("Reading storage inventory…");
    text.font = [NSFont systemFontOfSize:13]; text.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,716,200)];
    scroll.translatesAutoresizingMaskIntoConstraints = NO; scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    NSTableView* table = [[NSTableView alloc] initWithFrame:NSMakeRect(0,0,716,200)];
    table.delegate = self; table.dataSource = self; table.rowHeight = 38;
    table.usesAlternatingRowBackgroundColors = YES; table.autoresizingMask = NSViewWidthSizable;
    table.style = NSTableViewStyleFullWidth; table.intercellSpacing = NSMakeSize(6,0);
    table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    NSArray* keys = @[@"project",@"used_bytes",@"index_bytes",@"cache_bytes",@"other_bytes",@"reclaimable_bytes"];
    const char* titles[]{"Project", "Total", "Indexes", "Caches", "Other", "Reclaimable"};
    for (NSUInteger i=0;i<keys.count;++i) {
        NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:keys[i]];
        live_text(column, @"title", titles[i]); column.width = i==0 ? 240 : i==5 ? 90 : 80;
        column.resizingMask = i==0 ? NSTableColumnAutoresizingMask : NSTableColumnNoResizing;
        column.minWidth = i==0 ? 200 : 80;
        column.headerCell.alignment = i==0 ? NSTextAlignmentLeft : NSTextAlignmentRight;
        [table addTableColumn:column];
    }
    scroll.documentView = table;
    NSTextField* note = live_label("File sizes outside projects. Data may change during updates; choose Refresh for a new inventory. Manual builds and diagnostics are excluded. Cleanup preserves sources, indexes and history.");
    note.translatesAutoresizingMaskIntoConstraints = NO; note.font = [NSFont systemFontOfSize:12];
    note.textColor = NSColor.secondaryLabelColor;
    NSButton* policy = live_button("Automatically clean search caches of disconnected projects", self, @selector(toggleStorageAutoClean:), true);
    policy.translatesAutoresizingMaskIntoConstraints = NO;
    NSButton* refresh = live_button("Refresh", self, @selector(refreshStorage:)); refresh.translatesAutoresizingMaskIntoConstraints = NO;
    NSButton* clean = live_button("Clean search caches…", self, @selector(cleanStorage:)); clean.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView* view in @[text,scroll,note,policy,refresh,clean]) [window.contentView addSubview:view];
    [NSLayoutConstraint activateConstraints:@[
        [text.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:22],
        [text.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:22],
        [text.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-22],
        [scroll.topAnchor constraintEqualToAnchor:text.bottomAnchor constant:14],
        [scroll.leadingAnchor constraintEqualToAnchor:text.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:text.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:note.topAnchor constant:-12], [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:110],
        [note.leadingAnchor constraintEqualToAnchor:text.leadingAnchor], [note.trailingAnchor constraintEqualToAnchor:text.trailingAnchor],
        [note.bottomAnchor constraintEqualToAnchor:policy.topAnchor constant:-12],
        [policy.leadingAnchor constraintEqualToAnchor:text.leadingAnchor], [policy.trailingAnchor constraintLessThanOrEqualToAnchor:text.trailingAnchor],
        [policy.bottomAnchor constraintEqualToAnchor:refresh.topAnchor constant:-16],
        [refresh.leadingAnchor constraintEqualToAnchor:text.leadingAnchor], [refresh.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-20],
        [clean.trailingAnchor constraintEqualToAnchor:text.trailingAnchor], [clean.centerYAnchor constraintEqualToAnchor:refresh.centerYAnchor]]];
    self.storageWindow = window; self.storageText = text; self.storageTable = table;
    self.storageAutoClean = policy; self.storageClean = clean;
}
- (void)layoutStorageColumns {
    [self.storageWindow.contentView layoutSubtreeIfNeeded];
    NSScrollView* scroll=self.storageTable.enclosingScrollView;
    [scroll tile];
    const auto available=NSWidth(scroll.contentView.bounds);
    [self.storageTable setFrameSize:NSMakeSize(available,self.storageTable.frame.size.height)];
    if (self.storageTable.tableColumns.count==6) {
        NSTableColumn* first=self.storageTable.tableColumns[0];
        const auto right=NSMaxX([self.storageTable rectOfColumn:5]);
        first.width=std::max(first.minWidth,first.width+available-right-1);
    }
}
- (void)windowDidResize:(NSNotification*)notification {
    if (notification.object==self.storageWindow) [self layoutStorageColumns];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return tableView==self.storageTable ? static_cast<NSInteger>(self.storageRows.count) : 0;
}
- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    if (tableView!=self.storageTable || row<0 || static_cast<NSUInteger>(row)>=self.storageRows.count) return nil;
    NSDictionary<NSString*,NSString*>* values = self.storageRows[static_cast<NSUInteger>(row)];
    NSTableCellView* cell = [tableView makeViewWithIdentifier:column.identifier owner:self];
    const bool project = [column.identifier isEqualToString:@"project"];
    if (!cell) {
        cell = [[NSTableCellView alloc] init]; cell.identifier = column.identifier;
        NSTextField* text = [NSTextField labelWithString:@""]; text.translatesAutoresizingMaskIntoConstraints = NO;
        text.font = project ? [NSFont systemFontOfSize:12] : [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        text.alignment = project ? NSTextAlignmentLeft : NSTextAlignmentRight;
        text.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [cell addSubview:text]; cell.textField = text;
        [NSLayoutConstraint activateConstraints:@[[text.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:5],
            [text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-5]]];
        if (project) {
            NSTextField* path = [NSTextField labelWithString:@""]; path.tag=1;
            path.translatesAutoresizingMaskIntoConstraints = NO; path.font=[NSFont systemFontOfSize:10];
            path.textColor=NSColor.secondaryLabelColor; path.lineBreakMode=NSLineBreakByTruncatingMiddle;
            [cell addSubview:path];
            [NSLayoutConstraint activateConstraints:@[[text.topAnchor constraintEqualToAnchor:cell.topAnchor constant:3],
                [path.topAnchor constraintEqualToAnchor:text.bottomAnchor constant:1],
                [path.leadingAnchor constraintEqualToAnchor:text.leadingAnchor], [path.trailingAnchor constraintEqualToAnchor:text.trailingAnchor]]];
        } else [text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor].active=YES;
    }
    cell.textField.stringValue = values[column.identifier];
    cell.toolTip = values[@"path"];
    if (project) ((NSTextField*)[cell viewWithTag:1]).stringValue=values[@"path"];
    return cell;
}
- (void)applyStorageStatus:(const Json&)status {
    _storageStatus = status;
    const auto used=json_uint(status,"used_bytes"), shared=json_uint(status,"shared_bytes");
    NSDateFormatter* formatter = [[NSDateFormatter alloc] init];
        formatter.locale = [NSLocale localeWithLocaleIdentifier:shin::ui::language() == shin::ui::Language::ru ? @"ru_RU" : @"en_US"];
    formatter.dateStyle = NSDateFormatterShortStyle; formatter.timeStyle = NSDateFormatterShortStyle;
    NSString* measured = [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:static_cast<double>(json_uint(status,"sampled_at_ms"))/1000.0]];
    self.storageText.stringValue = [NSString stringWithFormat:ui_text("Shin service data: %@ of %@\nProject data: %@ · Shared service data: %@\nReclaimable: %@ · Free-space reserve: %@\n%@\nSampled: %@"),
        storage_bytes(used), storage_bytes(json_uint(status,"quota_bytes")), storage_bytes(json_uint(status,"project_bytes")), storage_bytes(shared),
        storage_bytes(json_uint(status,"reclaimable_bytes")), storage_bytes(json_uint(status,"reserve_free_bytes")), last_cleanup_status(status), measured];
    std::vector<Json> projects;
    if (status.contains("projects")) for (const auto& row : status.at("projects")) projects.push_back(row);
    std::stable_sort(projects.begin(),projects.end(),[](const auto& a,const auto& b){return json_uint(a,"used_bytes")>json_uint(b,"used_bytes");});
    NSMutableArray* rows = [NSMutableArray array];
    for (const auto& row : projects) {
        const auto root=row.at("root").get<std::string>();
        NSString* name=diagnostic_text_content(fs::path(root).filename().string());
        if (row.value("detached",false)) name=[name stringByAppendingString:ui_text(" · disconnected")];
        NSMutableDictionary* values = [@{@"project":name,@"path":diagnostic_text_content(root)} mutableCopy];
        for (NSString* key in @[@"used_bytes",@"index_bytes",@"cache_bytes",@"other_bytes",@"reclaimable_bytes"])
            values[key]=storage_bytes(json_uint(row,key.UTF8String));
        [rows addObject:values];
    }
    [rows addObject:@{@"project":ui_text("Shared service data"),@"path":ui_text("Data outside individual project directories"),
        @"used_bytes":storage_bytes(shared),@"index_bytes":@"—",@"cache_bytes":@"—",@"other_bytes":storage_bytes(shared),@"reclaimable_bytes":@"—"}];
    self.storageRows=rows; [self.storageTable reloadData]; [self layoutStorageColumns];
    self.storageAutoCleanEnabled = status.value("auto_clean", false);
    self.storageAutoClean.state = self.storageAutoCleanEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    self.storageClean.enabled = json_uint(status,"reclaimable_bytes")>0 && status.contains("revision") && !project_management_busy.load();
    self.storageAutoClean.enabled = !project_management_busy.load();
}
- (void)openStorageWindow:(id)sender {
    (void)sender;
    [self ensureStorageWindow];
    [NSApp activateIgnoringOtherApps:YES]; [self.storageWindow center]; [self.storageWindow makeKeyAndOrderFront:nil];
    [self refreshStorage:nil];
}
- (void)refreshStorage:(id)sender {
    (void)sender;
    const auto epoch=++self.storageRequestEpoch;
    _storageStatus = Json();
    live_text(self.storageText, @"stringValue", "Reading storage inventory…");
    self.storageClean.enabled = NO; self.storageAutoClean.enabled = NO;
    [self runCLI:@"storage-status" arguments:@[] completion:^(Json status, NSString* failure) {
        if (epoch!=self.storageRequestEpoch) return;
        if (failure || !status.value("ok", false)) {
            self.storageRows=@[]; [self.storageTable reloadData];
            self.storageText.stringValue = failure.length ? failure : friendly_cleanup_error(cli_error_code(status));
            return;
        }
        [self applyStorageStatus:status];
    }];
}

- (void)toggleStorageAutoClean:(id)sender {
    (void)sender;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) {
        self.storageAutoClean.state = self.storageAutoCleanEnabled ? NSControlStateValueOn : NSControlStateValueOff;
        return;
    }
    self.storageAutoClean.enabled = NO; self.storageClean.enabled = NO;
    _storageStatus = Json();
    const bool enable = self.storageAutoClean.state == NSControlStateValueOn;
    live_text(self.storageText, @"stringValue", "Saving automatic cleanup setting…");
    [self runCLI:@"storage-policy" arguments:@[@"--auto-clean", enable ? @"on" : @"off"] completion:^(Json result, NSString* failure) {
        if (failure || !result.value("ok", false)) {
            self.storageAutoClean.state = self.storageAutoCleanEnabled ? NSControlStateValueOn : NSControlStateValueOff;
            NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = ui_text("Setting unchanged");
            alert.informativeText = failure.length ? failure : ui_text("Shin did not confirm the policy change."); [alert runModal]; release_project_operation();
            [self refreshStorage:nil]; return;
        }
        self.storageAutoCleanEnabled = enable; release_project_operation(); [self refreshStorage:nil];
    }];
}

- (void)cleanStorage:(id)sender {
    (void)sender;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    self.storageClean.enabled = NO; self.storageAutoClean.enabled = NO;
    _storageStatus = Json();
    live_text(self.storageText, @"stringValue", "Checking search caches of disconnected projects…");
    // A fresh status response supplies the revision used for both preview and cleanup.
    [self runCLI:@"storage-status" arguments:@[] completion:^(Json status, NSString* failure) {
        if (failure || !status.value("ok", false)) {
            self.storageText.stringValue = failure.length ? failure : ui_text("Could not obtain a cleanup preview.");
            release_project_operation(); [self refreshStorage:nil]; return;
        }
        const auto bytes = status.value("reclaimable_bytes", 0ULL);
        const auto revision = status.value("revision", std::string{});
        if (!bytes || revision.empty()) {
            live_text(self.storageText, @"stringValue", "No search caches of disconnected projects to clean.");
            release_project_operation(); [self refreshStorage:nil]; return;
        }
        NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = ui_text("Clean Shin search caches?");
        alert.informativeText = [NSString stringWithFormat:ui_text("The preview allows reclaiming up to %@ of search caches of disconnected projects. Sources, indexes, history and Shin settings will be preserved."), storage_bytes(bytes)];
        [alert addButtonWithTitle:ui_text("Clean caches")]; [alert addButtonWithTitle:ui_text("Cancel")];
        if ([alert runModal] != NSAlertFirstButtonReturn) { release_project_operation(); [self refreshStorage:nil]; return; }
        NSString* revisionText = [NSString stringWithUTF8String:revision.c_str()];
        live_text(self.storageText, @"stringValue", "Cleaning search caches…");
        [self runCLI:@"storage-clean" arguments:@[@"--revision", revisionText ? revisionText : @""] completion:^(Json cleaned, NSString* cleanFailure) {
            self.storageText.stringValue = cleanFailure.length ? cleanFailure : cleanup_result_message(cleaned);
            release_project_operation();
            [self refreshStorage:nil];
        }];
    }];
}

- (void)showDiagnosticReport:(id)sender {
    (void)sender;
    if (diagnostic_report_text.empty()) return;
    [self closePanel:nil];
    if (self.diagnosticWindow) [self.diagnosticWindow close];
    self.diagnosticPayload = [NSString stringWithUTF8String:diagnostic_copy.c_str()];
    NSScreen* screen = NSScreen.mainScreen;
    const NSSize available = screen ? screen.visibleFrame.size : NSMakeSize(800, 700);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0,
        std::min(600.0, available.width - 32), std::min(660.0, available.height - 48))
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    live_text(window, @"title", "Project report · Shin");
    window.releasedWhenClosed = NO;
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window.minSize = NSMakeSize(360, 320);
    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.borderType = NSNoBorder;
    NSTextView* text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, window.contentView.bounds.size.width - 40, 100)];
    text.editable = NO;
    text.selectable = YES;
    text.verticallyResizable = YES;
    text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    text.textContainer.containerSize = NSMakeSize(text.frame.size.width, CGFLOAT_MAX);
    text.textContainerInset = NSMakeSize(20, 18);
    text.font = [NSFont systemFontOfSize:14];
    text.textColor = NSColor.labelColor;
    text.backgroundColor = NSColor.windowBackgroundColor;
    text.string = diagnostic_text_content(diagnostic_report_text);
    NSMutableParagraphStyle* paragraphs = [[NSMutableParagraphStyle alloc] init];
    paragraphs.lineSpacing = 4;
    paragraphs.lineBreakMode = NSLineBreakByWordWrapping;
    [text.textStorage addAttribute:NSParagraphStyleAttributeName value:paragraphs range:NSMakeRange(0, text.string.length)];
    const NSRange firstLine = [text.string lineRangeForRange:NSMakeRange(0, 0)];
    [text.textStorage addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:20 weight:NSFontWeightSemibold] range:firstLine];
    for (NSString* heading : @[ui_text("Next step"), ui_text("History"), ui_text("Technical details of the last observation")]) {
        const auto range = [text.string rangeOfString:[@"\n\n" stringByAppendingString:[heading stringByAppendingString:@"\n"]]];
        if (range.location != NSNotFound)
            [text.textStorage addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:14 weight:NSFontWeightSemibold]
                range:NSMakeRange(range.location + 2, range.length - 3)];
    }
    const auto technical = [text.string rangeOfString:@"\n{"];
    if (technical.location != NSNotFound) {
        const auto range = NSMakeRange(technical.location, text.string.length - technical.location);
        [text.textStorage addAttribute:NSFontAttributeName value:[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular] range:range];
        NSMutableParagraphStyle* codeStyle = [paragraphs mutableCopy];
        codeStyle.lineBreakMode = NSLineBreakByCharWrapping;
        [text.textStorage addAttribute:NSParagraphStyleAttributeName value:codeStyle range:range];
    }
    live_text(text, @"accessibilityLabel", "Project status report");
    scroll.documentView = text;
    NSButton* copy = live_button("Copy diagnostics", self, @selector(copyDiagnosticReport:));
    copy.image = [NSImage imageWithSystemSymbolName:@"doc.on.doc" accessibilityDescription:nil];
    copy.imagePosition = NSImageLeading;
    copy.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField* note = live_label("Last received observation", false);
    note.textColor = NSColor.secondaryLabelColor;
    note.font = [NSFont systemFontOfSize:12];
    note.lineBreakMode = NSLineBreakByTruncatingTail;
    [note setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    note.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:scroll];
    [window.contentView addSubview:copy];
    [window.contentView addSubview:note];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:window.contentView.topAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:copy.topAnchor constant:-12],
        [copy.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-20],
        [copy.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-14],
        [note.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:20],
        [note.centerYAnchor constraintEqualToAnchor:copy.centerYAnchor],
        [note.trailingAnchor constraintLessThanOrEqualToAnchor:copy.leadingAnchor constant:-12]]];
    self.diagnosticWindow = window;
    [window center];
    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}
- (void)copyDiagnosticReport:(id)sender {
    if (!self.diagnosticPayload) return;
    [NSPasteboard.generalPasteboard clearContents];
    if ([NSPasteboard.generalPasteboard setString:self.diagnosticPayload forType:NSPasteboardTypeString] &&
        [sender isKindOfClass:NSButton.class]) ((NSButton*)sender).title = ui_text("Copied");
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    NSString* home = [[NSUserDefaults standardUserDefaults] stringForKey:@"ShinHome"];
    try {
        if (const auto installed = installed_local_home(current_executable_path()))
            home = [NSString stringWithUTF8String:installed->c_str()];
    } catch (const std::exception& error) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = ui_text("Shin installation is incomplete");
        alert.informativeText = [NSString stringWithUTF8String:error.what()];
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    if (!home) home = [NSHomeDirectory() stringByAppendingPathComponent:@".shin-ce"];
    self.shinHome = home;
    // A fresh installation has no registry or running service. Make its first
    // action visible without relying on a successful service connection.
    if (![NSFileManager.defaultManager fileExistsAtPath:[home stringByAppendingPathComponent:@"registry"]])
        dispatch_async(dispatch_get_main_queue(), ^{ [self showSetup:nil]; });
    NSString* usageFile = [[NSUserDefaults standardUserDefaults] stringForKey:@"ShinUsageFile"];
    monitor = std::make_unique<Monitor>(std::string(home.fileSystemRepresentation), usageFile ? fs::path(usageFile.fileSystemRepresentation) : fs::path{});
    NSString* usageProject = [[NSUserDefaults standardUserDefaults] stringForKey:@"ShinUsageSelection"];
    if (!usageProject) {
        NSString* old = [[NSUserDefaults standardUserDefaults] stringForKey:@"ShinUsageProject"];
        if (old.length) usageProject = [@"usage/" stringByAppendingString:old];
    }
    const char* projectText = usageProject.UTF8String;
    if (projectText) model_project = projectText;
    dashboard_view = static_cast<int>(std::clamp<NSInteger>([[NSUserDefaults standardUserDefaults] integerForKey:@"ShinView"], 0, 3));
    self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    self.item.autosaveName = @"ShinMonitor";
    self.item.visible = YES;
    self.item.button.image = tray_logo();
    if (!self.item.button.image)
        self.item.button.image = [NSImage imageWithSystemSymbolName:@"circle.hexagongrid.fill" accessibilityDescription:@"Shin Monitor"];
    [self.item.button.image setTemplate:YES];
    self.item.button.title = @"";
    self.item.button.imagePosition = NSImageOnly;
    self.item.button.target = self;
    self.item.button.action = @selector(toggle:);
    [self.item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    self.content = [[ShinView alloc] init];
    self.popover = [[NSPopover alloc] init];
    self.popover.contentViewController = self.content;
    [self updatePanelSize];
    self.popover.behavior = NSPopoverBehaviorTransient;
    self.popover.animates = NO;
    self.popover.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.popover.delegate = self;
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self selector:@selector(activatedApplication:)
        name:NSWorkspaceDidActivateApplicationNotification object:nil];
    self.nextUpdateCheck=[NSDate dateWithTimeIntervalSinceNow:15];
    if([[NSUserDefaults standardUserDefaults] boolForKey:@"ShinUpdatePending"]) [self showUpdateResult];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* timer) {
        (void)timer;
        if (!monitor) return;
        if (![[NSUserDefaults standardUserDefaults] boolForKey:@"ShinAutomaticUpdatesDisabled"] &&
            self.nextUpdateCheck.timeIntervalSinceNow<=0) [self beginUpdateCheck:NO];
        self.item.button.title=release_update_version.empty() ? @"" : @"↑";
        self.item.length=release_update_version.empty() ? NSSquareStatusItemLength : NSVariableStatusItemLength;
        self.item.button.imagePosition=release_update_version.empty() ? NSImageOnly : NSImageLeading;
        NSWindow* statusWindow = self.item.button.window;
        NSScreen* screen = statusWindow.screen;
        if (!screen) screen = NSScreen.mainScreen;
        // Only our own numeric UI state is public; no project/window titles or paths.
        NSString* geometry = [NSString stringWithFormat:@"visible=%d window=%d frame=%@ image=%d active=%d screen=%@ right=%@ occluded=%d hidden=%d position=%ld",
            self.item.visible, statusWindow.visible, NSStringFromRect(statusWindow.frame),
            self.item.button.image != nil, NSApp.active, NSStringFromRect(screen.frame),
            NSStringFromRect(screen.auxiliaryTopRightArea),
            (statusWindow.occlusionState & NSWindowOcclusionStateVisible) == 0,
            self.item.button.hiddenOrHasHiddenAncestor, static_cast<long>(self.item.button.imagePosition)];
        if (self.statusSamples < 5) ++self.statusSamples;
        if (![geometry isEqualToString:self.statusGeometry] || self.statusSamples == 4) {
            self.statusGeometry = geometry;
            os_log(OS_LOG_DEFAULT, "ShinMonitor status-item: %{public}s", geometry.UTF8String);
        }
        const auto value = monitor->snapshot();
        self.item.button.toolTip = value.connected ? ui_text("Shin: service connected") : ui_text("Shin: no fresh service response");
        bool attention = false;
        if (value.connected) {
            attention = !value.data.at("registry_error").get<std::string>().empty() || !value.data.at("enrollment_errors").empty();
            for (const auto& row : value.data.at("telemetry").at("project_states"))
                attention = attention || shin::ui::active_issue(row);
        }
        const auto count = value.connected ? value.data.at("telemetry").at("project_states").size() : 0;
        if (value.connected) self.item.button.toolTip = [NSString stringWithFormat:
            attention ? ui_text("Shin: projects: %lu, attention required") :
                        ui_text("Shin: observed projects: %lu"), static_cast<unsigned long>(count)];
        if(!release_update_version.empty())self.item.button.toolTip=[NSString stringWithFormat:ui_text("Shin update available: %@"),diagnostic_text_content(release_update_version)];
        monitor->visible = self.popover.shown;
    }];
    if (!self.updateWindow.visible) [self toggle:nil];
}
- (void)updatePanelSize {
    NSScreen* screen = self.item.button.window.screen;
    if (!screen) screen = NSScreen.mainScreen;
    const NSSize available = screen ? screen.visibleFrame.size : NSMakeSize(800, 600);
    const bool compact = [[NSUserDefaults standardUserDefaults] boolForKey:@"ShinCompact"];
    self.popover.contentSize = NSMakeSize(std::min(compact ? 360.0 : 448.0, std::max(280.0, available.width - 24)),
        std::min(compact ? 440.0 : 620.0, std::max(240.0, available.height - 40)));
}
- (void)closePanel:(id)sender {
    (void)sender;
    if (self.popover.shown) [self.popover close];
}
- (void)applicationDidResignActive:(NSNotification*)notification {
    (void)notification;
    [self closePanel:nil];
}
- (void)activatedApplication:(NSNotification*)notification {
    NSRunningApplication* app = notification.userInfo[NSWorkspaceApplicationKey];
    if (app && app.processIdentifier != NSRunningApplication.currentApplication.processIdentifier) [self closePanel:nil];
}
- (void)toggle:(id)sender {
    (void)sender;
    if (NSApp.currentEvent.type == NSEventTypeRightMouseUp) {
        NSMenu* menu = [[NSMenu alloc] init];
        NSMenuItem* open = [menu addItemWithTitle:ui_text("Open panel") action:@selector(toggle:) keyEquivalent:@""];
        open.target = self;
        NSMenuItem* compact = [menu addItemWithTitle:ui_text("Compact view") action:@selector(toggleCompact:) keyEquivalent:@""];
        compact.target = self;
        NSMenuItem* storage = [menu addItemWithTitle:ui_text("Storage…") action:@selector(openStorageWindow:) keyEquivalent:@""];
        storage.target = self;
        NSMenuItem* update=[menu addItemWithTitle:ui_text("Check for updates…") action:@selector(checkForUpdates:) keyEquivalent:@""];update.target=self;
        NSMenuItem* automatic=[menu addItemWithTitle:ui_text("Automatically check for updates") action:@selector(toggleAutomaticUpdates:) keyEquivalent:@""];automatic.target=self;
        NSMenuItem* result=[menu addItemWithTitle:ui_text("Last update result…") action:@selector(showLastUpdateResult:) keyEquivalent:@""];result.target=self;
        automatic.state=[[NSUserDefaults standardUserDefaults] boolForKey:@"ShinAutomaticUpdatesDisabled"] ? NSControlStateValueOff : NSControlStateValueOn;
        compact.state = [[NSUserDefaults standardUserDefaults] boolForKey:@"ShinCompact"] ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItemWithTitle:ui_text("Quit Shin Monitor") action:@selector(terminate:) keyEquivalent:@""];
        [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, self.item.button.bounds.size.height) inView:self.item.button];
        return;
    }
    if (self.popover.shown) [self.popover close];
    else {
        [self updatePanelSize];
        [NSApp activateIgnoringOtherApps:YES];
        [self.popover showRelativeToRect:self.item.button.bounds ofView:self.item.button preferredEdge:NSRectEdgeMinY];
        ((MTKView*)self.content.view).paused = NO;
        [self.popover.contentViewController.view.window makeKeyWindow];
        __weak ShinApp* weakSelf = self;
        self.outsideMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
            handler:^(NSEvent* event) { (void)event; [weakSelf closePanel:nil]; }];
        self.escapeMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
            if (event.keyCode == 53 && weakSelf.popover.shown) { [weakSelf closePanel:nil]; return nil; }
            return event;
        }];
    }
}
- (void)toggleCompact:(id)sender {
    (void)sender;
    NSUserDefaults* preferences = [NSUserDefaults standardUserDefaults];
    [preferences setBool:![preferences boolForKey:@"ShinCompact"] forKey:@"ShinCompact"];
    [self updatePanelSize];
    if (!self.popover.shown) [self toggle:nil];
}
- (void)selectView:(NSMenuItem*)sender {
    activate(static_cast<int>(sender.tag));
    if (!self.popover.shown) [self toggle:nil];
}
- (void)chooseUsageFile:(id)sender {
    (void)sender;
    [self.popover close];
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    live_text(panel, @"title", "Normalized model history");
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    panel.canCreateDirectories = NO;
    panel.showsHiddenFiles = YES;
    if ([panel runModal] == NSModalResponseOK && panel.URL.path) {
        NSString* path = panel.URL.path;
        try {
            (void)read_model_usage(fs::path(path.fileSystemRepresentation), now_ms());
            [[NSUserDefaults standardUserDefaults] setObject:path forKey:@"ShinUsageFile"];
            [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"ShinUsageProject"];
            [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"ShinUsageSelection"];
            model_project.clear(); model_page = 0;
            monitor->set_usage_file(fs::path(path.fileSystemRepresentation));
        } catch (...) {
            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = ui_text("History not connected");
            alert.informativeText = ui_text("Select numeric history from the Shin collector (v2 or v3). The source file was preserved.");
            [alert runModal];
        }
    }
    [self toggle:nil];
}
- (void)chooseUsageProject:(id)sender {
    (void)sender;
    if (!monitor) return;
    const auto view = monitor->snapshot();
    const auto telemetry = view.data.is_object() ? view.data.value("telemetry", Json::object()) : Json::object();
    const auto choices = shin::ui::model_project_choices(*view.usage, telemetry.value("project_states", Json::array()));
    NSMenu* menu = [[NSMenu alloc] init];
    for (const auto& project : choices) {
        const auto name = project.at("name").get<std::string>() + (project.at("configured").get<bool>() ? "" : shin::ui::tr(" · not connected"));
        NSString* title = [NSString stringWithUTF8String:name.c_str()];
        NSString* identifier = [NSString stringWithUTF8String:project.at("key").get<std::string>().c_str()];
        if (!title || !identifier) continue;
        NSMenuItem* item = [menu addItemWithTitle:title action:@selector(selectUsageProject:) keyEquivalent:@""];
        item.representedObject = identifier; item.target = self;
        item.state = project.at("key") == model_project ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(16, self.content.view.bounds.size.height - 104) inView:self.content.view];
}
- (void)choosePeriod:(id)sender {
    (void)sender;
    NSMenu* menu = [[NSMenu alloc] init];
    const char* titles[]{shin::ui::tr("Now"), shin::ui::tr("Last hour"), shin::ui::tr("24 hours"), shin::ui::tr("7 days")};
    const int codes[]{20, 23, 21, 22}, hours[]{0, 1, 24, 168};
    for (int i = 0; i < 4; ++i) {
        NSString* title = [NSString stringWithUTF8String:titles[i]];
        if (!title) continue;
        NSMenuItem* item = [menu addItemWithTitle:title action:@selector(selectView:) keyEquivalent:@""];
        item.tag = codes[i]; item.target = self;
        item.state = history_hours == hours[i] ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(self.content.view.bounds.size.width - 116, self.content.view.bounds.size.height - 138) inView:self.content.view];
}
- (void)chooseTimingTraffic:(id)sender {
    (void)sender;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:ui_text("Query type")];
    NSArray<NSString*>* titles = @[ui_text("All queries"), ui_text("Unclassified"), ui_text("Diagnostic")];
    for (NSInteger i = 0; i < 3; ++i) {
        NSMenuItem* item = [menu addItemWithTitle:titles[i] action:@selector(selectView:) keyEquivalent:@""];
        item.tag = 63 + i; item.target = self;
        item.state = timing_filter == i ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(self.content.view.bounds.size.width / 2, self.content.view.bounds.size.height - 172) inView:self.content.view];
}
- (void)chooseTimingScope:(id)sender {
    (void)sender;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:ui_text("Measurement period")];
    NSArray<NSString*>* titles = @[ui_text("Recent requests in this run"), ui_text("All retained history (up to 7 days)")];
    for (NSInteger i = 0; i < 2; ++i) {
        NSMenuItem* item = [menu addItemWithTitle:titles[i] action:@selector(selectView:) keyEquivalent:@""];
        item.tag = 67 + i; item.target = self;
        item.state = timing_retained == (i == 1) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(16, self.content.view.bounds.size.height - 172) inView:self.content.view];
}
- (void)selectUsageProject:(NSMenuItem*)sender {
    NSString* identifier = sender.representedObject;
    if (![identifier isKindOfClass:NSString.class]) return;
    const char* projectText = identifier.UTF8String;
    if (!projectText) return;
    model_project = projectText; model_page = 0;
    [[NSUserDefaults standardUserDefaults] setObject:identifier forKey:@"ShinUsageSelection"];
    if (!self.popover.shown) [self toggle:nil];
}
- (void)popoverDidClose:(NSNotification*)notification {
    (void)notification;
    ((MTKView*)self.content.view).paused = YES;
    monitor_motion.initialized = false;
    monitor->visible = false;
    NSLog(@"ShinMonitor panel-close: rendering paused");
    if (self.outsideMonitor) { [NSEvent removeMonitor:self.outsideMonitor]; self.outsideMonitor = nil; }
    if (self.escapeMonitor) { [NSEvent removeMonitor:self.escapeMonitor]; self.escapeMonitor = nil; }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { (void)app; return NO; }
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    (void)app; (void)visible;
    // An explicit app-open request is distinct from ordinary focus/activation.
    if (!self.popover.shown) [self toggle:nil];
    return YES;
}
- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self.timer invalidate];
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
    if (self.outsideMonitor) { [NSEvent removeMonitor:self.outsideMonitor]; self.outsideMonitor = nil; }
    if (self.escapeMonitor) { [NSEvent removeMonitor:self.escapeMonitor]; self.escapeMonitor = nil; }
    ((MTKView*)self.content.view).paused = YES;
    monitor.reset();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOSX_Shutdown();
        ImGui_ImplMetal_Shutdown();
        ImGui::DestroyContext();
    }
}
@end

#ifdef SHIN_TRAY_UPDATE_WINDOW_TEST
#include "updates_focus_test.inc"
#elif defined(SHIN_TRAY_LAYOUT_TEST)
#include "layout_test.inc"
#else
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc > 1 && std::string_view(argv[1]) == "--update-progress") {
            if (argc != 4) return 2;
            const std::string language = argv[3];
            const std::string version = argv[2];
            if ((language != "en" && language != "ru") || version.size() > 64 ||
                version.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-") != std::string::npos) return 2;
            return shin::observer::release_update_progress_viewer(argv[2], shin::ui::language_for(language));
        }
        [NSApplication sharedApplication];
        initialize_language();
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        ShinApp* delegate = [[ShinApp alloc] init];
        NSApp.delegate = delegate;
        NSMenu* menu = [[NSMenu alloc] init];
        NSMenuItem* item = [[NSMenuItem alloc] init];
        NSMenu* application = [[NSMenu alloc] init];
        NSArray<NSString*>* titles = @[ui_text("Overview"), ui_text("Projects"), ui_text("Models"), ui_text("Sessions"),
            ui_text("Now"), ui_text("24 hours"), ui_text("7 days")];
        for (NSUInteger i = 0; i < titles.count; ++i) {
            NSMenuItem* choice = [application addItemWithTitle:titles[i] action:@selector(selectView:) keyEquivalent:@""];
            choice.tag = static_cast<NSInteger>(i < 4 ? 10 + i : 20 + i - 4);
            choice.target = delegate;
        }
        [application addItem:[NSMenuItem separatorItem]];
        NSMenuItem* compact = [application addItemWithTitle:ui_text("Compact view") action:@selector(toggleCompact:) keyEquivalent:@"k"];
        compact.target = delegate;
        NSMenuItem* usage = [application addItemWithTitle:ui_text("Model history…") action:@selector(chooseUsageFile:) keyEquivalent:@""];
        usage.target = delegate;
        NSMenuItem* setup = [application addItemWithTitle:ui_text("Set up Shin…") action:@selector(showSetup:) keyEquivalent:@""];
        setup.target = delegate;
        NSMenuItem* storage = [application addItemWithTitle:ui_text("Storage…") action:@selector(openStorageWindow:) keyEquivalent:@""];
        storage.target = delegate;
        [application addItemWithTitle:ui_text("Quit Shin Monitor") action:@selector(terminate:) keyEquivalent:@"q"];
        item.submenu = application;
        [menu addItem:item];
        NSApp.mainMenu = menu;
        [NSApp run];
    }
    return 0;
}
#endif
