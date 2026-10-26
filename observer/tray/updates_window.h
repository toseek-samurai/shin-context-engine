// Included in ShinApp: network and install run in the native CLI, never AppKit.
// One retained window owns the complete review. Async replies change its content,
// never its visibility, ordering or activation: only an explicit user action does.
- (void)ensureUpdateWindow {
    if (self.updateWindow) return;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 580, 400)
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
    stack.alignment = NSLayoutAttributeLeading; stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:24],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:window.contentView.bottomAnchor constant:-24]]];
    self.updateTitle = [NSTextField wrappingLabelWithString:@""];
    self.updateTitle.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    self.updateTitle.textColor = [NSColor colorWithCalibratedRed:.914 green:.914 blue:.867 alpha:1];
    self.updateDetail = [NSTextField wrappingLabelWithString:@""];
    self.updateDetail.font = [NSFont systemFontOfSize:13]; self.updateDetail.selectable = YES;
    self.updateDetail.textColor = [NSColor colorWithCalibratedRed:.76 green:.78 blue:.72 alpha:1];
    // Diagnostics may be long. Keep them selectable and scrollable without pushing
    // the actions outside the window (full JSON remains available via Copy).
    NSScrollView* details = [NSScrollView new]; details.drawsBackground = NO;
    details.hasVerticalScroller = YES; details.autohidesScrollers = YES;
    self.updateDetail.translatesAutoresizingMaskIntoConstraints = NO;
    details.documentView = self.updateDetail;
    [self.updateDetail.widthAnchor constraintEqualToAnchor:details.contentView.widthAnchor].active = YES;
    [details.heightAnchor constraintEqualToConstant:128].active = YES;
    self.updateSpinner = [NSProgressIndicator new];
    self.updateSpinner.style = NSProgressIndicatorStyleBar; self.updateSpinner.indeterminate = YES;
    self.updateSpinner.displayedWhenStopped = NO;
    self.updateCheckButton = live_button("Check for updates…", self, @selector(checkForUpdates:));
    self.updateInstallButton = live_button("Download and install", self, @selector(installReviewedUpdate:));
    self.updateNotesButton = live_button("Release notes", self, @selector(openUpdateReleaseNotes:));
    self.updateCopyButton = live_button("Copy diagnostic report", self, @selector(copyUpdateResult:));
    self.updateAcknowledgeButton = live_button("Acknowledge verified result", self, @selector(acknowledgeUpdateResult:));
    NSStackView* primary = [NSStackView stackViewWithViews:@[self.updateCheckButton, self.updateInstallButton]];
    NSStackView* secondary = [NSStackView stackViewWithViews:@[self.updateNotesButton, self.updateCopyButton]];
    primary.spacing = 12; secondary.spacing = 12;
    for (NSView* view in @[self.updateTitle, details, self.updateSpinner, primary, secondary, self.updateAcknowledgeButton])
        [stack addArrangedSubview:view];
    for (NSView* view in @[self.updateTitle, details, self.updateSpinner])
        [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    // Installing is deliberate; Return must never start it just by opening review.
    self.updateCheckButton.keyEquivalent = @"\r";
    self.updateWindow = window; [window center];
    [self renderUpdateWindow];
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
    self.updateCheckButton.enabled = !checking && !loading && !self.updateActionBusy;
    self.updateInstallButton.hidden = !offer && !starting;
    self.updateInstallButton.enabled = offer && !self.updateActionBusy;
    self.updateNotesButton.hidden = !offer;
    self.updateCopyButton.hidden = !result;
    self.updateAcknowledgeButton.hidden = !result || installed || !_updateResult.value("can_acknowledge", false);
    self.updateAcknowledgeButton.enabled = !self.updateActionBusy;
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
        detail = [NSString stringWithFormat:ui_text("Version %@ is available. An update window will show progress while Monitor closes and the kit is replaced. Projects, settings and history stay in place. Reconnect persistent MCP clients afterwards."), diagnostic_text_content(_reviewedUpdateOffer.value("version", ""))]; break;
    case UpdateScreen::current: title = ui_text("Shin is up to date"); break;
    case UpdateScreen::checkFailed:
        title = ui_text("Could not check for updates");
        detail = ui_text("GitHub is unavailable, the release is incomplete, or this is not an owned installation. Shin continues to work offline. Try again later."); break;
    case UpdateScreen::starting:
        title = ui_text("Opening update progress…");
        detail = ui_text("The update helper is starting. This window stays open until the progress window takes over."); break;
    case UpdateScreen::startFailed: title = ui_text("Update was not started"); detail = self.updateFailure; break;
    case UpdateScreen::resultLoading:
        title = ui_text("Reading update result…"); break;
    case UpdateScreen::acknowledging:
        title = ui_text("Verifying update result…"); break;
    case UpdateScreen::acknowledgeFailed:
        title = ui_text("Update needs review"); detail = self.updateFailure; break;
    case UpdateScreen::resultReady:
        if (_updateResult.value("phase", "") == "idle" || _updateResult.value("phase", "") == "reviewed") {
            title = ui_text("No pending update result"); self.updateCopyButton.hidden = YES;
        } else {
            title = installed ? ui_text("Shin was updated") : ui_text("Update needs review");
            detail = installed ? ui_text("The installed kit was verified. Reconnect persistent MCP clients to use the new version.") :
                ui_text("The update did not confirm completion. The saved result includes the candidate location and native recovery outcome. Do not repeat installation before reviewing it.");
        }
        break;
    case UpdateScreen::idle: break;
    }
    self.updateTitle.stringValue = title; self.updateDetail.stringValue = detail ? detail : @"";
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
        self.updateScreen = UpdateScreen::startFailed;
        self.updateFailure = ui_text("Finish the current Shin operation before updating.");
        [self renderUpdateWindow]; return;
    }
    const auto offered = _reviewedUpdateOffer;
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
        self.updateFailure = update_failure_text(result, failure);
        [self renderUpdateWindow];
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
    }];
}
- (void)setUpdatePending:(BOOL)pending {
    if (pending) [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"ShinUpdatePending"];
    else [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"ShinUpdatePending"];
}
