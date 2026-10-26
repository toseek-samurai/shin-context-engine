// Exercise the real update actions without GitHub, dialogs, installation or preferences.
@interface UpdateOfferHarness : ShinApp
@property(nonatomic) NSUInteger checks;
@property(nonatomic) NSUInteger presentations;
@property(nonatomic, copy) NSString* presentedVersion;
@property(nonatomic, copy) void (^pendingCheck)(Json, NSString*);
- (void)seedOldOffer;
- (void)presentUpdateOffer;
@end

@implementation UpdateOfferHarness
- (void)showUpdateWindow {} // Action-only tests; native window/focus has its own harness.
- (void)seedOldOffer {
    _updateOffer = {{"ok", true}, {"available", true}, {"version", "2.0.5-dev.21"}};
    self.lastUpdateCheck = NSDate.date;
}
- (void)runCLI:(NSString*)command arguments:(NSArray<NSString*>*)arguments completion:(void (^)(Json, NSString*))completion {
    if (![command isEqualToString:@"update-check"] || arguments.count || self.pendingCheck)
        throw std::runtime_error("update action launched an installer or duplicated a check");
    ++self.checks;
    self.pendingCheck = completion;
}
- (void)presentUpdateOffer {
    ++self.presentations;
    self.presentedVersion = diagnostic_text_content(_updateOffer.at("version").get<std::string>());
}
@end

// Capture the old manual-check cache shortcut before it can open a real dialog.
@interface ManualUpdateOfferHarness : UpdateOfferHarness
@end
@implementation ManualUpdateOfferHarness
- (void)showUpdateOffer:(id)sender { (void)sender; [self presentUpdateOffer]; }
@end

void check_update_offer_actions() {
    const auto prior_version = release_update_version;
    auto require = [](bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); };
    ManualUpdateOfferHarness* manual = [ManualUpdateOfferHarness new];
    [manual seedOldOffer];
    [manual checkForUpdates:nil];
    require(manual.checks == 1 && manual.presentations == 0 && manual.pendingCheck,
            "manual update check reused a stale offer instead of querying GitHub");
    [manual checkForUpdates:nil];
    require(manual.checks == 1, "manual update checks ran concurrently");
    auto manual_reply = manual.pendingCheck;
    manual.pendingCheck = nil;
    manual_reply({{"ok", true}, {"available", true}, {"version", "2.0.5-dev.24"}}, nil);
    require(manual.presentations == 1 && [manual.presentedVersion isEqualToString:@"2.0.5-dev.24"],
            "manual update check did not present the fresh offer");

    UpdateOfferHarness* menu = [UpdateOfferHarness new];
    [menu seedOldOffer];
    [menu showUpdateOffer:nil];
    require(menu.checks == 1 && menu.presentations == 0 && menu.pendingCheck,
            "update menu presented a cached offer before checking");
    auto menu_reply = menu.pendingCheck;
    menu.pendingCheck = nil;
    menu_reply({{"ok", true}, {"available", true}, {"version", "2.0.5-dev.24"}}, nil);
    require(menu.presentations == 1 && [menu.presentedVersion isEqualToString:@"2.0.5-dev.24"],
            "update menu did not present the fresh offer");

    UpdateOfferHarness* automatic = [UpdateOfferHarness new];
    [automatic seedOldOffer];
    [automatic beginUpdateCheck:NO];
    require(automatic.checks == 0 && automatic.presentations == 0,
            "automatic checks lost their coalescing interval");

    const Json changed{{"error", {{"code", "update-offer-changed"}, {"message", "old diagnostic"}}}};
    NSString* expected_changed = shin::ui::language() == shin::ui::Language::ru
        ? @"Предложение обновления изменилось. Перед установкой проверьте обновления снова."
        : @"The release offer changed. Check for updates again before installing.";
    require([update_failure_text(changed, nil) isEqualToString:expected_changed],
            "changed-offer refusal lost its localized recovery instruction");
    require([update_failure_text({{"error", {{"code", "update-network-unavailable"}, {"message", "native failure detail"}}}}, nil)
                isEqualToString:@"native failure detail"], "native update error was hidden by the generic alert");
    require([update_failure_text(changed, @"transport failure") isEqualToString:@"transport failure"],
            "transport failure was hidden by a native diagnostic");
    for (const auto& malformed : {Json(), Json::object(), Json{{"error", 7}}, Json{{"error", {{"message", 3}}}}})
        require([update_failure_text(malformed, nil) isEqualToString:ui_text("The release or installation could not be confirmed. Check again before retrying; the current kit is preserved.")],
                "missing update diagnostics did not retain the safe fallback");
    release_update_version = prior_version;
}
