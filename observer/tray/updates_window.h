// Included in ShinApp: network and install run in the native CLI, never AppKit.
// One retained window owns the complete review. Background replies stay quiet;
// a failed explicit installation returns the still-open review to the foreground.
- (void)ensureUpdateWindow {
    if (self.updateWindow) return;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 360)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO; window.restorable = NO;
    window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
    live_text(window, @"title", "Shin updates");
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window.backgroundColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
    window.contentView.wantsLayer = YES;
    window.contentView.layer.backgroundColor = window.backgroundColor.CGColor;
    window.contentView.layer.borderWidth = 1;
    window.contentView.layer.borderColor = [NSColor colorWithCalibratedRed:.894 green:.800 blue:.635 alpha:1].CGColor;
    NSStackView* stack = [NSStackView new];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading; stack.spacing = 10;
    stack.distribution = NSStackViewDistributionFill; stack.detachesHiddenViews = YES;
    self.updateLayout = stack;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:22],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:window.contentView.bottomAnchor constant:-22]]];
    NSColor* sand = [NSColor colorWithCalibratedRed:.894 green:.800 blue:.635 alpha:1];
    NSColor* ink = [NSColor colorWithCalibratedRed:.941 green:.937 blue:.890 alpha:1];
    NSColor* blue = [NSColor colorWithCalibratedRed:.510 green:.753 blue:.941 alpha:1];
    NSColor* muted = [NSColor colorWithCalibratedRed:.690 green:.710 blue:.651 alpha:1];
    NSImageView* icon = [NSImageView new];
    NSImage* logo = [NSBundle.mainBundle imageForResource:@"shin-logo"];
    icon.image = logo ? logo : [NSImage imageWithSystemSymbolName:@"arrow.down.circle.fill" accessibilityDescription:nil];
    icon.contentTintColor = sand; icon.imageScaling = NSImageScaleProportionallyDown;
    [icon.widthAnchor constraintEqualToConstant:30].active = YES;
    [icon.heightAnchor constraintEqualToConstant:30].active = YES;
    NSTextField* brand = [NSTextField labelWithString:@"SHIN"];
    brand.font = [NSFont systemFontOfSize:16 weight:NSFontWeightMedium]; brand.textColor = ink;
    NSTextField* channel = [NSTextField labelWithString:@""]; live_text(channel, @"stringValue", "Preview channel");
    channel.font = [NSFont systemFontOfSize:13]; channel.textColor = muted;
    NSStackView* identity = [NSStackView stackViewWithViews:@[brand, channel]];
    identity.orientation = NSUserInterfaceLayoutOrientationVertical; identity.alignment = NSLayoutAttributeLeading; identity.spacing = 2;
    NSStackView* header = [NSStackView stackViewWithViews:@[icon, identity]]; header.spacing = 10;
    self.updateTitle = [NSTextField wrappingLabelWithString:@""];
    self.updateTitle.font = [NSFont systemFontOfSize:24 weight:NSFontWeightMedium];
    self.updateTitle.textColor = ink;
    self.updateVersion = [NSTextField wrappingLabelWithString:@""];
    self.updateVersion.font = [NSFont monospacedDigitSystemFontOfSize:15 weight:NSFontWeightMedium];
    self.updateVersion.textColor = blue;
    self.updateDetail = [NSTextField wrappingLabelWithString:@""];
    self.updateDetail.font = [NSFont systemFontOfSize:16]; self.updateDetail.selectable = YES;
    self.updateDetail.textColor = muted;
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
    self.updateResumeButton = live_button("Continue update", self, @selector(resumeUpdateResult:));
    self.updateReviewButton = live_button("Review previous update", self, @selector(showLastUpdateResult:));
    for (NSButton* button in @[self.updateCheckButton, self.updateInstallButton, self.updateNotesButton,
            self.updateCopyButton, self.updateAcknowledgeButton, self.updateReviewButton, self.updateResumeButton]) {
        button.font = [NSFont systemFontOfSize:15];
        [button setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
        [button setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    for (NSButton* button in @[self.updateInstallButton, self.updateAcknowledgeButton]) {
        button.bezelColor = sand; button.contentTintColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
        button.font = [NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
    }
    for (NSButton* button in @[self.updateNotesButton, self.updateCopyButton]) {
        button.bordered = NO; button.bezelStyle = NSBezelStyleInline; button.contentTintColor = blue;
        button.font = [NSFont systemFontOfSize:14];
    }
    NSStackView* links = [NSStackView stackViewWithViews:@[self.updateNotesButton, self.updateCopyButton]];
    links.spacing = 12; links.detachesHiddenViews = YES;
    NSStackView* actions = [NSStackView stackViewWithViews:@[self.updateCheckButton, self.updateInstallButton]];
    actions.spacing = 10; actions.distribution = NSStackViewDistributionFill; actions.detachesHiddenViews = YES;
    NSBox* separator = [NSBox new]; separator.boxType = NSBoxSeparator;
    for (NSView* view in @[header, self.updateTitle, self.updateVersion, details, self.updateSpinner, self.updateAcknowledgeButton, self.updateResumeButton, self.updateReviewButton, separator, links, actions])
        [stack addArrangedSubview:view];
    for (NSView* view in @[self.updateTitle, self.updateVersion, details, self.updateSpinner, separator])
        [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    for (NSView* view in @[links, actions])
        [view.widthAnchor constraintLessThanOrEqualToAnchor:stack.widthAnchor].active = YES;
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
    const bool installed = _updateResult.is_object() && (_updateResult.value("outcome", "") == "installed" ||
        (_updateResult.value("phase", "") == "installed" && _updateResult.value("ok", false)));
    self.updateReviewButton.hidden = self.updateScreen != UpdateScreen::startFailed ||
        cli_error_code(_updateResult) != "update-outcome-unknown";
    self.updateCheckButton.enabled = !checking && !loading && !self.updateActionBusy;
    self.updateInstallButton.hidden = !offer && !starting;
    self.updateInstallButton.enabled = offer && !self.updateActionBusy;
    self.updateNotesButton.hidden = !offer;
    self.updateCopyButton.hidden = !result && self.updateScreen != UpdateScreen::startFailed;
    self.updateAcknowledgeButton.hidden = !result || installed || !_updateResult.value("can_acknowledge", false);
    self.updateAcknowledgeButton.enabled = !self.updateActionBusy;
    self.updateResumeButton.hidden = !result || !(_updateResult.value("can_retry", false) || _updateResult.value("can_resume", false));
    self.updateResumeButton.enabled = !self.updateActionBusy;
    self.updateResumeButton.title = ui_text(_updateResult.is_object() && _updateResult.value("can_retry", false) ? "Retry update" : "Continue update");
    self.updateCheckButton.title = ui_text(offer || self.updateScreen == UpdateScreen::startFailed ? "Check again" : "Check for updates…");
    for (NSButton* button in @[self.updateInstallButton, self.updateAcknowledgeButton])
        button.attributedTitle = [[NSAttributedString alloc] initWithString:button.title attributes:@{
            NSFontAttributeName:button.font, NSForegroundColorAttributeName:[NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1]}];
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
        detail = ui_text("Monitor will close while the updater installs the new version, then reopen. Projects, settings and history stay in place. Compatible MCP connections resume automatically; older integrations may need one reconnection."); break;
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
            detail = installed ? ui_text(_updateResult.value("projects_pending", 0) > 0 ?
                "Shin is installed. Some projects are still preparing or need attention; inspect Projects for details." :
                "The installed kit was verified. Compatible MCP connections resume automatically; reconnect an older integration if it reports a version mismatch.") :
                _updateResult.value("can_acknowledge", false) && _updateResult.value("installation_verified", false) ?
                ui_text("The current installation is verified and has no pending recovery. Confirm this saved result to clear the previous attempt and allow another update. Project data stays in place.") :
                ui_text("The update did not confirm completion. The saved result includes the candidate location and native recovery outcome. Do not repeat installation before reviewing it.");
        }
        if (!installed && _updateResult.value("phase", "") != "failed" && (_updateResult.contains("error") ||
                (_updateResult.contains("result") && _updateResult.at("result").is_object() && _updateResult.at("result").contains("error"))))
            detail = [NSString stringWithFormat:@"%@\n\n%@", update_failure_text(_updateResult, nil), detail];
        break;
    case UpdateScreen::idle: break;
    }
    if (result && _updateResult.contains("affected_projects") && _updateResult.at("affected_projects").is_array()) {
        NSMutableString* projects = [NSMutableString string];
        std::size_t shown = 0;
        for (const auto& project : _updateResult.at("affected_projects")) {
            if (!project.is_object() || !project.contains("name") || !project.at("name").is_string() ||
                !project.contains("reason") || !project.at("reason").is_string()) continue;
            if (shown++ == 16) break;
            [projects appendFormat:@"\n%@ — %@", diagnostic_text_content(project.at("name").get_ref<const std::string&>()),
                diagnostic_text_content(project.at("reason").get_ref<const std::string&>())];
        }
        if (projects.length) detail = [NSString stringWithFormat:@"%@\n\n%@%@", detail,
            ui_text("Affected projects"), projects];
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
    self.updateDetailHeight.constant = std::clamp(std::ceil(textBounds.size.height) + 6.0, 24.0, 128.0);
    [self.updateWindow.contentView layoutSubtreeIfNeeded];
    const CGFloat desired = std::ceil(self.updateLayout.fittingSize.height) + 44.0;
    if (desired > 460 && !self.updateDetails.hidden) {
        self.updateDetailHeight.constant = std::max(24.0, self.updateDetailHeight.constant - (desired - 460));
        [self.updateWindow.contentView layoutSubtreeIfNeeded];
    }
    const CGFloat height = std::clamp(std::ceil(self.updateLayout.fittingSize.height) + 44.0, 240.0, 460.0);
    const NSRect before = self.updateWindow.frame;
    NSRect frame = [self.updateWindow frameRectForContentRect:NSMakeRect(0, 0, 620, height)];
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
    if (shin::observer::homebrew_managed()) {
        if (manual) {
            NSAlert* alert = [NSAlert new];
            alert.icon = NSApp.applicationIconImage;
            alert.messageText = ui_text("Update with Homebrew");
            alert.informativeText = [NSString stringWithFormat:@"%@\n\nbrew upgrade toseek-samurai/tap/shin-context-engine",
                ui_text("Homebrew manages this installation. Quit Monitor and TUI before upgrading, then restart the background service. Compatible MCP connections resume automatically; older integrations may need one reconnection.")];
            [alert addButtonWithTitle:ui_text("Close")];
            [alert addButtonWithTitle:ui_text("Copy command")];
            [NSApp activateIgnoringOtherApps:YES];
            if ([alert runModal] == NSAlertSecondButtonReturn) {
                [NSPasteboard.generalPasteboard clearContents];
                [NSPasteboard.generalPasteboard setString:@"brew upgrade toseek-samurai/tap/shin-context-engine" forType:NSPasteboardTypeString];
            }
        }
        return;
    }
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
                if (result.value("available", false)) {
                    const auto epoch = self.updateWindowEpoch;
                    self.updateChecking = YES;
                    [self runCLI:@"update-status" arguments:@[] completion:^(Json saved, NSString* problem) {
                        self.updateChecking = NO;
                        if (epoch != self.updateWindowEpoch && self.updateScreen != UpdateScreen::checking) return;
                        const auto phase = saved.value("outcome", saved.value("phase", ""));
                        if (problem || phase.empty() || (phase != "idle" && phase != "reviewed" && phase != "installed")) {
                            _updateResult = saved;
                            if (problem) _updateResult = {{"ok", false}, {"error", {{"message", problem.UTF8String}}}};
                            self.updateScreen = UpdateScreen::resultReady; [self renderUpdateWindow];
                        } else [self presentUpdateOffer];
                    }];
                }
                else { self.updateScreen = UpdateScreen::current; [self renderUpdateWindow]; }
            }
        } else if (visibleReview) { self.updateScreen = UpdateScreen::checkFailed; [self renderUpdateWindow]; }
    }];
}
- (void)toggleAutomaticUpdates:(id)sender {
    (void)sender;
    if (shin::observer::homebrew_managed()) return;
    const BOOL disabled = [[NSUserDefaults standardUserDefaults] boolForKey:@"ShinAutomaticUpdatesDisabled"];
    [[NSUserDefaults standardUserDefaults] setBool:!disabled forKey:@"ShinAutomaticUpdatesDisabled"];
    if (disabled) self.nextUpdateCheck = NSDate.date;
}
- (void)showLastUpdateResult:(id)sender { (void)sender; [self showUpdateResult]; }
- (void)showUpdateResult {
    if (shin::observer::homebrew_managed()) { [self beginUpdateCheck:YES]; return; }
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

- (void)resumeUpdateResult:(id)sender {
    (void)sender;
    if (self.updateActionBusy || self.updateScreen != UpdateScreen::resultReady ||
        !(_updateResult.value("can_retry", false) || _updateResult.value("can_resume", false))) return;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    const auto revision = _updateResult.value("review_revision", "");
    const auto version = _updateResult.value("version", "");
    if (revision.empty() || version.empty()) { release_project_operation(); return; }
    _reviewedUpdateOffer = {{"version", version}, {"current_version", _updateResult.value("current_version", std::string(SHIN_OBSERVER_VERSION))}};
    self.updateActionBusy = YES; self.updateScreen = UpdateScreen::starting;
    [self renderUpdateWindow]; [self setUpdatePending:YES];
    [self runCLI:@"update-resume" arguments:@[@"--revision", diagnostic_text_content(revision), @"--ui-version", diagnostic_text_content(version),
        @"--wait-pid", [NSString stringWithFormat:@"%d", getpid()], @"--ui-language",
        shin::ui::language() == shin::ui::Language::ru ? @"ru" : @"en"] completion:^(Json result, NSString* failure) {
        release_project_operation(); self.updateActionBusy = NO; [self setUpdatePending:NO];
        _updateResult = result; self.updateFailure = failure;
        if (failure) _updateResult = {{"ok", false}, {"error", {{"message", failure.UTF8String}}}};
        self.updateScreen = UpdateScreen::resultReady; [self renderUpdateWindow];
        if (self.updateWindow.visible && !self.updateWindow.miniaturized) [self showUpdateWindow];
    }];
}
