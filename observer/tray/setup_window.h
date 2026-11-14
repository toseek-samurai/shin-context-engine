// Native onboarding. Detection is read-only; only checked targets are written.
- (NSString*)projectElapsedText {
    const auto seconds = static_cast<long long>(self.setupAdding ?
        std::max(0.0, NSProcessInfo.processInfo.systemUptime - self.setupProgressStarted) : self.setupProgressDuration);
    return [NSString stringWithFormat:ui_text("Elapsed: %lld s"), seconds];
}
- (NSString*)projectStageText:(NSInteger)stage {
    const bool observed = (self.setupProgressMask & (1U << stage)) != 0;
    NSString* mark = stage == self.setupProgressStage ? (self.setupPreparationFailed ? @"!" : @"●") :
        observed ? @"✓" : stage < self.setupProgressStage ? @"—" : @"○";
    return [NSString stringWithFormat:@"%@  %@", mark,
        diagnostic_text_content(shin::ui::project_connection_label(static_cast<int>(stage)))];
}
- (void)showProjectStage:(NSInteger)stage {
    if (stage >= 0 && stage < static_cast<NSInteger>(shin::ui::project_connection_stages.size())) {
        self.setupProgressMask |= 1U << stage;
        self.setupProgressStage = std::max(stage, self.setupProgressStage);
    }
    for (NSUInteger i = 0; i < self.setupProgressRows.count; ++i) {
        NSTextField* row = self.setupProgressRows[i];
        const bool current = static_cast<NSInteger>(i) == self.setupProgressStage;
        row.stringValue = [self projectStageText:static_cast<NSInteger>(i)];
        row.textColor = current ? (self.setupPreparationFailed ?
            [NSColor colorWithCalibratedRed:.941 green:.518 blue:.471 alpha:1] :
            [NSColor colorWithCalibratedRed:.894 green:.800 blue:.635 alpha:1]) :
            (self.setupProgressMask & (1U << i)) ? [NSColor colorWithCalibratedRed:.478 green:.749 blue:.561 alpha:1] : NSColor.secondaryLabelColor;
    }
}
- (void)prepareProjectPresentation:(NSString*)root {
    ++self.setupEpoch; // A previous agent discovery must not replace progress.
    self.setupAdding = YES; self.setupPreparationFailed = NO;
    self.setupProjectConfirmed = NO;
    self.setupRoot = root; self.setupProgressStage = -1; self.setupProgressMask = 0;
    self.setupProgressDuration = 0;
    self.setupProgressStarted = NSProcessInfo.processInfo.systemUptime;
    [self showSetup:nil]; // Create and show the retained window before launching CLI.
    self.setupProject.stringValue = root; self.setupProject.toolTip = root;
    self.setupChoose.enabled = NO;
    self.setupAdvanced.hidden = self.setupConnect.hidden = self.setupCheck.hidden = self.setupLogin.hidden = self.setupNote.hidden = YES;
    self.setupWindow.contentMinSize = NSMakeSize(660, 500);
    [self.setupWindow setContentSize:NSMakeSize(self.setupWindow.contentView.bounds.size.width, 520)];
    [self.setupWindow standardWindowButton:NSWindowCloseButton].enabled = NO;
    live_text(self.setupAgentsHeading, @"stringValue", "Connecting project");
    live_text(self.setupStatus, @"stringValue", "Preparing project… This can take a moment on the first import.");
    self.setupStatus.textColor = NSColor.secondaryLabelColor;
    for (NSView* view in self.setupAgents.arrangedSubviews.copy) { [self.setupAgents removeArrangedSubview:view]; [view removeFromSuperview]; }
    self.setupProgressSpinner = [NSProgressIndicator new];
    self.setupProgressSpinner.style = NSProgressIndicatorStyleSpinning;
    self.setupProgressSpinner.indeterminate = YES;
    [self.setupProgressSpinner.widthAnchor constraintEqualToConstant:18].active = YES;
    [self.setupProgressSpinner.heightAnchor constraintEqualToConstant:18].active = YES;
    self.setupProgressTitle = live_label("Preparing project…");
    NSStackView* working = [NSStackView stackViewWithViews:@[self.setupProgressSpinner, self.setupProgressTitle]];
    working.spacing = 10;
    [self.setupAgents addArrangedSubview:working];
    [self.setupProgressSpinner startAnimation:nil];
    self.setupProgressRows = [NSMutableArray array];
    __weak ShinApp* weak = self;
    for (NSInteger i = 0; i < static_cast<NSInteger>(shin::ui::project_connection_stages.size()); ++i) {
        NSTextField* row = [NSTextField labelWithString:[self projectStageText:i]];
        row.font = [NSFont systemFontOfSize:14];
        live_caption(row, @"stringValue", ^{ return [weak projectStageText:i]; });
        [self.setupProgressRows addObject:row]; [self.setupAgents addArrangedSubview:row];
    }
    self.setupProgressElapsed = [NSTextField labelWithString:[self projectElapsedText]];
    self.setupProgressElapsed.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular];
    self.setupProgressElapsed.textColor = NSColor.secondaryLabelColor;
    live_caption(self.setupProgressElapsed, @"stringValue", ^{ return [weak projectElapsedText]; });
    [self.setupAgents addArrangedSubview:self.setupProgressElapsed];
    [self.setupProgressTimer invalidate];
    self.setupProgressTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* timer) {
        if (!weak.setupAdding) { [timer invalidate]; return; }
        weak.setupProgressElapsed.stringValue = [weak projectElapsedText];
    }];
    [self showProjectStage:-1];
}
- (void)showSetup:(id)sender {
    (void)sender;
    [self closePanel:nil];
    if (!self.setupWindow) {
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 720, 620)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        live_text(window, @"title", "Set up Shin"); window.releasedWhenClosed = NO;
        window.contentMinSize = NSMakeSize(660, 620);
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        window.backgroundColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
        window.contentView.wantsLayer = YES;
        window.contentView.layer.backgroundColor = window.backgroundColor.CGColor;
        window.contentView.layer.borderWidth = 1;
        window.contentView.layer.borderColor = [NSColor colorWithCalibratedRed:.894 green:.800 blue:.635 alpha:1].CGColor;
        NSColor* secondaryInk = [NSColor colorWithCalibratedRed:.690 green:.710 blue:.651 alpha:1];
        NSStackView* stack = [[NSStackView alloc] init];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading; stack.spacing = 10;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
            [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
            [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:24],
            [stack.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-24]]];
        NSTextField* intro = live_label("Choose a project, then select the agents that may use its context. Shin keeps your existing projects and settings.");
        intro.font = [NSFont systemFontOfSize:16];
        [stack addArrangedSubview:intro];
        self.setupProject = live_label("1. Add your first project folder");
        self.setupProject.font = [NSFont systemFontOfSize:16 weight:NSFontWeightMedium];
        self.setupProject.selectable = YES; self.setupProject.maximumNumberOfLines = 2;
        self.setupProject.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [stack addArrangedSubview:self.setupProject];
        self.setupChoose = live_button("Choose project folder…", self, @selector(beginSetupProject:));
        [stack addArrangedSubview:self.setupChoose];
        self.setupAgentsHeading = live_label("2. Select coding agents and profiles", false);
        self.setupAgentsHeading.font = [NSFont systemFontOfSize:16 weight:NSFontWeightMedium];
        [stack addArrangedSubview:self.setupAgentsHeading];
        self.setupAdvanced = live_button("Advanced agent settings", self, @selector(toggleSetupAdvanced:), true);
        [stack addArrangedSubview:self.setupAdvanced];
        NSScrollView* scroll = [[NSScrollView alloc] init];
        scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES; scroll.borderType = NSNoBorder;
        scroll.drawsBackground = YES; scroll.backgroundColor = [NSColor colorWithCalibratedRed:.145 green:.157 blue:.137 alpha:1];
        scroll.wantsLayer = YES; scroll.layer.cornerRadius = 10;
        scroll.layer.borderWidth = 1;
        scroll.layer.borderColor = [NSColor colorWithCalibratedRed:.267 green:.286 blue:.235 alpha:1].CGColor;
        self.setupAgents = [[NSStackView alloc] init];
        self.setupAgents.orientation = NSUserInterfaceLayoutOrientationVertical;
        self.setupAgents.alignment = NSLayoutAttributeLeading; self.setupAgents.spacing = 12;
        self.setupAgents.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
        self.setupAgents.translatesAutoresizingMaskIntoConstraints = NO;
        scroll.documentView = self.setupAgents;
        [self.setupAgents.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor].active = YES;
        // A short, unflipped document otherwise sits at the bottom of its clip
        // view. Fill the viewport at minimum; longer agent lists still grow.
        [self.setupAgents.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.contentView.heightAnchor].active = YES;
        [stack addArrangedSubview:scroll];
        [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
        NSStackView* actions = [NSStackView stackViewWithViews:@[]]; actions.spacing = 12;
        self.setupConnect = live_button("Connect selected agents", self, @selector(connectSetupAgents:));
        self.setupCheck = live_button("Check project", self, @selector(checkSetup:));
        [actions addArrangedSubview:self.setupConnect]; [actions addArrangedSubview:self.setupCheck];
        [stack addArrangedSubview:actions];
        self.setupStatus = live_label("Select a folder to begin.");
        self.setupStatus.font = [NSFont systemFontOfSize:15];
        self.setupStatus.selectable = YES; self.setupStatus.maximumNumberOfLines = 8;
        [stack addArrangedSubview:self.setupStatus];
        NSTextField* note = live_label("Only checked agents are changed. Existing settings are backed up. Live connections refresh automatically; older connections need one MCP reconnect.");
        self.setupNote = note;
        note.font = [NSFont systemFontOfSize:14]; note.textColor = secondaryInk;
        [stack addArrangedSubview:note];
        self.setupLogin = live_button("Enable background service at login", self, @selector(enableSetupLogin:));
        [stack addArrangedSubview:self.setupLogin];
        for (NSButton* button in @[self.setupChoose, self.setupAdvanced, self.setupConnect, self.setupCheck, self.setupLogin]) {
            button.font = [NSFont systemFontOfSize:15];
            [button setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
            [button setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
        }
        self.setupConnect.bezelColor = [NSColor colorWithCalibratedRed:.894 green:.800 blue:.635 alpha:1]; self.setupConnect.contentTintColor = [NSColor colorWithCalibratedRed:.114 green:.122 blue:.110 alpha:1];
        self.setupConnect.font = [NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
        for (NSView* view in @[intro, self.setupProject, scroll, self.setupStatus, note])
            [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
        self.setupWindow = window; [window center];
    }
    if (!self.setupConnecting && !self.setupAdding && !self.setupPreparationFailed) {
        self.setupWindow.contentMinSize = NSMakeSize(660, 620);
        if (self.setupWindow.contentView.bounds.size.height < 620)
            [self.setupWindow setContentSize:NSMakeSize(self.setupWindow.contentView.bounds.size.width, 620)];
        self.setupAdvanced.hidden = self.setupConnect.hidden = self.setupCheck.hidden = self.setupLogin.hidden = self.setupNote.hidden = NO;
        live_text(self.setupAgentsHeading, @"stringValue", "2. Select coding agents and profiles");
        self.setupStatus.textColor = NSColor.labelColor;
        [self refreshSetup:nil];
    }
    [NSApp activateIgnoringOtherApps:YES]; [self.setupWindow makeKeyAndOrderFront:nil];
}
- (void)showAgentSetup:(id)sender {
    (void)sender;
    if (diagnostic_root.empty()) return;
    if (!self.setupConnecting && !self.setupAdding) {
        self.setupPreparationFailed = NO;
        self.setupProjectConfirmed = NO;
        self.setupRoot = [NSString stringWithUTF8String:diagnostic_root.c_str()];
    }
    [self showSetup:nil];
}
- (void)beginSetupProject:(id)sender {
    (void)sender;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    [self addProjectFolder:nil];
}
- (void)showSetupAccess:(NSMutableDictionary*)target state:(NSString*)state {
    target[@"accessState"] = state;
    NSTextField* label = target[@"accessLabel"];
    if (!label) return;
    __weak NSMutableDictionary* accessTarget = target;
    live_caption(label, @"stringValue", ^{
        NSString* current = accessTarget[@"accessState"];
        return ui_text(shin::ui::client_access_label(current ? current.UTF8String : "not-configured"));
    });
}
- (void)refreshSetup:(id)sender {
    (void)sender;
    if (self.setupConnecting || self.setupAdding || self.setupPreparationFailed) return;
    const NSUInteger epoch = ++self.setupEpoch;
    self.setupConnect.enabled = NO;
    self.setupCheck.enabled = self.setupRoot.length > 0 && !project_management_busy.load();
    self.setupLogin.enabled = self.setupCheck.enabled;
    self.setupChoose.enabled = !project_management_busy.load();
    self.setupProject.stringValue = self.setupRoot.length ? self.setupRoot : ui_text("1. Add your first project folder");
    self.setupProject.toolTip = self.setupRoot;
    for (NSView* view in self.setupAgents.arrangedSubviews.copy) { [self.setupAgents removeArrangedSubview:view]; [view removeFromSuperview]; }
    self.setupAdvancedViews = [NSMutableArray array];
    self.setupTargets = [NSMutableArray array]; self.setupTargetButtons = [NSMutableArray array]; self.setupFileButtons = [NSMutableArray array];
    if (!self.setupRoot.length) {
        NSTextField* empty = live_label("Available agents and profiles will appear after you choose a project.");
        empty.font = [NSFont systemFontOfSize:15];
        [self.setupAgents addView:empty inGravity:NSStackViewGravityTop];
        [empty.widthAnchor constraintEqualToAnchor:self.setupAgents.widthAnchor constant:-24].active = YES;
        return;
    }
    live_text(self.setupStatus, @"stringValue", "Finding agents and profiles…");
    [self runCLI:@"client-targets" arguments:@[@"--root", self.setupRoot] completion:^(Json result, NSString* failure) {
        if (epoch != self.setupEpoch) return;
        if (failure || !result.value("ok", false)) { self.setupStatus.stringValue = failure ? failure : ui_text("Could not discover agent settings."); return; }
        NSDictionary* bundles = @{@"codex": @"com.openai.codex", @"opencode": @"ai.opencode.desktop", @"command-code": @"ai.commandcode.desktop", @"hermes": @"com.nousresearch.hermes"};
        for (const auto& row : result.at("targets")) {
            NSString* client = [NSString stringWithUTF8String:row.at("client").get<std::string>().c_str()];
            NSString* label = [NSString stringWithUTF8String:row.at("label").get<std::string>().c_str()];
            NSString* path = [NSString stringWithUTF8String:row.at("config").get<std::string>().c_str()];
            const bool detected = row.value("detected", false) || [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:bundles[client]] != nil;
            NSMutableDictionary* target = [@{@"client": client, @"label": label, @"config": path} mutableCopy];
            target[@"accessState"] = diagnostic_text_content(row.value("access_state", "not-configured"));
            const NSInteger index = static_cast<NSInteger>(self.setupTargets.count); [self.setupTargets addObject:target];
            NSStackView* group = [[NSStackView alloc] init]; group.orientation = NSUserInterfaceLayoutOrientationVertical; group.alignment = NSLayoutAttributeLeading; group.spacing = 4;
            NSStackView* controls = [NSStackView stackViewWithViews:@[]]; controls.spacing = 12;
            NSString* title = detected ? label : [NSString stringWithFormat:ui_text("%@ — not detected"), label];
            NSButton* check = [NSButton checkboxWithTitle:title target:self action:@selector(selectSetupTarget:)]; check.tag = index;
            check.font = [NSFont systemFontOfSize:15];
            live_caption(check, @"title", ^{ return detected ? label : [NSString stringWithFormat:ui_text("%@ — not detected"), label]; });
            NSButton* file = live_button("Choose settings file…", self, @selector(chooseSetupConfig:)); file.tag = index;
            file.font = [NSFont systemFontOfSize:14]; file.bordered = NO; file.bezelStyle = NSBezelStyleInline;
            file.contentTintColor = [NSColor colorWithCalibratedRed:.510 green:.753 blue:.941 alpha:1];
            for (NSButton* button in @[check, file]) {
                [button setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
                [button setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
            }
            [controls addArrangedSubview:check]; [controls addArrangedSubview:file];
            NSTextField* location = [NSTextField labelWithString:path.length ? path : ui_text("Choose a settings file for this custom installation.")]; location.selectable = YES; location.lineBreakMode = NSLineBreakByTruncatingMiddle;
            if (!path.length) live_text(location, @"stringValue", "Choose a settings file for this custom installation.");
            location.font = [NSFont systemFontOfSize:13]; location.toolTip = path;
            location.textColor = [NSColor colorWithCalibratedRed:.690 green:.710 blue:.651 alpha:1];
            NSTextField* access = [NSTextField labelWithString:ui_text(shin::ui::client_access_label([target[@"accessState"] UTF8String]))];
            access.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
            access.textColor = NSColor.labelColor;
            target[@"accessLabel"] = access;
            [self showSetupAccess:target state:target[@"accessState"]];
            [group addArrangedSubview:controls]; [group addArrangedSubview:access]; [group addArrangedSubview:location]; [self.setupAgents addView:group inGravity:NSStackViewGravityTop];
            [group.widthAnchor constraintEqualToAnchor:self.setupAgents.widthAnchor constant:-24].active = YES;
            [location.widthAnchor constraintEqualToAnchor:group.widthAnchor].active = YES;
            target[@"pathLabel"] = location;
            file.hidden = self.setupAdvanced.state != NSControlStateValueOn;
            location.hidden = file.hidden;
            [self.setupAdvancedViews addObject:file]; [self.setupAdvancedViews addObject:location];
            [self.setupTargetButtons addObject:check]; [self.setupFileButtons addObject:file];
        }
        if (self.setupProjectConfirmed) live_text(self.setupStatus, @"stringValue", "Project added. Select the agents that should have access.");
        else live_text(self.setupStatus, @"stringValue", "Choose connections to add or update. Unchecked agents keep their existing configuration.");
    }];
}
- (void)toggleSetupAdvanced:(id)sender {
    (void)sender;
    for (NSView* view in self.setupAdvancedViews) view.hidden = self.setupAdvanced.state != NSControlStateValueOn;
}
- (void)selectSetupTarget:(id)sender {
    (void)sender;
    bool selectedAgent = false;
    for (NSButton* button in self.setupTargetButtons) if (button.state == NSControlStateValueOn) selectedAgent = true;
    self.setupConnect.enabled = selectedAgent && self.setupRoot.length && !self.setupConnecting && !self.setupAdding &&
        !self.setupPreparationFailed && !project_management_busy.load();
}
- (void)chooseSetupConfig:(NSButton*)sender {
    if (self.setupConnecting || sender.tag < 0 || static_cast<NSUInteger>(sender.tag) >= self.setupTargets.count) return;
    NSMutableDictionary* target = self.setupTargets[static_cast<NSUInteger>(sender.tag)];
    NSOpenPanel* panel = [NSOpenPanel openPanel]; panel.canChooseFiles = YES; panel.canChooseDirectories = NO;
    live_text(panel, @"title", "Choose agent settings"); panel.prompt = ui_text("Select");
    NSString* currentPath = target[@"config"];
    panel.directoryURL = [NSURL fileURLWithPath:currentPath.length ? currentPath.stringByDeletingLastPathComponent : NSHomeDirectory()];
    if ([panel runModal] != NSModalResponseOK || !panel.URL.path) return;
    target[@"config"] = panel.URL.path;
    NSTextField* label = target[@"pathLabel"]; label.stringValue = panel.URL.path; label.toolTip = panel.URL.path;
    [self showSetupAccess:target state:@"review-required"];
    const NSUInteger epoch = self.setupEpoch; NSString* selectedPath = panel.URL.path;
    [self runCLI:@"client-connect" arguments:@[@"--root", self.setupRoot, @"--client", target[@"client"], @"--config", selectedPath]
        completion:^(Json result, NSString* failure) {
            if (epoch != self.setupEpoch || ![target[@"config"] isEqualToString:selectedPath]) return;
            [self showSetupAccess:target state:diagnostic_text_content(failure ? "review-required" : result.value("access_state", "review-required"))];
        }];
}
- (void)setSetupConnecting:(BOOL)connecting {
    _setupConnecting = connecting;
    self.setupChoose.enabled = !connecting;
    self.setupCheck.enabled = !connecting && self.setupRoot.length;
    self.setupLogin.enabled = self.setupCheck.enabled;
    for (NSButton* button in self.setupTargetButtons) button.enabled = !connecting;
    for (NSButton* button in self.setupFileButtons) button.enabled = !connecting;
    if (!connecting) release_project_operation();
    [self selectSetupTarget:nil];
}
- (void)connectSetupAgents:(id)sender {
    (void)sender;
    if (!self.setupConnect.enabled) return;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    NSMutableArray<NSMutableDictionary*>* selected = [NSMutableArray array];
    for (NSButton* button in self.setupTargetButtons) if (button.state == NSControlStateValueOn) {
        [selected addObject:self.setupTargets[static_cast<NSUInteger>(button.tag)]];
    }
    ++self.setupEpoch;
    self.setupConnecting = YES; live_text(self.setupStatus, @"stringValue", "Checking existing settings…");
    [self connectSetupSelection:selected root:self.setupRoot reviews:Json()];
}
- (void)connectSetupSelection:(NSArray<NSMutableDictionary*>*)selected root:(NSString*)root reviews:(const Json&)reviews {
    const bool applying = reviews.is_array();
    Json targets = Json::array();
    for (NSUInteger i = 0; i < selected.count; ++i) {
        NSDictionary* target = selected[i];
        Json entry{{"client", [target[@"client"] UTF8String]}, {"config", [target[@"config"] UTF8String]}};
        if (applying) entry["revision"] = reviews.at(i).at("revision");
        targets.push_back(std::move(entry));
    }
    NSMutableArray* args = [@[@"--root", root] mutableCopy];
    if (applying) [args addObject:@"--apply"];
    const auto bytes = targets.dump();
    NSData* input = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    [self runCLI:@"client-connect-batch" arguments:args input:input progress:nil completion:^(Json result, NSString* failure) {
        const auto rows = result.value("results", Json::array());
        if (!failure && !applying && result.value("ok", false) && rows.size() == selected.count) {
            [self connectSetupSelection:selected root:root reviews:rows];
            return;
        }
        NSMutableString* report = [NSMutableString string];
        if (!applying) [report appendString:ui_text("No agent settings were changed.\n")];
        for (NSUInteger i = 0; i < rows.size() && i < selected.count; ++i) {
            const auto& row = rows.at(i);
            NSMutableDictionary* target = selected[i];
            const auto access = row.value("access_state", "review-required");
            [self showSetupAccess:target state:diagnostic_text_content(access)];
            if (row.value("ok", false)) {
                if (applying) [report appendFormat:@"%@: %@\n", target[@"label"], ui_text(shin::ui::client_access_label(access))];
            } else {
                const auto reason = row.at("error").value("message", "");
                [report appendFormat:@"%@: %@\n", target[@"label"], reason.empty() ?
                    ui_text("Could not update agent settings.") : diagnostic_text_content(reason)];
            }
        }
        if (!failure && applying && result.value("ok", false) && rows.size() == selected.count) {
            [report appendString:ui_text(result.value("restart_required", false) ?
                "Access saved. Reconnect older MCP connections once; subsequent projects appear without restarting the agent." :
                "Access saved. Connected agents see this project on their next shin_projects call; no restart is needed.")];
        } else if (failure || rows.empty() || rows.back().value("ok", false)) {
            const auto error = result.value("error", Json::object());
            const auto reason = error.value("message", "");
            if (!failure) failure = error.value("code", "") == "client-config-duplicate" ?
                ui_text("Choose a different settings file for each target.") :
                reason.empty() ? ui_text("Could not update agent settings.") : diagnostic_text_content(reason);
            [report appendString:failure];
        }
        self.setupStatus.stringValue = report;
        self.setupConnecting = NO;
    }];
}
- (void)checkSetup:(id)sender {
    (void)sender;
    if (!self.setupRoot.length || !self.setupCheck.enabled) return;
    self.setupCheck.enabled = NO; const NSUInteger epoch = self.setupEpoch;
    live_text(self.setupStatus, @"stringValue", "Checking project…");
    [self runCLI:@"status" arguments:@[@"--root", self.setupRoot] completion:^(Json result, NSString* failure) {
        if (epoch != self.setupEpoch) return;
        self.setupCheck.enabled = YES;
        if (failure || !result.value("ok", false) || !result.value("local_ready", false)) {
            live_text(self.setupStatus, @"stringValue", "Search is not ready yet. Open Projects for preparation progress or diagnostics, then check again."); return;
        }
        const bool partial = result.contains("source_coverage") && result.at("source_coverage").value("state", "") == "partial";
        self.setupStatus.stringValue = partial ? ui_text("Search is ready with partial source coverage. Review Projects for excluded files. The agent connection still needs checking in the agent.") :
            ui_text("Project search is ready. Ask the selected agent to call shin_projects, then shin_status for this project.");
    }];
}
- (void)enableSetupLogin:(id)sender {
    (void)sender;
    if (shin::observer::homebrew_managed()) {
        self.setupStatus.stringValue = ui_text("For login startup, stop the session service and run: brew services start toseek-samurai/tap/shin-context-engine");
        return;
    }
    if (!self.setupLogin.enabled) return;
    self.setupLogin.enabled = NO; const NSUInteger epoch = self.setupEpoch;
    [self runCLI:@"service-install" arguments:@[] completion:^(Json result, NSString* failure) {
        if (epoch != self.setupEpoch) return; self.setupLogin.enabled = YES;
        self.setupStatus.stringValue = !failure && result.value("ok", false) ?
            ui_text("Background service will start at login. Monitor login startup is controlled in its app menu.") :
            (failure ? failure : ui_text("Could not enable login startup. Check the service diagnostics."));
    }];
}
