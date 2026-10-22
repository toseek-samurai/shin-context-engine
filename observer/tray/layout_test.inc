// Test-only entry point: real ImGui/Metal rendering, no IPC or user preferences.
#include <iostream>
#include "updates_window_test.inc"

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            if ((argc != 4 && argc != 5) || std::string(argv[2]) != "-ShinHome") throw std::runtime_error("output directory and test home required");
            const auto initial_language = shin::ui::language_for(argc == 5 ? argv[4] : "ru");
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
            if (live_text(static_cast<NSTextField*>(nil), @"stringValue", "Finding agents and profiles…") != nil)
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
                for (NSView* control : @[setupApp.setupChoose, setupApp.setupConnect, setupApp.setupCheck, setupApp.setupLogin, setupApp.setupStatus]) {
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
            for (const auto size : {ImVec2{432, 460}, ImVec2{360, 440}, ImVec2{280, 340}, ImVec2{280, 240}}) {
                for (const auto* scenario : {"first-run", "update", "cold", "current", "quarantined", "profile-partial", "blocked", "secret-blocked", "offline", "pending", "warming", "history", "unknown-history", "projects",
                     "projection-partial", "timing-live-partial", "timing-live", "timing-retained", "timing-missing", "timing-offline", "timing-empty", "timing-partial",
                     "overview", "language-live-en", "language-live-ru", "language-live-auto", "models", "models-partial", "models-stale", "models-details", "route-review", "profile-review", "profile-review-loading", "profile-review-error",
                     "profile-review-stale", "profile-review-waiting", "profile-review-success", "profile-review-partial", "profile-review-failed", "profile-review-disabled"}) {
                    Json project = base;
                    const std::string kind = scenario;
                    release_update_version=kind=="update" ? "2.0.5-dev.18" : "";
                    apply_language_preference(initial_language == shin::ui::Language::ru ? 2 : 1);

                    profile_review_open = kind.starts_with("profile-review-") || kind == "profile-review";
                    profile_review_root = "/fixture/workspace";
                    timing_open = kind.starts_with("timing-");
                    dashboard_view = timing_open || kind == "overview" ? 0 : kind.starts_with("models") ? 2 : 1;
                    timing_retained = kind != "timing-live" && kind != "timing-live-partial";
                    timing_filter = 0;
                    if (kind == "blocked") project.update({{"phase", "blocked"}, {"active_issue", true}, {"error_code", "refresh-validation-failed"}});
                    if (kind == "route-review") project.update({{"phase", "blocked"}, {"active_issue", true}, {"error_code", "route-revision-conflict"}});
                    if (kind == "profile-partial") project["source_coverage"] = {{"state", "partial"}, {"reason", "profile-revision-pending"}};
                    if (kind == "quarantined") project["source_coverage"] = {{"state", "partial"}, {"quarantined_files", 2}};
                    if (kind == "secret-blocked") project.update({{"phase", "blocked"}, {"active_issue", true},
                        {"error_code", "refresh-validation-failed"}, {"validation_diagnostic", {
                            {"schema_version", "shin.validation-diagnostic.v1"},
                            {"gates", Json::array({{{"id", "doctor"}, {"reasons", Json::array({"secret-like-source-content"})}}})}}}});
                    if (kind == "pending") project.update({{"phase", "checking"}, {"query_ready", false},
                        {"name", "Shin-Context-Engine-a-very-long-project-name"}});
                    if (kind == "warming") project.update({{"query_ready", false}, {"query_phase", "preparing"}});
                    ViewState view;
                    view.connected = kind != "first-run" && kind != "offline" && kind != "timing-offline";
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
                    if (kind == "projects") for (int i = 1; i < 9; ++i) {
                        auto other = base; other["id"] = "project-" + std::to_string(i);
                        other["name"] = "Project " + std::to_string(i); projects.push_back(other);
                    }
                    view.data = {{"version", "2.0.4"}, {"core_version", "2.0.4"},
                        {"registry_error", ""}, {"enrollment_errors", Json::object()}, {"query_workers", 0},
                        {"resident_projects", Json::array()}, {"telemetry", {{"project_states", projects},
                        {"history", {{"status", kind == "unknown-history" ? "unavailable" : "ready"}, {"rows", 14000},
                        {"projects", Json::array({{{"id", "fixture"}, {"requests", 1543}, {"errors", 5}, {"unobserved", 0}}})}}}}}};
                    if (kind == "projection-partial" || kind == "timing-live-partial")
                        view.data["telemetry"]["projection"] = {{"truncated", true}};
                    if (timing_open && kind != "timing-missing") {
                        auto& telemetry = view.data["telemetry"];
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
                    selected = kind == "projects" ? "" : "fixture";
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
                    for (int frame = 0; frame < 3; ++frame) {
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
                        if (kind == "models-details" && frame > 0) {
                            const auto badge = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& item) { return item.chart == 52; });
                            if (badge != ui_actions.end()) io.AddMousePosEvent(badge->origin.x + 2, badge->origin.y + 2);
                        } else io.AddMousePosEvent(-100, -100);
                        ImGui_ImplMetal_NewFrame(pass);
                        ImGui::NewFrame(); render(view); ImGui::Render();
                        auto command = [queue commandBuffer];
                        auto encoder = [command renderCommandEncoderWithDescriptor:pass];
                        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command, encoder);
                        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
                        if (command.status == MTLCommandBufferStatusError) throw std::runtime_error("Metal render failed");
                    }
                    if (kind.starts_with("language-live-")) {
                        const auto expected = shin::ui::resolve_language(live_preference == 1 ? "en" : live_preference == 2 ? "ru" : "auto", platform_system_language());
                        const auto badge = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 50; });
                        if (shin::ui::language() != expected || ImGui::GetCurrentContext() != retained_context ||
                            selected != "fixture" || dashboard_view != 1 || badge == ui_actions.end() || badge->label != shin::ui::tr("Service connected"))
                            throw std::runtime_error("live switch lost ImGui state or retained an old accessibility label");
                    }
                    const auto context = kind + " " + std::to_string(static_cast<int>(size.x)) + "x" + std::to_string(static_cast<int>(size.y));
                    if (kind == "secret-blocked") {
                        const auto visible = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 54 && action.label == shin::ui::tr("Possible secret in sources");
                        });
                        if (visible == ui_actions.end() || diagnostic_report_text.find(shin::ui::language() == shin::ui::Language::ru ? "сработала проверка секретов" : "secret detection triggered") == std::string::npos)
                            throw std::runtime_error("secret blocker not visible: " + context);
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
                    if (kind == "quarantined" || kind == "profile-partial") {
                        const auto visible = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 54 && action.label == shin::ui::tr("Context partially available");
                        });
                        if (visible == ui_actions.end()) throw std::runtime_error("partial coverage not visible: " + context);
                    }
                    if (kind == "first-run") {
                        const auto setup = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                            return action.chart == 85 && action.enabled;
                        });
                        if (setup == ui_actions.end()) throw std::runtime_error("fresh user cannot open setup without a service");
                    }
                    if(kind=="update" && std::none_of(ui_actions.begin(),ui_actions.end(),[](const auto& a){return a.chart==86 && a.enabled;}))
                        throw std::runtime_error("release offer is not actionable");
                    if (std::none_of(ui_actions.begin(), ui_actions.end(), [](const auto& action) {
                        return action.chart == 87 && action.informational && action.label == "v" SHIN_OBSERVER_VERSION;
                    })) throw std::runtime_error("installed version is not visible and accessible: " + context);
                    for (const auto& action : ui_actions) {
                        if (action.chart == 50 && std::abs(action.origin.x + action.size.x -
                            (size.x - ImGui::GetStyle().WindowPadding.x)) > .5f)
                            throw std::runtime_error("service status is not right-aligned: " + context);
                        if (action.origin.x < 0 || action.origin.y < 0 || action.origin.x + action.size.x > size.x + 1 ||
                            action.origin.y + action.size.y > size.y + 1)
                            throw std::runtime_error("out-of-bounds control: " + context + " " + action.label);
                        if (action.chart >= 54 && action.chart <= 59 && action.origin.y + action.size.y > size.y - 75)
                            throw std::runtime_error("diagnostic text overlaps footer: " + context + " " + action.label);
                        if (timing_open && action.chart >= 60 && action.chart <= 69 && action.origin.y + action.size.y > size.y - 34)
                            throw std::runtime_error("timing content overlaps footer: " + context + " " + action.label);
                    }
                    if (kind == "projects") {
                        const auto add = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 80; });
                        const auto request = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 53; });
                        const auto errors = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 37; });
                        if (add == ui_actions.end() || !add->enabled || request == ui_actions.end() || errors == ui_actions.end() || std::abs(request->origin.y - errors->origin.y) > .5f ||
                            request->size.y != errors->size.y) throw std::runtime_error("counter baseline mismatch: " + context);
                    }
                    if (kind == "current") {
                        const auto actions = std::find_if(ui_actions.begin(), ui_actions.end(), [](const auto& a) { return a.chart == 84; });
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
                    std::size_t bright = 0;
                    for (std::size_t i = 0; i < pixels.size(); i += 4) if (pixels[i] > 100 && pixels[i + 1] > 100) ++bright;
                    if (bright < 1000) throw std::runtime_error("blank rendering: " + context);
                    const auto filename = output / (kind + "-" + std::to_string(static_cast<int>(size.x)) + "x" + std::to_string(static_cast<int>(size.y)) + ".png");
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
            activate(32);
            if (!selected.empty()) throw std::runtime_error("project-list navigation failed");
            ImGui_ImplMetal_Shutdown(); ImGui::DestroyContext();
            std::cout << rendered << " Metal renders and 4 native Storage renders: bounds, counter alignment and nonblank pixels passed\n";
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
