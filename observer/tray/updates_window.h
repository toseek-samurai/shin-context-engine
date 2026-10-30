// Included in ShinApp: network and install run in the native CLI, never AppKit.
// One retained window owns the complete review. Background replies stay quiet;
// a failed explicit installation returns the still-open review to the foreground.
- (void)ensureUpdateWindow {
    if (self.updateWindow) return;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 300)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO; window.restorable = NO;
    window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
    live_text(window, @"title", "Shin updates");
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window.backgroundColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
    window.contentView.wantsLayer = YES;
    window.contentView.layer.backgroundColor = window.backgroundColor.CGColor;
    NSStackView* stack = [NSStackView new];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading; stack.spacing = 12;
    stack.distribution = NSStackViewDistributionFill; stack.detachesHiddenViews = YES;
    self.updateLayout = stack;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:22],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:window.contentView.bottomAnchor constant:-22]]];
    NSColor* sand = [NSColor colorWithCalibratedRed:.84 green:.74 blue:.57 alpha:1];
    NSImageView* icon = [NSImageView new];
    NSImage* logo = [NSBundle.mainBundle imageForResource:@"shin-logo"];
    icon.image = logo ? logo : [NSImage imageWithSystemSymbolName:@"arrow.down.circle.fill" accessibilityDescription:nil];
    icon.contentTintColor = sand; icon.imageScaling = NSImageScaleProportionallyDown;
    [icon.widthAnchor constraintEqualToConstant:30].active = YES;
    [icon.heightAnchor constraintEqualToConstant:30].active = YES;
    NSTextField* brand = [NSTextField labelWithString:@"SHIN"];
    brand.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold]; brand.textColor = sand;
    NSTextField* channel = [NSTextField labelWithString:@""]; live_text(channel, @"stringValue", "Preview channel");
    channel.font = [NSFont systemFontOfSize:11]; channel.textColor = NSColor.secondaryLabelColor;
    NSStackView* identity = [NSStackView stackViewWithViews:@[brand, channel]];
    identity.orientation = NSUserInterfaceLayoutOrientationVertical; identity.alignment = NSLayoutAttributeLeading; identity.spacing = 2;
    NSStackView* header = [NSStackView stackViewWithViews:@[icon, identity]]; header.spacing = 10;
    self.updateTitle = [NSTextField wrappingLabelWithString:@""];
    self.updateTitle.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    self.updateTitle.textColor = [NSColor colorWithCalibratedRed:.914 green:.914 blue:.867 alpha:1];
    self.updateVersion = [NSTextField wrappingLabelWithString:@""];
    self.updateVersion.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium];
    self.updateVersion.textColor = sand;
    self.updateDetail = [NSTextField wrappingLabelWithString:@""];
    self.updateDetail.font = [NSFont systemFontOfSize:13]; self.updateDetail.selectable = YES;
    self.updateDetail.textColor = [NSColor colorWithCalibratedRed:.76 green:.78 blue:.72 alpha:1];
    // Diagnostics may be long. Keep them selectable and scrollable without pushing
    // the actions outside the window (full JSON remains available via Copy).
    NSScrollView* details = [NSScrollView new]; details.drawsBackground = NO;
    details.hasVerticalScroller = YES; details.autohidesScrollers = YES;
    self.updateDetails = details;
    self.updateDetail.translatesAutoresizingMaskIntoConstraints = NO;
    details.documentView = self.updateDetail;
    [self.updateDetail.widthAnchor constraintEqualToAnchor:details.contentView.widthAnchor].active = YES;
    self.updateDetailHeight = [details.heightAnchor constraintEqualToConstant:48]; self.updateDetailHeight.active = YES;
    self.updateSpinner = [NSProgressIndicator new];
    self.updateSpinner.style = NSProgressIndicatorStyleBar; self.updateSpinner.indeterminate = YES;
    self.updateSpinner.displayedWhenStopped = NO;
    self.updateCheckButton = live_button("Check for updates…", self, @selector(checkForUpdates:));
    self.updateInstallButton = live_button("Download and install", self, @selector(installReviewedUpdate:));
    self.updateNotesButton = live_button("Release notes", self, @selector(openUpdateReleaseNotes:));
    self.updateCopyButton = live_button("Copy diagnostic report", self, @selector(copyUpdateResult:));
    self.updateAcknowledgeButton = live_button("Acknowledge verified result", self, @selector(acknowledgeUpdateResult:));
    self.updateReviewButton = live_button("Review previous update", self, @selector(showLastUpdateResult:));
    for (NSButton* button in @[self.updateInstallButton, self.updateAcknowledgeButton]) {
        button.bezelColor = sand; button.contentTintColor = [NSColor colorWithCalibratedRed:.16 green:.17 blue:.14 alpha:1];
        button.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    }
    for (NSButton* button in @[self.updateNotesButton, self.updateCopyButton]) {
        button.bordered = NO; button.bezelStyle = NSBezelStyleInline; button.contentTintColor = sand;
        button.font = [NSFont systemFontOfSize:12];
    }
    NSView* spacer = [NSView new]; [spacer.widthAnchor constraintGreaterThanOrEqualToConstant:8].active = YES;
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* actions = [NSStackView stackViewWithViews:@[self.updateNotesButton, self.updateCopyButton, spacer, self.updateCheckButton, self.updateInstallButton]];
    actions.spacing = 10; actions.distribution = NSStackViewDistributionFill; actions.detachesHiddenViews = YES;
    NSBox* separator = [NSBox new]; separator.boxType = NSBoxSeparator;
    for (NSView* view in @[header, self.updateTitle, self.updateVersion, details, self.updateSpinner, self.updateAcknowledgeButton, self.updateReviewButton, separator, actions])
        [stack addArrangedSubview:view];
    for (NSView* view in @[self.updateTitle, self.updateVersion, details, self.updateSpinner, separator, actions])
        [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    // Installing is deliberate; Return must never start it just by opening review.
    self.updateCheckButton.keyEquivalent = @"";
    self.updateWindow = window;
    [self renderUpdateWindow];
    [window center];
}
- (void)showUpdateWindow {
    [self closePanel:nil];
    [self ensureUpdateWindow];
    if (self.updateWindow.miniaturized) [self.updateWindow deminiaturize:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self.updateWindow makeKeyAndOrderFront:nil];
}
- (void)renderUpdateWindow {
    if (!self.updateWindow) return;
    const bool checking = self.updateScreen == UpdateScreen::checking;
    const bool starting = self.updateScreen == UpdateScreen::starting;
    const bool loading = self.updateScreen == UpdateScreen::resultLoading;
    const bool acknowledging = self.updateScreen == UpdateScreen::acknowledging;
    const bool offer = self.updateScreen == UpdateScreen::offer;
    const bool result = self.updateScreen == UpdateScreen::resultReady || self.updateScreen == UpdateScreen::acknowledgeFailed;
    const bool installed = _updateResult.is_object() && _updateResult.value("phase", "") == "installed" && _updateResult.value("ok", false);
    self.updateReviewButton.hidden = self.updateScreen != UpdateScreen::startFailed ||
        cli_error_code(_updateResult) != "update-outcome-unknown";
    self.updateCheckButton.enabled = !checking && !loading && !self.updateActionBusy;
    self.updateInstallButton.hidden = !offer && !starting;
    self.updateInstallButton.enabled = offer && !self.updateActionBusy;
    self.updateNotesButton.hidden = !offer;
    self.updateCopyButton.hidden = !result && self.updateScreen != UpdateScreen::startFailed;
    self.updateAcknowledgeButton.hidden = !result || installed || !_updateResult.value("can_acknowledge", false);
    self.updateAcknowledgeButton.enabled = !self.updateActionBusy;
    self.updateCheckButton.title = ui_text(offer || self.updateScreen == UpdateScreen::startFailed ? "Check again" : "Check for updates…");
    for (NSButton* button in @[self.updateInstallButton, self.updateAcknowledgeButton])
        button.attributedTitle = [[NSAttributedString alloc] initWithString:button.title attributes:@{
            NSFontAttributeName:button.font, NSForegroundColorAttributeName:[NSColor colorWithCalibratedRed:.16 green:.17 blue:.14 alpha:1]}];
    self.updateSpinner.hidden = !(checking || starting || loading || acknowledging);
    if (self.updateSpinner.hidden) [self.updateSpinner stopAnimation:nil];
    else [self.updateSpinner startAnimation:nil];
    NSString* title = ui_text("Shin updates");
    NSString* detail = @"";
    switch (self.updateScreen) {
    case UpdateScreen::checking:
        title = ui_text("Checking for updates…");
        detail = ui_text("Shin continues to work while the release is checked. You can leave this window open or close it."); break;
    case UpdateScreen::offer:
        title = ui_text("A Shin update is available");
        detail = ui_text("Monitor will close while the updater installs the new version, then reopen. Projects, settings and history stay in place. Reconnect persistent MCP clients afterwards."); break;
    case UpdateScreen::current: title = ui_text("Shin is up to date"); break;
    case UpdateScreen::checkFailed:
        title = ui_text("Could not check for updates");
        detail = ui_text("GitHub is unavailable, the release is incomplete, or this is not an owned installation. Shin continues to work offline. Try again later."); break;
    case UpdateScreen::starting:
        title = ui_text("Opening update progress…");
        detail = ui_text("The update helper is starting. This window stays open until the progress window takes over."); break;
    case UpdateScreen::startFailed: title = ui_text("Update was not started"); detail = update_failure_text(_updateResult, self.updateFailure); break;
    case UpdateScreen::resultLoading:
        title = ui_text("Reading update result…"); break;
    case UpdateScreen::acknowledging:
        title = ui_text("Verifying update result…"); break;
    case UpdateScreen::acknowledgeFailed:
        title = ui_text("Update needs review"); detail = self.updateFailure; break;
    case UpdateScreen::resultReady:
        if (_updateResult.value("phase", "") == "idle" || _updateResult.value("phase", "") == "reviewed") {
            title = ui_text("No pending update result"); self.updateCopyButton.hidden = YES;
        } else if (_updateResult.value("phase", "") == "failed" && _updateResult.contains("error")) {
            title = ui_text("Update was not started"); detail = update_failure_text(_updateResult, nil);
        } else {
            title = installed ? ui_text("Shin was updated") : ui_text("Update needs review");
            detail = installed ? ui_text("The installed kit was verified. Reconnect persistent MCP clients to use the new version.") :
                _updateResult.value("can_acknowledge", false) && _updateResult.value("installation_verified", false) ?
                ui_text("The current installation is verified and has no pending recovery. Confirm this saved result to clear the previous attempt and allow another update. Project data stays in place.") :
                ui_text("The update did not confirm completion. The saved result includes the candidate location and native recovery outcome. Do not repeat installation before reviewing it.");
        }
        break;
    case UpdateScreen::idle: break;
    }
    self.updateTitle.stringValue = title;
    const bool reviewed = offer || starting || self.updateScreen == UpdateScreen::startFailed;
    self.updateVersion.hidden = !reviewed || !_reviewedUpdateOffer.contains("version");
    if (result && _updateResult.contains("current_version")) {
        self.updateVersion.hidden = NO;
        self.updateVersion.stringValue = [NSString stringWithFormat:ui_text("Installed version: %@"),
            diagnostic_text_content(_updateResult.value("current_version", ""))];
    }
    else if (!self.updateVersion.hidden) self.updateVersion.stringValue = [NSString stringWithFormat:@"%@  →  %@",
        diagnostic_text_content(_reviewedUpdateOffer.value("current_version", std::string(SHIN_OBSERVER_VERSION))),
        diagnostic_text_content(_reviewedUpdateOffer.value("version", ""))];
    self.updateDetails.hidden = !detail.length;
    NSMutableParagraphStyle* paragraph = [NSMutableParagraphStyle new]; paragraph.lineSpacing = 3;
    self.updateDetail.attributedStringValue = [[NSAttributedString alloc] initWithString:detail ? detail : @""
        attributes:@{NSFontAttributeName:self.updateDetail.font, NSForegroundColorAttributeName:self.updateDetail.textColor,
            NSParagraphStyleAttributeName:paragraph}];
    const CGFloat width = self.updateWindow.contentView.bounds.size.width - 48;
    const auto textBounds = [self.updateDetail.attributedStringValue boundingRectWithSize:NSMakeSize(width, CGFLOAT_MAX)
        options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading];
    self.updateDetailHeight.constant = std::clamp(std::ceil(textBounds.size.height) + 6.0, 22.0, 112.0);
    [self.updateWindow.contentView layoutSubtreeIfNeeded];
    const CGFloat height = std::clamp(std::ceil(self.updateLayout.fittingSize.height) + 44.0, 210.0, 440.0);
    const NSRect before = self.updateWindow.frame;
    NSRect frame = [self.updateWindow frameRectForContentRect:NSMakeRect(0, 0, 560, height)];
    frame.origin = NSMakePoint(before.origin.x, NSMaxY(before) - frame.size.height);
    [self.updateWindow setFrame:frame display:self.updateWindow.visible];
    [self.updateWindow.contentView layoutSubtreeIfNeeded];
    [self.updateDetail.enclosingScrollView.contentView scrollToPoint:NSZeroPoint];
}
- (void)showUpdateOffer:(id)sender { (void)sender; [self beginUpdateCheck:YES]; }
- (void)presentUpdateOffer {
    if (!_updateOffer.value("available", false)) return;
    _reviewedUpdateOffer = _updateOffer; // Pin the exact visible review, even if a background check finishes.
    self.updateScreen = UpdateScreen::offer;
    [self renderUpdateWindow];
}
- (void)openUpdateReleaseURL:(NSURL*)url { [NSWorkspace.sharedWorkspace openURL:url]; }
- (void)openUpdateReleaseNotes:(id)sender {
    (void)sender;
    if (self.updateScreen != UpdateScreen::offer) return;
    NSURL* url = [NSURL URLWithString:diagnostic_text_content(_reviewedUpdateOffer.value("release_url", ""))];
    if (url) [self openUpdateReleaseURL:url]; // Browser activation is intentional; keep the review alive.
}
- (void)installReviewedUpdate:(id)sender {
    (void)sender;
    if (self.updateScreen != UpdateScreen::offer || self.updateActionBusy) return;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) {
        _updateResult = {{"ok", false}, {"error", {{"code", "ui-operation-busy"}}}};
        self.updateScreen = UpdateScreen::startFailed;
        self.updateFailure = nil;
        [self renderUpdateWindow]; return;
    }
    const auto offered = _reviewedUpdateOffer;
    _updateResult = Json::object();
    self.updateActionBusy = YES; self.updateScreen = UpdateScreen::starting;
    [self renderUpdateWindow];
    [self setUpdatePending:YES];
    [self runCLI:@"update-install" arguments:@[@"--ui-version", diagnostic_text_content(offered.at("version").get<std::string>()), @"--revision", diagnostic_text_content(offered.at("revision").get<std::string>()), @"--wait-pid", [NSString stringWithFormat:@"%d", getpid()], @"--ui-language", shin::ui::language() == shin::ui::Language::ru ? @"ru" : @"en"] completion:^(Json result, NSString* failure) {
        release_project_operation(); self.updateActionBusy = NO;
        [self setUpdatePending:NO];
        const auto error = result.is_object() ? result.value("error", Json()) : Json();
        if (error.is_object() && error.value("code", Json()) == "update-offer-changed") {
            _updateOffer = Json::object(); release_update_version.clear(); self.lastUpdateCheck = nil;
            self.nextUpdateCheck = NSDate.date;
        }
        self.updateScreen = UpdateScreen::startFailed;
        _updateResult = result.is_object() ? result : Json::object();
        if (failure.length && _updateResult.empty()) _updateResult = {{"ok", false}, {"error", {{"message", failure.UTF8String}}}};
        self.updateFailure = failure;
        [self renderUpdateWindow];
        // The short-lived progress viewer can have owned focus before exiting.
        // Return the explicit operation's failure, but respect dismissal/minimize.
        if (self.updateWindow.visible && !self.updateWindow.miniaturized) [self showUpdateWindow];
    }];
}
- (void)checkForUpdates:(id)sender { (void)sender; [self beginUpdateCheck:YES]; }
- (void)beginUpdateCheck:(BOOL)manual {
    if (manual) {
        [self showUpdateWindow];
        if (self.updateActionBusy) return;
        self.updateScreen = UpdateScreen::checking;
        self.updateCheckEpoch = ++self.updateWindowEpoch;
        [self renderUpdateWindow];
    }
    if (self.updateChecking || self.updateActionBusy) return;
    // Explicit review always fetches the current offer. A manual request during
    // a background check promotes that same request into this visible review.
    if (!manual && self.lastUpdateCheck && -self.lastUpdateCheck.timeIntervalSinceNow < 60) return;
    if (!manual) self.updateCheckEpoch = 0;
    self.updateChecking = YES; self.lastUpdateCheck = NSDate.date;
    self.nextUpdateCheck = [NSDate dateWithTimeIntervalSinceNow:6 * 3600];
    [self runCLI:@"update-check" arguments:@[] completion:^(Json result, NSString* failure) {
        self.updateChecking = NO;
        const bool visibleReview = self.updateCheckEpoch && self.updateCheckEpoch == self.updateWindowEpoch;
        self.updateCheckEpoch = 0;
        if (!failure && result.value("ok", false)) {
            _updateOffer = result; release_update_version = result.value("version", "");
            if (visibleReview) {
                if (result.value("available", false)) [self presentUpdateOffer];
                else { self.updateScreen = UpdateScreen::current; [self renderUpdateWindow]; }
            }
        } else if (visibleReview) { self.updateScreen = UpdateScreen::checkFailed; [self renderUpdateWindow]; }
    }];
}
- (void)toggleAutomaticUpdates:(id)sender {
    (void)sender;
    const BOOL disabled = [[NSUserDefaults standardUserDefaults] boolForKey:@"ShinAutomaticUpdatesDisabled"];
    [[NSUserDefaults standardUserDefaults] setBool:!disabled forKey:@"ShinAutomaticUpdatesDisabled"];
    if (disabled) self.nextUpdateCheck = NSDate.date;
}
- (void)showLastUpdateResult:(id)sender { (void)sender; [self showUpdateResult]; }
- (void)showUpdateResult {
    [self showUpdateWindow];
    if (self.updateActionBusy || self.updateScreen == UpdateScreen::resultLoading) return;
    [self setUpdatePending:NO];
    const auto epoch = ++self.updateWindowEpoch;
    self.updateScreen = UpdateScreen::resultLoading; [self renderUpdateWindow];
    [self runCLI:@"update-status" arguments:@[] completion:^(Json result, NSString* failure) {
        if (epoch != self.updateWindowEpoch) return;
        _updateResult = result;
        self.updateScreen = UpdateScreen::resultReady;
        if (failure) _updateResult = {{"ok", false}, {"error", failure.UTF8String ? failure.UTF8String : ""}};
        [self renderUpdateWindow];
    }];
}
- (void)copyUpdateResult:(id)sender {
    (void)sender;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:diagnostic_text_content(_updateResult.dump(2)) forType:NSPasteboardTypeString];
    // Leave result and recovery actions visible after copying.
}
- (void)acknowledgeUpdateResult:(id)sender {
    (void)sender;
    if (self.updateActionBusy || (self.updateScreen != UpdateScreen::resultReady && self.updateScreen != UpdateScreen::acknowledgeFailed) ||
        !_updateResult.value("can_acknowledge", false)) return;
    self.updateActionBusy = YES; self.updateScreen = UpdateScreen::acknowledging;
    [self renderUpdateWindow];
    [self runCLI:@"update-acknowledge" arguments:@[@"--revision", diagnostic_text_content(_updateResult.value("review_revision", ""))] completion:^(Json acknowledged, NSString* problem) {
        self.updateActionBusy = NO;
        if (problem || !acknowledged.value("ok", false)) {
            self.updateScreen = UpdateScreen::acknowledgeFailed;
            self.updateFailure = update_failure_text(acknowledged, problem);
        } else { _updateResult = acknowledged; self.updateScreen = UpdateScreen::resultReady; }
        [self renderUpdateWindow];
        if (self.updateScreen == UpdateScreen::acknowledgeFailed && self.updateWindow.visible && !self.updateWindow.miniaturized)
            [self showUpdateWindow];
    }];
}
- (void)setUpdatePending:(BOOL)pending {
    if (pending) [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"ShinUpdatePending"];
    else [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"ShinUpdatePending"];
}
