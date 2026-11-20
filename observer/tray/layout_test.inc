// Test-only entry point: real ImGui/Metal rendering, no IPC or user preferences.
#import <QuartzCore/CATransaction.h>
#include <iostream>
#include "updates_window_test.inc"

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if ((argc != 4 && argc != 5 && argc != 6) || std::string(argv[2]) != "-ShinHome") throw std::runtime_error("output directory and test home required");
            const bool fit_test = argc == 6 && std::string_view(argv[5]) == "fit";
            if (argc == 6 && !fit_test) throw std::runtime_error("unknown render sizing mode");
            const auto initial_language = shin::ui::language_for(argc >= 5 ? argv[4] : "ru");
            shin::ui::set_language(initial_language);
            check_update_offer_actions();
            NSDictionary* arguments = [[NSUserDefaults standardUserDefaults] volatileDomainForName:NSArgumentDomain];
            if (![arguments[@"ShinHome"] isEqualToString:[NSString stringWithUTF8String:argv[3]]])
                throw std::runtime_error("explicit development home argument was not read");
            if ([ShinApp instancesRespondToSelector:NSSelectorFromString(@"chooseHome:")] ||
                [ShinView instancesRespondToSelector:NSSelectorFromString(@"settings")])
                throw std::runtime_error("service-directory picker remains exposed by the UI");
            if (![diagnostic_text_content("Valid report") isEqualToString:@"Valid report"] ||
                !diagnostic_text_content(std::string(1, '\xff')).length)
                throw std::runtime_error("diagnostic report UTF-8 boundary failed");
            ProfileReviewChannel channel;
            if (!channel.preview("/fixture/a") || channel.preview("/fixture/a"))
                throw std::runtime_error("duplicate review preview was queued");
            auto preview = channel.take();
            if (!preview || preview->kind != ProfileReviewAction::Kind::preview || preview->root != "/fixture/a")
                throw std::runtime_error("review preview did not pin the selected root");
            Json review{{"revision", "revision-a"}, {"review_token", "token-a"}, {"changed", true}, {"can_accept", true}};
            channel.preview_ready("/fixture/a", review);
            if (!channel.accept() || channel.accept()) throw std::runtime_error("review acceptance was repeated");
            auto approval = channel.take();
            if (!approval || approval->kind != ProfileReviewAction::Kind::accept || approval->root != "/fixture/a" ||
                approval->revision != "revision-a" || approval->token != "token-a")
                throw std::runtime_error("acceptance did not retain preview authority");
            channel.accepted("/fixture/a", 5000.0, true, 2);
            if (channel.preview("/fixture/a") || channel.preview("/fixture/b"))
                throw std::runtime_error("root changed or duplicate request entered while acceptance was pending");
            channel.observed("/fixture/a", 4500.0, true, 2, false, true, "blocked", true);
            if (channel.snapshot().status != "accepted-waiting")
                throw std::runtime_error("old blocked telemetry was mistaken for a new partial result");
            channel.observed("/fixture/a", 4500.0, true, 2, false, false, "current", false);
            if (channel.snapshot().status != "accepted-waiting")
                throw std::runtime_error("pre-approval successful telemetry was mistaken for a new check");
            channel.observed("/fixture/a", 0.0, true, 2, false, false, "current", false);
            if (channel.snapshot().status != "success") throw std::runtime_error("new successful telemetry was not reported");
            ProfileReviewChannel failed_channel;
            failed_channel.preview("/fixture/fail");
            failed_channel.take();
            failed_channel.preview_ready("/fixture/fail", review);
            failed_channel.accept();
            failed_channel.take();
            failed_channel.accepted("/fixture/fail", 5000.0, true, 2);
            failed_channel.observed("/fixture/fail", 4800.0, true, 3, true, true, "blocked", false);
            if (failed_channel.snapshot().status != "failed")
                throw std::runtime_error("new failed qualification was reported as partial success");
            const fs::path output = argv[1];
            fs::create_directories(output);
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
            [NSApp finishLaunching];
            if (![storage_bytes(0) isEqualToString:ui_text("0 B")]) throw std::runtime_error("storage zero size is not numeric");
            ShinApp* storageApp = [[ShinApp alloc] init];
            [storageApp ensureStorageWindow];
            for (const auto scrollerStyle : {NSScrollerStyleOverlay, NSScrollerStyleLegacy})
            for (const auto& scenario : {std::string("empty"),std::string("many")}) {
                storageApp.storageTable.enclosingScrollView.scrollerStyle=NSScrollerStyleOverlay;
                Json storage{{"ok",true},{"used_bytes",1024ULL*1024*1024},{"shared_bytes",123456ULL},
                    {"project_bytes",1024ULL*1024*1024-123456},{"quota_bytes",8ULL*1024*1024*1024},
                    {"reserve_free_bytes",2ULL*1024*1024*1024},{"reclaimable_bytes",456789ULL},
                    {"sampled_at_ms",1792011000000ULL},{"revision","fixture"},{"auto_clean",false},{"projects",Json::array()}};
                if (scenario=="many") for (unsigned i=0;i<16;++i)
                    storage["projects"].push_back({{"root","/fixture/checkout-"+std::to_string(i)+"/very-long-project-name"},
                        {"used_bytes",1000ULL+i*1000000},{"index_bytes",900ULL+i*900000},{"cache_bytes",90ULL+i*90000},
                        {"other_bytes",10ULL+i*10000},{"reclaimable_bytes",i%2 ? 10000 : 0},{"detached",i%2==1}});
                if (scenario=="empty") storage.update({{"used_bytes",0},{"project_bytes",0},{"shared_bytes",0},{"reclaimable_bytes",0}});
                std::uintmax_t project_bytes=0, reclaimable_bytes=0;
                for (const auto& row : storage.at("projects")) { project_bytes+=json_uint(row,"used_bytes"); reclaimable_bytes+=json_uint(row,"reclaimable_bytes"); }
                storage["project_bytes"]=project_bytes; storage["used_bytes"]=project_bytes+json_uint(storage,"shared_bytes");
                storage["reclaimable_bytes"]=reclaimable_bytes;
                [storageApp applyStorageStatus:storage];
                for (const auto size : {NSMakeSize(760,580),NSMakeSize(720,500)}) {
                    [storageApp.storageWindow setContentSize:size];
                    NSView* body=storageApp.storageWindow.contentView;
                    [storageApp.storageWindow orderFront:nil];
                    [body layoutSubtreeIfNeeded];
                    [storageApp layoutStorageColumns];
                    // A system scrollbar preference can change the clip width
                    // after the window's resize/layout callback has completed.
                    storageApp.storageTable.enclosingScrollView.scrollerStyle=scrollerStyle;
                    [storageApp.storageTable layoutSubtreeIfNeeded];
                    [storageApp.storageWindow displayIfNeeded];
                    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.04]];
                    [body layoutSubtreeIfNeeded];
                    if (![storageApp.storageClean.title isEqualToString:ui_text("Clean search caches…")] || !storageApp.storageText.stringValue.length)
                        throw std::runtime_error("storage control title or summary is missing");
                    const auto expected=scenario=="many" ? 17 : 1;
                    if (storageApp.storageTable.numberOfRows!=expected || storageApp.storageTable.numberOfColumns!=6)
                        throw std::runtime_error("storage table lost projects or columns");
                    const auto rect=storageApp.storageTable.enclosingScrollView.frame;
                    if (NSHeight(rect)<100 || NSIntersectsRect(rect,storageApp.storageAutoClean.frame) ||
                        NSIntersectsRect(rect,storageApp.storageText.frame))
                        throw std::runtime_error("storage table overlaps summary or controls");
                    const auto lastColumn=[storageApp.storageTable rectOfColumn:5];
                    if (NSMaxX(lastColumn)>NSWidth(storageApp.storageTable.enclosingScrollView.contentView.bounds)+1) {
                        Json columns=Json::array();
                        for (NSTableColumn* column in storageApp.storageTable.tableColumns) columns.push_back(column.width);
                        throw std::runtime_error("storage columns require horizontal scrolling: "+Json{{"columns",columns},
                            {"right",NSMaxX(lastColumn)},{"table",NSWidth(storageApp.storageTable.frame)},
                            {"available",NSWidth(storageApp.storageTable.enclosingScrollView.contentView.bounds)},
                            {"spacing",storageApp.storageTable.intercellSpacing.width}}.dump());
                    }
                    auto first=[storageApp.storageTable viewAtColumn:0 row:0 makeIfNecessary:YES];
                    if (!first || !first.toolTip.length) throw std::runtime_error("storage row lost full project path");
                    NSBitmapImageRep* bitmap=[body bitmapImageRepForCachingDisplayInRect:body.bounds];
                    [body cacheDisplayInRect:body.bounds toBitmapImageRep:bitmap];
                    NSData* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                    const auto name=output/(std::string("storage-")+(scrollerStyle==NSScrollerStyleLegacy ? "legacy-" : "")+
                        scenario+"-"+std::to_string(static_cast<int>(size.width))+".png");
                    if (!png.length || ![png writeToFile:diagnostic_text_content(name.string()) atomically:YES])
                        throw std::runtime_error("native storage window render failed");
                }
            }
            [storageApp.storageWindow close];
            if (live_text(static_cast<NSTextField*>(nil), @"stringValue", "Finding agent settings…") != nil)
                throw std::runtime_error("a not-yet-created setup label became an object");
            ShinApp* setupApp = [[ShinApp alloc] init];
            [setupApp showSetup:nil];
            if (!setupApp.setupChoose.enabled || setupApp.setupConnect.enabled || setupApp.setupCheck.enabled)
                throw std::runtime_error("first-run controls need a service or allow copying an empty config");
            for (const auto size : {NSMakeSize(720, 620), NSMakeSize(660, 620)}) {
                [setupApp.setupWindow setContentSize:size];
                NSView* body = setupApp.setupWindow.contentView;
                [body layoutSubtreeIfNeeded];
                [setupApp.setupWindow displayIfNeeded];
                [setupApp.setupAgents displayIfNeeded];
                [CATransaction flush];
                // Let AppKit finish the scroll document's native presentation
                // cycle before caching it; model frames settle before its layers.
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.25]];
                [body layoutSubtreeIfNeeded];
                [setupApp.setupWindow displayIfNeeded];
                for (NSView* control : @[setupApp.setupChoose, setupApp.setupConnect, setupApp.setupCheck, setupApp.setupLogin, setupApp.setupStatus, setupApp.setupAdvanced]) {
                    const auto rect = [control convertRect:control.bounds toView:body];
                    if (!NSContainsRect(body.bounds, rect) || rect.size.height < 10)
                        throw std::runtime_error("setup control clipped outside its window");
                }
                NSBitmapImageRep* bitmap = [body bitmapImageRepForCachingDisplayInRect:body.bounds];
                [body cacheDisplayInRect:body.bounds toBitmapImageRep:bitmap];
                NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                const auto name = output / ("setup-" + std::to_string(static_cast<int>(size.width)) + ".png");
                if (!png.length || ![png writeToFile:diagnostic_text_content(name.string()) atomically:YES])
                    throw std::runtime_error("setup snapshot failed");
            }
            [setupApp prepareProjectPresentation:@"/fixture/project"];
            [setupApp showProjectStage:0]; [setupApp showProjectStage:1]; [setupApp showProjectStage:2];
            [setupApp.setupWindow.contentView layoutSubtreeIfNeeded];
            [setupApp.setupWindow displayIfNeeded];
            if (!setupApp.setupWindow.visible || setupApp.setupChoose.enabled || setupApp.setupProgressRows.count != 6)
                throw std::runtime_error("project preparation is hidden or permits duplicate connection");
            NSView* progressBody = setupApp.setupWindow.contentView;
            NSBitmapImageRep* progressBitmap = [progressBody bitmapImageRepForCachingDisplayInRect:progressBody.bounds];
            [progressBody cacheDisplayInRect:progressBody.bounds toBitmapImageRep:progressBitmap];
            NSData* progressPNG = [progressBitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if (![progressPNG writeToFile:diagnostic_text_content((output / "setup-progress.png").string()) atomically:YES])
                throw std::runtime_error("project preparation snapshot failed");
            [setupApp.setupProgressTimer invalidate]; [setupApp.setupProgressSpinner stopAnimation:nil];
            setupApp.setupAdding = NO;
            [setupApp.setupWindow standardWindowButton:NSWindowCloseButton].enabled = YES;
            setupApp.setupRoot = nil; [setupApp showSetup:nil];
            // Change language in place: retained Cocoa windows and draft state
            // must survive without running setup discovery or service commands.
            setupApp.setupRoot = @"/fixture/project";
            setupApp.setupProject.stringValue = setupApp.setupRoot;
            NSButton* choice = live_button("Connect selected agents", setupApp, @selector(selectSetupTarget:), true);
            choice.state = NSControlStateValueOn;
            [setupApp.setupAgents addArrangedSubview:choice];
            const auto setup_epoch = setupApp.setupEpoch;
            storageApp.storageAutoClean.state = NSControlStateValueOn;
            storageApp.storageClean.enabled = NO;
            storageApp.storageAutoClean.enabled = NO;
            NSString* storage_path = storageApp.storageRows.firstObject[@"path"];
            id saved_preference = [[NSUserDefaults standardUserDefaults] objectForKey:@"ShinLanguage"];
            for (const int preference : {1, 2, 0, 2, 1}) {
                select_language_preference(preference);
                const auto expected = shin::ui::resolve_language(preference == 1 ? "en" : preference == 2 ? "ru" : "auto", platform_system_language());
                [setupApp refreshLanguage:nil]; [storageApp refreshLanguage:nil];
                if (shin::ui::language() != expected || language_preference != preference ||
                    ![setupApp.setupWindow.title isEqualToString:ui_text("Set up Shin")] ||
                    ![setupApp.setupChoose.title isEqualToString:ui_text("Choose project folder…")] ||
                    ![storageApp.storageWindow.title isEqualToString:ui_text("Shin storage")] ||
                    ![storageApp.storageTable.tableColumns[0].title isEqualToString:ui_text("Project")] ||
                    [storageApp.storageText.stringValue rangeOfString:ui_text("Last cleanup: not run yet")].location == NSNotFound)
                    throw std::runtime_error("retained native windows did not change language immediately");
                if (storageApp.storageAutoClean.state != NSControlStateValueOn || storageApp.storageClean.enabled || storageApp.storageAutoClean.enabled)
                    throw std::runtime_error("language switch changed a pending storage control");
                if (choice.state != NSControlStateValueOn || ![setupApp.setupProject.stringValue isEqualToString:setupApp.setupRoot] ||
                    setupApp.setupEpoch != setup_epoch || ![storage_path isEqualToString:storageApp.storageRows.firstObject[@"path"]])
                    throw std::runtime_error("language switch changed draft selection, path or setup lifecycle");
            }
            if (saved_preference != [[NSUserDefaults standardUserDefaults] objectForKey:@"ShinLanguage"] &&
                ![saved_preference isEqual:[[NSUserDefaults standardUserDefaults] objectForKey:@"ShinLanguage"]])
                throw std::runtime_error("render test changed real language preferences");
            shin::ui::set_language(initial_language);
            [setupApp.setupWindow close];
            auto device = MTLCreateSystemDefaultDevice();
            if (!device) return 77;
            auto queue = [device newCommandQueue];
            configure_imgui();
            ImGui_ImplMetal_Init(device);
            load_icons(device);
            dashboard_view = 1;
            const Json base{{"id", "fixture"}, {"name", "Example Workspace"}, {"root", "/fixture/workspace"},
                {"phase", "current"}, {"last_completed_phase", "current"}, {"active_issue", false},
                {"last_error_code", "refresh-validation-failed"}, {"error_code", ""},
                {"last_success_age_ms", 14000}, {"activity_age_ms", 1000}, {"failures", 5},
                {"publications", 12}, {"preparations", 12}, {"recoveries", 0}, {"last_tick_ms", 42},
                {"authority", "local-canonical"}, {"query_ready", true}};
            unsigned rendered = 0;
            for (const auto maximum_size : {ImVec2{520, 680}, ImVec2{620, 810}, ImVec2{400, 560}, ImVec2{448, 620}, ImVec2{432, 460}, ImVec2{360, 440}, ImVec2{280, 340}, ImVec2{280, 240}}) {
                if (fit_test && maximum_size.x != 520) continue;
                for (const auto* scenario : {"project-analytics", "project-analytics-empty", "project-analytics-offline", "first-run", "update", "cold", "current", "documentation-warning", "redacted-lines", "profile-partial", "blocked", "secret-blocked", "offline", "pending", "warming", "history", "unknown-history", "projects", "projects-last-page", "project-states",
                     "projection-partial", "timing-live-partial", "timing-live", "timing-retained", "timing-missing", "timing-offline", "timing-empty", "timing-partial",
                     "overview", "overview-hourly", "overview-warning", "overview-warning-short", "search-warning", "search-warning-details", "search-warning-invalid", "search-warning-opened", "settings", "settings-history", "about", "project-settings", "project-disconnect", "language-live-en", "language-live-ru", "language-live-auto", "sessions", "sessions-project", "sessions-partial", "sessions-full", "sessions-full-end", "sessions-empty", "sessions-notes", "sessions-details", "sessions-reorder-back", "sessions-unavailable", "sessions-omitted", "models", "models-partial", "models-stale", "models-details", "route-review", "profile-review", "profile-review-loading", "profile-review-error",
                     "profile-review-stale", "profile-review-waiting", "profile-review-success", "profile-review-partial", "profile-review-failed", "profile-review-disabled"}) {
                    const std::string kind = scenario;
                    if (!fit_test && kind == "overview-warning-short") continue;
                    const bool test_journal_input = kind == "sessions-reorder-back";
                    if ((test_journal_input || kind == "overview-hourly") && maximum_size.x != 520) continue;
                    auto limit = maximum_size;
                    if (kind == "overview-warning-short") limit.y = 420;
                    auto size = limit;
                    if (kind == "overview-warning-short") size.y = 280;
                    if (fit_test && kind == "overview") size.y = 280;
                    Json project = base;
                    if (kind == "documentation-warning") project["documentation_warnings"] = 1;
                    release_update_version=kind=="update" ? "2.0.5-dev.18" : "";
                    apply_language_preference(initial_language == shin::ui::Language::ru ? 2 : 1);

                    profile_review_open = kind.starts_with("profile-review-") || kind == "profile-review";
                    profile_review_root = "/fixture/workspace";
                    timing_open = kind.starts_with("timing-");
                    dashboard_view = kind.starts_with("overview") ? 0 :
                        timing_open || kind.starts_with("models") || kind.starts_with("sessions") ? 2 :
                        kind == "about" ? 4 : kind.starts_with("settings") || kind == "update" ? 3 : 1;
                    settings_view = kind == "update" ? SettingsPage::updates :
                        kind == "settings-history" ? SettingsPage::history : SettingsPage::overview;
                    project_view = kind == "project-settings" ? ProjectPage::settings :
                        kind == "project-disconnect" ? ProjectPage::disconnect : ProjectPage::overview;
                    journal_entry.reset();
                    journal_project = kind == "sessions-project" ? std::string(64, 'b') : "";
                    if (kind.starts_with("sessions")) project["id"] = std::string(64, 'b');
                    activity_view = kind.starts_with("models") ? 1 : kind.starts_with("sessions") ? 2 : 0;
                    diagnostic_details_open = kind == "search-warning-details" || kind == "search-warning-invalid";
                    if (kind.starts_with("search-warning") || kind.starts_with("overview-warning")) project["quality_warning"] = {
                        {"state", "degraded"}, {"diagnostic", {{"schema_version", "shin.validation-diagnostic.v1"}, {"gates", Json::array({
                            {{"id", "eval"}, {"cases", Json::array({{{"id", "fixture-discovery"},
                                {"expected_paths", Json::array({"docs/access.md"})}, {"reasons", Json::array({"missing-required-path"})}}})}}
                        })}}}};
                    if (kind == "search-warning-invalid") project["quality_warning"]["diagnostic"] = nullptr;
                    timing_retained = kind != "timing-live" && kind != "timing-live-partial";
                    timing_filter = 0;
                    if (kind == "blocked") project.update({{"phase", "blocked"}, {"active_issue", true}, {"error_code", "refresh-validation-failed"}});
                    if (kind == "route-review") project.update({{"phase", "blocked"}, {"active_issue", true}, {"error_code", "route-revision-conflict"}});
                    if (kind == "profile-partial") project["source_coverage"] = {{"state", "partial"}, {"reason", "profile-revision-pending"}};
                    if (kind == "redacted-lines") project["source_coverage"] = {{"state", "partial"}, {"redacted_files", 2},
                        {"files", Json::array({{{"path", "tests/auth_test.cpp"}}, {{"path", "src/credentials.ts"}}})}};
                    if (kind == "secret-blocked") project.update({{"phase", "blocked"}, {"active_issue", true},
                        {"error_code", "refresh-validation-failed"}, {"validation_diagnostic", {
                            {"schema_version", "shin.validation-diagnostic.v1"},
                            {"gates", Json::array({{{"id", "doctor"}, {"reasons", Json::array({"secret-like-source-content"})}}})}}}});
                    if (kind == "pending") project.update({{"phase", "checking"}, {"query_ready", false},
                        {"name", "Shin-Context-Engine-a-very-long-project-name"}});
                    if (kind == "warming") project.update({{"query_ready", false}, {"query_phase", "preparing"}});
                    ViewState view;
                    view.connected = kind != "first-run" && kind != "offline" && kind != "timing-offline";
                    view.agent_access_root = "/fixture/workspace";
                    view.agent_access = std::make_shared<Json>(Json::array({
                        {{"label", "Codex"}, {"access_state", "configured"}},
                        {{"label", "Claude Code"}, {"access_state", "not-configured"}}
                    }));
                    view.profile_review = std::make_shared<ProfileReviewChannel>();
                    if (kind.starts_with("profile-review")) {
                        view.profile_review->preview("/fixture/workspace");
                        if (kind == "profile-review-loading") {
                        } else if (kind == "profile-review-error") {
                            view.profile_review->failed("/fixture/workspace", "profile-review-unavailable");
                        } else if (kind == "profile-review-stale") {
                            view.profile_review->failed("/fixture/workspace", "profile-review-changed");
                        } else {
                            Json review{{"schema_version", "shin.profile-review.v1"}, {"revision", "rev-fixture"},
                                {"review_token", "token-fixture"}, {"changed", true},
                                {"can_accept", kind != "profile-review-disabled"}, {"baseline_available", true},
                                {"checks_preserved", true}, {"categories", Json::array({
                                    {{"id", "documents"}, {"added", 2}, {"removed", 1}, {"changed", 0}, {"added_paths", Json::array({"docs/guide.md"})}},
                                    {{"id", "document-selection"}, {"added", 1}, {"removed", 0}, {"changed", 0}, {"added_paths", Json::array({"docs/platform/FIRESTORE-QUERIES.md"})}},
                                    {{"id", "sources"}, {"added", 1}, {"removed", 0}, {"changed", 0}},
                                    {{"id", "exclusions"}, {"added", 1}, {"removed", 1}, {"changed", 0}},
                                    {{"id", "extensions"}, {"added", 0}, {"removed", 1}, {"changed", 0}},
                                    {{"id", "checks"}, {"added", 0}, {"removed", 0}, {"changed", 1}},
                                    {{"id", "limits"}, {"added", 0}, {"removed", 0}, {"changed", 1}},
                                    {{"id", "guidance"}, {"added", 1}, {"removed", 0}, {"changed", 0}},
                                    {{"id", "other"}, {"added", 0}, {"removed", 0}, {"changed", 1}}})},
                                {"changed_fields", Json::array({"sources.include", "checks.doctor"})}};
                            view.profile_review->preview_ready("/fixture/workspace", std::move(review));
                            if (kind == "profile-review-waiting" || kind == "profile-review-success" || kind == "profile-review-partial" || kind == "profile-review-failed") {
                                view.profile_review->accept();
                                view.profile_review->accepted("/fixture/workspace", 5000.0, true, 0);
                                if (kind != "profile-review-waiting")
                                    view.profile_review->observed("/fixture/workspace", 0.0, true,
                                        kind == "profile-review-failed" ? 1 : 0, false, kind == "profile-review-failed",
                                        kind == "profile-review-failed" ? "blocked" : "current",
                                        kind == "profile-review-partial", kind == "profile-review-partial" ? 2 : 0);
                            }
                        }
                    }
                    if (kind == "first-run") { dashboard_view = 0; timing_open = false; selected.clear(); }
                    view.received = Clock::now();
                    view.error = view.connected ? "" : "service-disconnected";
                    Json projects = kind == "first-run" ? Json::array() : Json::array({project});
                    if (kind.starts_with("projects") || kind == "project-states") for (int i = 1; i < (kind == "projects-last-page" ? 10 : 9); ++i) {
                        auto other = base; other["id"] = "project-" + std::to_string(i);
                        other["name"] = "Project " + std::to_string(i);
                        if (kind == "project-states") {
                            if (i == 1) other.update({{"query_ready", false}, {"query_phase", "preparing"}});
                            if (i == 2) other["source_coverage"] = {{"state", "partial"}, {"redacted_files", 2}};
                            if (i == 3) other.update({{"phase", "blocked"}, {"active_issue", true}, {"error_code", "refresh-validation-failed"}});
                            if (i == 4) other["query_phase"] = "standby";
                            if (i == 5) other.update({{"phase", "coalescing"}, {"query_ready", false}});
                            if (i == 6) other["recovering"] = true;
                            if (i == 7) other["documentation_warnings"] = 1;
                        }
                        projects.push_back(other);
                    }
                    if (kind == "sessions")
                        projects.push_back({{"id", std::string(64, 'c')}, {"name", "Example API"}});
                    view.data = {{"version", SHIN_OBSERVER_VERSION}, {"core_version", "2.0.5-dev.37"},
                        {"registry_error", ""}, {"enrollment_errors", Json::object()}, {"query_workers", 0},
                        {"resident_projects", Json::array()}, {"telemetry", {{"project_states", projects},
                        {"history", {{"status", kind == "unknown-history" ? "unavailable" : "ready"}, {"rows", 14000},
                        {"projects", Json::array({{{"id", "fixture"}, {"requests", 1543}, {"errors", 5}, {"unobserved", 0}}})}}}}}};
                    if (kind.starts_with("overview")) {
                        // Retained buckets include a gap and restart in the ordinary Overview.
                        history_hours = kind == "overview-hourly" ? 24 : 1;
                        auto& telemetry = view.data["telemetry"];
                        telemetry["samples"] = Json::array({{{"elapsed_ms", 3600000}, {"rss_bytes", 128 * 1048576}, {"workers", 0}}});
                        auto& history = telemetry["history"];
                        constexpr std::int64_t captured = 1792756800000;
                        history["captured_at_ms"] = captured;
                        history["oldest_at_ms"] = captured - 3600000;
                        history["minute_timeline"] = Json::array();
                        for (int minute = 0; minute <= 60; ++minute) {
                            const double pulse = std::max(0.0, 1.0 - std::abs(minute - 30) / 18.0);
                            const double rss = (118 + minute / 6.0 + 53 * pulse + (minute % 10 < 5 ? 1 : 0)) * 1048576;
                            const bool missing = kind == "overview" && minute >= 24 && minute <= 27;
                            const bool restarted = kind == "overview" && minute >= 45;
                            const double latency = 65 + 34 * pulse + minute % 7;
                            history["minute_timeline"].push_back(Json::array({captured - (60 - minute) * 60000,
                                1 + minute % 3 + static_cast<int>(6 * pulse), 0, 0, latency * 4, 4, missing ? Json(nullptr) : Json(rss),
                                missing ? Json(nullptr) : Json(rss), missing ? 0 : 30, 1,
                                restarted ? "run-b" : "run-a"}));
                        }
                        if (history_hours == 24) {
                            history["timeline"] = Json::array();
                            // Hourly rows have no service-run identity.
                            for (int hour = 2; hour <= 24; ++hour) {
                                const int requests = 14 + hour % 7;
                                const auto rss = (102 + hour % 12 * 5) * 1048576;
                                history["timeline"].push_back(Json::array({captured - (24 - hour) * 3600000LL,
                                    requests, 0, 0, requests * (28 + hour), requests, 62, rss, 30, 1}));
                            }
                        }
                    }
                    if (kind == "projection-partial" || kind == "timing-live-partial")
                        view.data["telemetry"]["projection"] = {{"truncated", true}};
                    if (timing_open && kind != "timing-missing") {
                        auto& telemetry = view.data["telemetry"];
                        telemetry["mcp_activity"] = {{"schema_version", "shin.mcp-activity.v1"}, {"status", "ready"},
                            {"attempts", 200}, {"calls", {{"query", 100}, {"inspect", 24}, {"read", 31}, {"status", 17}, {"help", 18}, {"projects", 10}}},
                            {"outcomes", {{"success", 190}, {"tool-error", 7}, {"rpc-error", 2}, {"unfinished", 1}}},
                            {"coverage_start_ms", 1792920000000LL}};
                        telemetry["activity"] = {{"schema_version", "shin.service-activity.v1"},
                            {"calls", {{"query", 100}, {"inspect", 24}, {"read", 31}, {"status", 17}}},
                            {"diagnostic", 10}, {"uncorrelated", 42}, {"reported_sessions", 64}, {"sessions_truncated", true},
                            {"models", {{"fixture-model-a", 80}, {"fixture-model-b", 50}}}, {"model_unknown", 42}, {"model_overflow", 0}};
                        telemetry["request_metrics"]["recent"] = Json::array({
                            {{"method", "query"}, {"duration_ms", 25}, {"service_duration_ms", 160}, {"reader_state", "warm"},
                             {"purpose", "unclassified"}, {"phase_ms", {{"admission", 3}, {"documents", 4}, {"source", 15}, {"source_collection", 5}, {"final_verification", 3}}}},
                            {{"method", "query"}, {"duration_ms", 40}, {"service_duration_ms", 1250}, {"reader_state", "cold"}, {"purpose", "diagnostic"}}
                        });
                        telemetry["history"]["request_cohorts"] = Json::array({
                            Json::array({"fixture", "query", "unclassified", "warm", 90, 0, 90, 120, 240, 25, 80, 3, 4, 15, 5, 3, 90, 90, 90, 90, 90, 90}),
                            Json::array({"fixture", "query", "diagnostic", "cold", 10, 0, 10, 1500, 2200, 40, 80, 3, 5, 24, 5, 8, 10, 10, 10, 10, 10, 10})
                        });
                        if (kind == "timing-empty") telemetry["history"]["request_cohorts"] = Json::array();
                        if (kind == "timing-partial") telemetry["history"]["request_cohorts_truncated"] = true;
                        if (kind == "timing-live-partial") telemetry["request_metrics"]["recent_truncated"] = true;
                    }
                    if (kind.starts_with("models")) {
                        model_project.clear();
                        Json totals{{"input_tokens", 5700000000ULL}, {"output_tokens", 15600000}, {"cache_read_tokens", 5600000000ULL}};
                        Json models = Json::array();
                        for (const auto* label : {"model-x", "model-y", "model-z"}) {
                            auto row = totals; row.update({{"model", label}, {"client", "codex"}, {"records", 25424}}); models.push_back(row);
                        }
                        auto day = totals; day["at_ms"] = 1792007810915LL;
                        view.usage = std::make_shared<const Json>(Json{{"status", "available"}, {"fresh", kind != "models-stale"},
                            {"complete", kind == "models"}, {"unassigned_sessions", 42}, {"skipped_sessions", kind == "models" ? 0 : 92},
                            {"sources", {{"codex", {{"state", kind == "models" ? "available" : "partial"},
                                {"skipped_reasons", kind == "models" ? Json::object() : Json{{"rollout-missing", 92}, {"rollout-lines-omitted", 6}}}}}}},
                            {"projects", Json::array({{{"id", ""}, {"name", "Все проекты"}, {"totals", totals},
                                {"models", models}, {"days", Json::array({day})}}})}});
                    }
                    if (kind.starts_with("sessions")) {
                        const std::string session_id(64,'a');
                        view.data["telemetry"]["mcp_activity"] = {{"status","ready"},{"window_start_ms",0},{"dropped_updates",0},{"unconfirmed_updates",0},{"sessions_truncated",kind=="sessions-partial"},
                            {"session_rows",Json::array({{{"session_id",session_id},{"context_calls",21},{"errors",2},{"tasks",3},
                                {"recovery_requests",4},{"model_unknown",21},{"models",Json::object()},
                                {"query_times",Json::array({1792920003500LL})},{"queries_complete",true},
                                {"timeline",Json::array({{{"method","query"},{"intent","context-recovery"},{"outcome","success"},{"at_ms",1792920003500LL}},
                                    {{"method","read"},{"intent","follow-up"},{"outcome","success"},{"at_ms",1792920004500LL}},
                                    {{"method","query"},{"intent","explore"},{"outcome","tool-error"},{"at_ms",1792920005500LL}},
                                    {{"method","inspect"},{"intent","explore"},{"outcome","unfinished"},{"at_ms",1792920006500LL}}})}}})}};
                        view.usage = std::make_shared<const Json>(Json{{"context_events",{{"source","codex"},{"complete",kind=="sessions"},
                            {"events",Json::array({{{"session_id",session_id},{"at_ms",1792920002000LL},{"kind","compaction"}},
                                {{"session_id",session_id},{"at_ms",1792920001000LL},{"kind","turn-model"},{"model_id","history-model"}}})}}}});
                    }
                    if (kind.starts_with("sessions")) {
                        auto& row = view.data["telemetry"]["mcp_activity"]["session_rows"][0];
                        row["timeline"][0].update({{"project_id", std::string(64, 'b')}, {"duration_ms", 71}, {"model_id", "fixture-model"}});
                        row["timeline"][1].update({{"project_id", std::string(64, 'c')}, {"duration_ms", 17}});
                        if (kind == "sessions") {
                            row["timeline"][2].update({{"project_id", std::string(64, 'b')}, {"duration_ms", 124}});
                            row["timeline"][3].update({{"project_id", std::string(64, 'c')}});
                            for (std::size_t event = 0; event < row["timeline"].size(); ++event)
                                row["timeline"][event]["at_ms"] = 1792920003500LL + 180000 * static_cast<std::int64_t>(event);
                        }
                        row["analysis"] = {{"schema_version","shin.session-analysis.v1"},{"calls_complete",kind=="sessions"},
                            {"unassigned_context_calls",3},{"reads",{{"reads",12},{"same_version_repeats",4},{"additional_versions",2},{"unknown_reads",1}}},
                            {"episodes",Json::array({{{"start_ms",1792920000000LL},{"kind","compaction"},{"closed",true},
                                {"context_calls",7},{"errors",1},{"interval_complete",kind=="sessions"},
                                {"reads",{{"same_version_repeats",2},{"previously_seen_versions",1}}}}})}};
                        session_details_open.clear(); session_notes_open = false;
                        if (kind == "sessions-empty" || kind == "sessions-notes" || kind == "sessions-omitted")
                            view.data["telemetry"]["mcp_activity"]["session_rows"] = Json::array();
                        if (kind == "sessions-omitted")
                            view.data["telemetry"]["projection"] = {{"truncated",true},{"omitted_rows",{{"/mcp_activity/session_rows",8}}}};
                        if (kind == "sessions-unavailable") view.data["telemetry"].erase("mcp_activity");
                        if (kind == "sessions-partial") view.data["telemetry"]["mcp_activity"]["dropped_updates"] = 1;
                        if (kind.starts_with("sessions-full")) {
                            auto& rows = view.data["telemetry"]["mcp_activity"]["session_rows"];
                            auto row = rows[0]; rows = Json::array();
                            for (int i = 0; i < 8; ++i) {
                                row["session_id"] = std::string(64,static_cast<char>('0'+i));
                                row["timeline"] = Json::array();
                                for (int event = 0; event < 8; ++event)
                                    row["timeline"].push_back({{"method","query"},{"intent","context-recovery"},
                                        {"outcome",event%2 ? "tool-error" : "success"},{"at_ms",1792920000000LL+event*1000+i}});
                                rows.push_back(row);
                            }
                        }
                        if (kind == "sessions-notes") activate(90);
                    }
                    if (kind.starts_with("project-analytics")) {
                        auto& telemetry = view.data["telemetry"];
                        view.connected = kind != "project-analytics-offline";
                        auto& recent = telemetry["request_metrics"]["recent"];
                        recent = Json::array();
                        if (kind != "project-analytics-empty") {
                            for (int i = 0; i < 32; ++i)
                                recent.push_back({{"project_id", "fixture"}, {"method", i % 5 == 0 ? "read" : i % 7 == 0 ? "inspect" : "query"},
                                    {"elapsed_ms", i * 2000}, {"duration_ms", 12 + i % 9},
                                    {"service_duration_ms", 40 + (i % 9) * 7 + (i == 12 ? 240 : 0)}, {"outcome", "response-ready"}});
                            telemetry["history"]["request_cohorts"] = Json::array({
                                Json::array({"fixture", "query", "unclassified", "warm", 1240, 2, 1240, 72, 320, 18, 64, 2, 5, 8, 3, 2, 1240, 1240, 1240, 1240, 1240, 1240})});
                        } else telemetry["history"]["request_cohorts"] = Json::array();
                    }
                    selected = kind.starts_with("projects") || kind == "project-states" ? "" : "fixture";
                    project_page = kind == "projects-last-page" ? 100 : 0;
                    if (kind == "cold") { view = ViewState{}; selected.clear(); }
                    profile_accept_within_parent_clip = false;
                    profile_accept_origin = ImVec2{};
                    profile_accept_size = ImVec2{};
                    profile_accept_clip_min = ImVec2{};
                    profile_accept_clip_max = ImVec2{};
                    diagnostic_history_first = kind == "history" || kind == "unknown-history";
                    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                        width:static_cast<NSUInteger>(size.x * 2) height:static_cast<NSUInteger>(size.y * 2) mipmapped:NO];
                    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                    descriptor.storageMode = MTLStorageModeShared;
                    auto texture = [device newTextureWithDescriptor:descriptor];
                    auto* retained_context = ImGui::GetCurrentContext();
                    const int live_preference = kind == "language-live-en" ? 1 : kind == "language-live-ru" ? 2 : 0;
                    std::set<int> reachable_settings;
                    UiAction first_journal_action{};
                    const int frames = kind == "settings" ? 18 : kind == "overview-warning-short" ? 12 :
                        test_journal_input ? 10 : fit_test ? 8 : 3;
                    for (int frame = 0; frame < frames; ++frame) {
                        // Begin on the real short first-run page, then deliver
                        // a project with a warning under a smaller screen cap.
                        if (kind == "overview-warning-short" && frame == 3) {
                            size.y = 280;
                            descriptor.height = 560;
                            texture = [device newTextureWithDescriptor:descriptor];
                        }
                        if (kind == "search-warning-opened" && frame == 1) activate(120);
                        if (kind == "sessions-details" && frame == 1 && journal_rows.front())
                            activate(open_journal_record, journal_rows.front()->event.key);
                        if (kind == "sessions-details" && frame == 2) activate(journal_session_details);
                        if (test_journal_input && frame == 1) {
                            const auto first = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                                return action.chart == open_journal_record && action.enabled;
                            });
                            if (first == ui_actions.end() || !first->item_id || first->target.empty())
                                throw std::runtime_error("journal entry could not be selected");
                            first_journal_action = *first;
                        }
                        if (test_journal_input && frame == 2) {
                            view.data["telemetry"]["mcp_activity"]["session_rows"][0]["timeline"].push_back({
                                {"call_id", std::string(64, 'd')}, {"method", "query"}, {"outcome", "success"}, {"at_ms", 1792920007500LL}});
                        }
                        if (test_journal_input && frame == 3) {
                            const auto previous = std::find_if(ui_actions.begin(), ui_actions.end(), [&](const auto& action) {
                                return action.chart == open_journal_record && action.target == first_journal_action.target;
                            });
                            if (!journal_rows.front() || journal_rows.front()->event.key == first_journal_action.target ||
                                previous == ui_actions.end() || previous->item_id != first_journal_action.item_id)
                                throw std::runtime_error("inserting a newer event changed a journal control's identity");
                            if (journal_entry) throw std::runtime_error("releasing a reordered row activated its replacement");
                            activate(open_journal_record, first_journal_action.target);
                            if (!journal_entry || journal_entry->event.key != first_journal_action.target)
                                throw std::runtime_error("a retained journal action opened another event");
                        }
                        if (kind.starts_with("language-live-")) {
                            if (frame == 0) shin::ui::set_language(live_preference == 2 ? shin::ui::Language::en : shin::ui::Language::ru);
                            if (frame == 1) select_language_preference(live_preference);
                        }
                        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                        pass.colorAttachments[0].texture = texture;
                        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                        pass.colorAttachments[0].clearColor = MTLClearColorMake(.09, .10, .11, 1);
                        auto& io = ImGui::GetIO(); io.DisplaySize = size; io.DisplayFramebufferScale = {2, 2}; io.DeltaTime = 1.0f / 20;
                        if (kind == "settings") {
                            io.AddMousePosEvent(size.x / 2, size.y - 65);
                            if (frame >= 3 && frame < 14) io.AddMouseWheelEvent(0, -1);
                            if (frame == 14) io.AddMouseWheelEvent(0, 1000);
                        } else if (kind == "models-details" && frame > 0) {
                            const auto badge = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& item) { return item.chart == 52; });
                            if (badge != ui_actions.end()) io.AddMousePosEvent(badge->origin.x + 2, badge->origin.y + 2);
                        } else if (kind == "sessions-full-end") {
                            io.AddMousePosEvent(size.x/2,size.y/2);
                            if (frame > 0) io.AddMouseWheelEvent(0,-1000);
                        } else if (test_journal_input && (frame == 1 || frame == 2)) {
                            io.AddMousePosEvent(first_journal_action.origin.x + first_journal_action.size.x / 2,
                                first_journal_action.origin.y + first_journal_action.size.y / 2);
                            io.AddMouseButtonEvent(0, frame == 1);
                        } else if (test_journal_input && (frame == 5 || frame == 6)) {
                            const auto back = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                                return action.chart == back_from_journal_entry && action.enabled;
                            });
                            if (back == ui_actions.end()) throw std::runtime_error("journal Back is unreachable");
                            io.AddMousePosEvent(back->origin.x + back->size.x / 2, back->origin.y + back->size.y / 2);
                            io.AddMouseButtonEvent(0, frame == 5);
                        } else io.AddMousePosEvent(-100, -100);
                        ImGui_ImplMetal_NewFrame(pass);
                        ImGui::NewFrame();
                        float preferred = size.y;
                        auto frame_view = view;
                        if (kind == "overview-warning-short" && frame < 3)
                            frame_view.data["telemetry"]["project_states"] = Json::array();
                        try { preferred = render(frame_view, fit_test ? limit.y : 0); } catch (const std::exception& e) { throw std::runtime_error(kind + " " + std::to_string(static_cast<int>(size.x)) + ": " + e.what()); }
                        if (test_journal_input && (frame == 1 || frame == 5) && !ImGui::IsAnyItemActive())
                            throw std::runtime_error("synthetic mouse down did not reach the journal control");
                        if (kind == "overview-warning-short" && frame == 2 && preferred >= 400)
                            throw std::runtime_error("warning regression fixture did not establish a short baseline");
                        if (kind == "settings") for (const auto& action : ui_actions)
                            if (action.enabled && !action.informational) reachable_settings.insert(action.chart);
                        ImGui::Render();
                        auto command = [queue commandBuffer];
                        auto encoder = [command renderCommandEncoderWithDescriptor:pass];
                        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command, encoder);
                        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
                        if (command.status == MTLCommandBufferStatusError) throw std::runtime_error("Metal render failed");
                        if (fit_test && std::abs(preferred - size.y) > 1) {
                            if (frame + 1 == frames) throw std::runtime_error("automatic panel height did not settle: " + kind);
                            size.y = preferred;
                            descriptor.height = static_cast<NSUInteger>(size.y * 2);
                            texture = [device newTextureWithDescriptor:descriptor];
                        }
                    }
                    if (kind.starts_with("language-live-")) {
                        const auto expected = shin::ui::resolve_language(live_preference == 1 ? "en" : live_preference == 2 ? "ru" : "auto", platform_system_language());
                        const auto badge = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 50; });
                        if (shin::ui::language() != expected || ImGui::GetCurrentContext() != retained_context ||
                            selected != "fixture" || dashboard_view != 1 || badge == ui_actions.end() || badge->label != shin::ui::tr("Service connected"))
                            throw std::runtime_error("live switch lost ImGui state or retained an old accessibility label");
                    }
                    const auto context = kind + " " + std::to_string(static_cast<int>(size.x)) + "x" + std::to_string(static_cast<int>(size.y));
                    if (test_journal_input && (journal_entry || dashboard_view != 2 || activity_view != 2))
                        throw std::runtime_error("clicking Back did not return to the journal: " + context);
                    if (fit_test && kind == "overview" && (size.y <= 400 ||
                        std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 44;
                        }))) throw std::runtime_error("overview did not grow from a short page: " + context);
                    if (kind == "overview-warning-short" &&
                        (size.y > 420 || std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) { return action.chart == 121; })))
                        throw std::runtime_error("fitting hid the warning after a short page: " + context);
                    if (kind == "secret-blocked") {
                        const auto visible = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 54 && action.label == shin::ui::tr("Possible secret in sources");
                        });
                        if (visible == ui_actions.end() || diagnostic_report_text.find(shin::ui::language() == shin::ui::Language::ru ? "сработала проверка секретов" : "secret detection triggered") == std::string::npos)
                            throw std::runtime_error("secret blocker not visible: " + context);
                    }
                    if (kind == "blocked" || kind == "secret-blocked") {
                        const auto expected = kind == "blocked" ? 120 : 38;
                        const auto action = std::find_if(ui_actions.begin(), ui_actions.end(), [&](const auto& item) {
                            return item.chart == expected && item.enabled;
                        });
                        if (action == ui_actions.end()) throw std::runtime_error("recovery action missing: " + context);
                    }
                    if (kind == "route-review") {
                        const auto action = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& item) {
                            return item.chart == 70 && item.label == shin::ui::tr("Review changes");
                        });
                        if (action == ui_actions.end() || !action->enabled) throw std::runtime_error("profile review entry point missing: " + context);
                    }
                    if (kind.starts_with("profile-review")) {
                        const int expected = kind == "profile-review-stale" || kind == "profile-review-error" ? 73 :
                            kind == "profile-review-loading" || kind == "profile-review-waiting" || kind == "profile-review-failed" ? -1 :
                                kind == "profile-review-success" || kind == "profile-review-partial" ? 72 : 71;
                        if (expected >= 0) {
                            const auto action = std::find_if(ui_actions.begin(), ui_actions.end(), [expected](const auto& item) { return item.chart == expected; });
                            if (action == ui_actions.end()) throw std::runtime_error("profile review action missing: " + context);
                            if (kind == "profile-review-disabled" && action->enabled)
                                throw std::runtime_error("unacceptable profile was enabled: " + context);
                            if ((kind == "profile-review" || kind == "profile-review-disabled") &&
                                (!profile_accept_within_parent_clip || profile_accept_size.y < 29 ||
                                 profile_accept_origin.y + profile_accept_size.y > profile_accept_clip_max.y + .5f))
                                throw std::runtime_error("profile acceptance button is clipped by its parent: " + context +
                                    " button=" + std::to_string(profile_accept_origin.y) + "+" + std::to_string(profile_accept_size.y) +
                                    " clip=" + std::to_string(profile_accept_clip_min.y) + ".." + std::to_string(profile_accept_clip_max.y));
                        }
                        const auto status = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& item) { return item.chart == 74; });
                        if (status == ui_actions.end()) throw std::runtime_error("profile review status is not accessible: " + context);
                    }
                    if (kind == "redacted-lines" || kind == "profile-partial") {
                        const auto visible = std::find_if(ui_actions.begin(), ui_actions.end(), [&](const auto& action) {
                            return action.chart == 54 && action.label == shin::ui::tr(kind == "redacted-lines" ?
                                "Partial source coverage" : "Context partially available");
                        });
                        if (visible == ui_actions.end()) throw std::runtime_error("partial coverage not visible: " + context);
                        if (kind == "redacted-lines" && size.y >= 680 && std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 129 && action.label == "tests/auth_test.cpp · secret-like-source-content";
                        })) throw std::runtime_error("file with hidden lines is not visible: " + context);
                    }
                    if (kind == "first-run") {
                        const auto setup = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 85 && action.enabled;
                        });
                        if (setup == ui_actions.end()) throw std::runtime_error("fresh user cannot open setup without a service");
                    }
                    if (kind == "update") {
                        if (std::none_of(ui_actions.begin(),ui_actions.end(),[](const auto& a){return a.chart==86 && a.enabled;}))
                            throw std::runtime_error("release offer is not actionable");
                        if (std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 13 && action.label.find(shin::ui::tr("Update available")) != std::string::npos;
                        })) throw std::runtime_error("Settings does not disclose the update indication");
                    }
                    if (kind.starts_with("sessions") && size.y >= 440) {
                        const auto has = [&](const char* key) { return std::any_of(ui_actions.begin(),ui_actions.end(),[&](const auto& action) {
                            return action.label == shin::ui::tr(key);
                        }); };
                        if (kind != "sessions-details" && kind != "sessions-full-end" && !has("Shin requests"))
                            throw std::runtime_error("journal tabs are inaccessible: " + context);
                        if ((kind == "sessions-empty" || kind == "sessions-notes") && !has("No linked sessions yet"))
                            throw std::runtime_error("session empty state is missing: " + context);
                        if (kind == "sessions-unavailable" && (has("No linked sessions yet") || !has("Session activity is unavailable.")))
                            throw std::runtime_error("unavailable sessions look empty: " + context);
                        if (kind == "sessions-omitted" && (has("No linked sessions yet") || !has("Only part of the session history is available.")))
                            throw std::runtime_error("omitted sessions look empty: " + context);
                        if (kind == "sessions" && (std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                                return action.chart == open_journal_record && !action.target.empty();
                            }) ||
                            has("Caller-reported recovery and explicit host compaction are different events.")))
                            throw std::runtime_error("session details are not disclosed separately: " + context);
                        if (kind == "sessions-project" && std::count_if(journal_rows.begin(), journal_rows.end(), [](const auto& row) {
                            return row.has_value();
                        }) != 1) throw std::runtime_error("project journal did not filter the recorded calls: " + context);
                        if (kind == "sessions-notes" && size.y >= 620 && !has("Hide recording notes")) throw std::runtime_error("recording notes cannot be opened: " + context);
                        if (kind == "sessions-details" && session_details_open != std::string(64,'a')) throw std::runtime_error("session details cannot be opened: " + context);
                        if (kind == "sessions-full" && size.y >= 620) {
                            char limit[128]; std::snprintf(limit,sizeof(limit),shin::ui::tr("Shown: %zu events · limit %zu"),std::size_t{48},std::size_t{48});
                            if (!has(limit)) throw std::runtime_error("full journal does not disclose its bound: " + context);
                        }
                        if (kind == "sessions-full-end" && size.y >= 620 && !has("What is recorded?"))
                            {
                            std::string visible;
                            for (const auto& action : ui_actions) if (action.chart >= 90 && action.chart < 100)
                                visible += " [" + std::to_string(action.chart) + " " + action.label + "@" + std::to_string(action.origin.y) + "]";
                            throw std::runtime_error("full journal cannot scroll to its final controls: " + context + visible);
                        }
                    }
                    if (kind == "about" && size.y >= 440 && std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                        return action.chart == 87 && action.informational && action.label == SHIN_OBSERVER_VERSION;
                    })) throw std::runtime_error("installed version is not visible and accessible in About: " + context);
                    for (const auto& action : ui_actions) {
                        if (action.chart == 50) {
                            const auto about_item = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& item) { return item.chart == about; });
                            if (about_item == ui_actions.end() || std::abs(action.origin.x + action.size.x -
                                (about_item->origin.x + about_item->size.x)) > .5f)
                                throw std::runtime_error("service status is not aligned with the footer: " + context);
                        }
                        if (action.origin.x < 0 || action.origin.y < 0 || action.origin.x + action.size.x > size.x + 1 ||
                            action.origin.y + action.size.y > size.y + 1)
                            throw std::runtime_error("out-of-bounds control: " + context + " " + action.label);
                        if (action.chart >= 54 && action.chart <= 59 && action.origin.y + action.size.y > size.y - 39)
                            throw std::runtime_error("diagnostic text overlaps footer: " + context + " " + action.label);
                        if (timing_open && action.chart >= 60 && action.chart <= 69 && action.origin.y + action.size.y > size.y - 34)
                            throw std::runtime_error("timing content overlaps footer: " + context + " " + action.label);
                        if (action.chart >= 90 && !(action.chart == 110 && action.label == shin::ui::tr("Updates")) && action.origin.y + action.size.y > size.y - 39 + .5f)
                            throw std::runtime_error("session content overlaps footer: " + context + " " + action.label);
                    }
                    if (kind == "projects") {
                        const auto add = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 80; });
                        const auto request = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 53; });
                        const auto errors = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 37; });
                        if (add == ui_actions.end() || !add->enabled || request == ui_actions.end() || !request->informational || errors != ui_actions.end()) throw std::runtime_error("passive counters exposed an implicit action: " + context);
                        for (const auto& action : ui_actions)
                            if ((action.chart == 80 || action.chart == 53 || action.chart == 37 || action.chart == 30 || action.chart == 31) &&
                                action.origin.y + action.size.y > size.y - 39)
                                throw std::runtime_error("project controls clipped by footer: " + context + " " + action.label);
                    }
                    if (kind == "projects-last-page") {
                        const auto range = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 88; });
                        const auto last = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.project == "project-9"; });
                        const auto first = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.project == "fixture"; });
                        if (range == ui_actions.end() || !range->informational || !range->label.ends_with(" 10") ||
                            last == ui_actions.end() || first != ui_actions.end())
                            throw std::runtime_error("last project page hides its total or rows: " + context);
                    }
                    if (kind == "settings" && size.y >= 440) {
                        for (const int id : std::array<int, 6>{83, 112, choose_language, compact_view, update_settings, history_settings})
                            if (!reachable_settings.contains(id))
                                throw std::runtime_error("settings action unreachable: " + context + " " + std::to_string(id));
                    }
                    if (kind == "overview-warning" && size.y >= 440) {
                        const auto attention = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) { return action.chart == 121; });
                        if (attention == ui_actions.end()) throw std::runtime_error("overview attention action is missing: " + context);
                        activate(attention->chart, attention->target);
                        if (dashboard_view != 1 || selected != "fixture" || diagnostic_history_first)
                            throw std::runtime_error("overview attention action lost the affected project");
                    }
                    if (kind == "search-warning" && size.y >= 440) {
                        const auto action = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 120; });
                        if (action == ui_actions.end() || action->informational || !action->enabled)
                            throw std::runtime_error("search warning has no explicit next action");
                        activate(120);
                        if (!diagnostic_details_open || selected != "fixture") throw std::runtime_error("warning details lost their project");
                    }
                    if (kind == "search-warning-opened" &&
                        std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 124; }))
                        throw std::runtime_error("opening search checks did not reveal them: " + context);
                    if (kind == "search-warning-details" && size.y >= 620 &&
                        std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 126 && a.label.find("docs/access.md") != std::string::npos; }))
                        throw std::runtime_error("warning details omit their expected owner");
                    if (kind == "current") {
                        const auto actions = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == project_settings; });
                        if (actions == ui_actions.end() || !actions->enabled)
                            throw std::runtime_error("project actions menu missing: " + context);
                        const auto overlaps = [](const UiAction& a, const UiAction& b) {
                            return a.origin.x < b.origin.x + b.size.x && b.origin.x < a.origin.x + a.size.x &&
                                a.origin.y < b.origin.y + b.size.y && b.origin.y < a.origin.y + a.size.y;
                        };
                        for (const int chart : {34, 36, 38}) {
                            const auto other = std::find_if(ui_actions.begin(), ui_actions.end(), [chart](const auto& a) { return a.chart == chart; });
                            if (other != ui_actions.end() && overlaps(*actions, *other))
                                throw std::runtime_error("project actions overlap another footer control: " + context);
                        }
                    }
                    const auto width = texture.width, height = texture.height;
                    std::vector<unsigned char> pixels(width * height * 4);
                    [texture getBytes:pixels.data() bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
                    std::size_t surface_pixels = 0, text_pixels = 0;
                    const auto near_color = [](const unsigned char* pixel, ImVec4 color) {
                        return std::abs(pixel[0] - color.x * 255) < 20 &&
                            std::abs(pixel[1] - color.y * 255) < 20 &&
                            std::abs(pixel[2] - color.z * 255) < 20;
                    };
                    for (std::size_t i = 0; i < pixels.size(); i += 4) {
                        if (near_color(&pixels[i], background) || near_color(&pixels[i], raised)) ++surface_pixels;
                        if (near_color(&pixels[i], ink) || near_color(&pixels[i], muted)) ++text_pixels;
                    }
                    if (surface_pixels < 1000 || text_pixels < 300) throw std::runtime_error("blank rendering: " + context +
                        " surfaces=" + std::to_string(surface_pixels) + " text=" + std::to_string(text_pixels));
                    // The target is 2x: sample three logical points in, beyond
                    // the single stroke's antialiasing and inside the former rim.
                    if (std::abs(pixels[(height / 2 * width + 6) * 4 + 1] - background.y * 255) > 3)
                        throw std::runtime_error("dark outer rim returned around the Sand outline: " + context);
                    const auto filename = output / (kind + "-" + std::to_string(static_cast<int>(limit.x)) + "x" + std::to_string(static_cast<int>(limit.y)) + (fit_test ? "-fit.png" : ".png"));
                    auto color = CGColorSpaceCreateDeviceRGB();
                    auto provider = CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size(), nullptr);
                    auto image = CGImageCreate(width, height, 8, 32, width * 4, color,
                        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast), provider, nullptr, false, kCGRenderingIntentDefault);
                    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:filename.c_str()]];
                    auto destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
                    CGImageDestinationAddImage(destination, image, nullptr);
                    const bool written = CGImageDestinationFinalize(destination);
                    CFRelease(destination); CGImageRelease(image); CGDataProviderRelease(provider); CGColorSpaceRelease(color);
                    if (!written) throw std::runtime_error("PNG export failed");
                    ++rendered;
                }
            }
            selected.clear(); release_update_version.clear();
            history_hours = 24; activate(60);
            if (!timing_open || !timing_retained) throw std::runtime_error("timing navigation failed");
            activate(67); activate(65);
            if (timing_retained || timing_filter != 2) throw std::runtime_error("timing controls failed");
            activate(61);
            if (timing_open || history_hours != 24) throw std::runtime_error("timing return changed pulse period");
            select_project("fixture"); activate(34);
            if (selected != "fixture" || !diagnostic_history_first) throw std::runtime_error("history navigation failed");
            activate(35);
            if (selected != "fixture" || diagnostic_history_first) throw std::runtime_error("overview navigation failed");
            dashboard_view = 1;
            activate(project_settings);
            activate(confirm_disconnect);
            if (project_view != ProjectPage::disconnect || !back_from_panel_page() ||
                project_view != ProjectPage::settings || !back_from_panel_page() || project_view != ProjectPage::overview)
                throw std::runtime_error("project settings confirmation cannot be cancelled");
            activate(project_journal);
            if (dashboard_view != 2 || journal_project != "fixture" || activity_view != 2)
                throw std::runtime_error("project log lost its scope");
            activate(journal_back);
            if (dashboard_view != 1 || selected != "fixture") throw std::runtime_error("journal did not return to its project");
            activate(12);
            if (!journal_project.empty()) throw std::runtime_error("general journal retained a project filter");
            activate(journal_back);
            activate(32);
            if (!selected.empty()) throw std::runtime_error("project-list navigation failed");

            const auto first = project_disconnection.begin("a", "/fixture/a", "Atlas");
            if (!first || !project_disconnection.pending_for("a", "/fixture/a") ||
                project_disconnection.begin("b", "/fixture/b", "Boreal"))
                throw std::runtime_error("a second detach could replace the first request");
            selected = "b";
            dashboard_view = 1;
            project_view = ProjectPage::disconnect;
            complete_panel_disconnect(first->ticket, {{"ok", true}, {"service", {{"ok", false}, {"error", {{"code", "test-refusal"}}}}}});
            const auto* refusal = project_disconnection.result_for("a", "/fixture/a");
            if (!refusal || refusal->disconnected || refusal->detail != "test-refusal" ||
                project_disconnection.result_for("b", "/fixture/b") || selected != "b" || project_view != ProjectPage::disconnect)
                throw std::runtime_error("a detached project's refusal was routed to another project");
            const auto second = project_disconnection.begin("a", "/fixture/a", "Atlas");
            if (!second) throw std::runtime_error("a refusal prevented another attempt");
            complete_panel_disconnect(first->ticket, {{"ok", true}});
            if (!project_disconnection.pending_for("a", "/fixture/a") || project_disconnection.result())
                throw std::runtime_error("a stale detach ticket replaced the pending request");
            dashboard_view = 3;
            settings_view = SettingsPage::history;
            complete_panel_disconnect(second->ticket, {{"ok", true}});
            const auto* success = project_disconnection.result_for("a", "/fixture/a");
            if (!success || !success->disconnected || project_disconnection.result_for("a", "/fixture/moved") ||
                dashboard_view != 3 || settings_view != SettingsPage::history || selected != "b")
                throw std::runtime_error("a late detach replaced the current page");
            project_disconnection = {};
            ImGui_ImplMetal_Shutdown(); ImGui::DestroyContext();
            std::cout << rendered << " Metal renders and 4 native Storage renders: bounds, counter alignment and nonblank pixels passed\n";
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
