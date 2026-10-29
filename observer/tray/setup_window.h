// Native onboarding. Detection is read-only; only checked targets are written.
- (void)showSetup:(id)sender {
    (void)sender;
    [self closePanel:nil];
    if (!self.setupWindow) {
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 720, 620)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        live_text(window, @"title", "Set up Shin"); window.releasedWhenClosed = NO;
        window.contentMinSize = NSMakeSize(660, 620);
        NSStackView* stack = [[NSStackView alloc] init];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading; stack.spacing = 12;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:24],
            [stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-24],
            [stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:24],
            [stack.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-24]]];
        NSTextField* intro = live_label("Choose a project, then select the agents that may use its context. Shin keeps your existing projects and settings.");
        [stack addArrangedSubview:intro];
        self.setupProject = live_label("1. Add your first project folder");
        self.setupProject.selectable = YES; self.setupProject.maximumNumberOfLines = 2;
        self.setupProject.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [stack addArrangedSubview:self.setupProject];
        self.setupChoose = live_button("Choose project folder…", self, @selector(beginSetupProject:));
        [stack addArrangedSubview:self.setupChoose];
        [stack addArrangedSubview:live_label("2. Select coding agents and profiles", false)];
        self.setupAdvanced = live_button("Advanced agent settings", self, @selector(toggleSetupAdvanced:), true);
        [stack addArrangedSubview:self.setupAdvanced];
        NSScrollView* scroll = [[NSScrollView alloc] init];
        scroll.hasVerticalScroller = YES; scroll.borderType = NSBezelBorder;
        self.setupAgents = [[NSStackView alloc] init];
        self.setupAgents.orientation = NSUserInterfaceLayoutOrientationVertical;
        self.setupAgents.alignment = NSLayoutAttributeLeading; self.setupAgents.spacing = 12;
        self.setupAgents.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
        self.setupAgents.translatesAutoresizingMaskIntoConstraints = NO;
        scroll.documentView = self.setupAgents;
        [self.setupAgents.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor].active = YES;
        [stack addArrangedSubview:scroll];
        [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
        NSStackView* actions = [NSStackView stackViewWithViews:@[]]; actions.spacing = 12;
        self.setupConnect = live_button("Connect selected agents", self, @selector(connectSetupAgents:));
        self.setupCheck = live_button("Check project", self, @selector(checkSetup:));
        [actions addArrangedSubview:self.setupConnect]; [actions addArrangedSubview:self.setupCheck];
        [stack addArrangedSubview:actions];
        self.setupStatus = live_label("Select a folder to begin.");
        self.setupStatus.selectable = YES; self.setupStatus.maximumNumberOfLines = 8;
        [stack addArrangedSubview:self.setupStatus];
        NSTextField* note = live_label("Only checked agents are changed. A private backup is kept for existing files. Restart those agents after connecting; project-specific settings may override user settings.");
        [stack addArrangedSubview:note];
        self.setupLogin = live_button("Enable background service at login", self, @selector(enableSetupLogin:));
        [stack addArrangedSubview:self.setupLogin];
        for (NSView* view in @[intro, self.setupProject, scroll, self.setupStatus, note])
            [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
        self.setupWindow = window; [window center];
    }
    if (!self.setupConnecting) [self refreshSetup:nil];
    [NSApp activateIgnoringOtherApps:YES]; [self.setupWindow makeKeyAndOrderFront:nil];
}
- (void)showAgentSetup:(id)sender {
    (void)sender;
    if (diagnostic_root.empty()) return;
    if (!self.setupConnecting) self.setupRoot = [NSString stringWithUTF8String:diagnostic_root.c_str()];
    [self showSetup:nil];
}
- (void)beginSetupProject:(id)sender {
    (void)sender;
    bool expected = false;
    if (!project_management_busy.compare_exchange_strong(expected, true)) return;
    [self addProjectFolder:nil];
}
- (void)refreshSetup:(id)sender {
    (void)sender;
    if (self.setupConnecting) return;
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
        [self.setupAgents addArrangedSubview:live_label("Available agents and profiles will appear after you choose a project.")];
        return;
    }
    live_text(self.setupStatus, @"stringValue", "Finding agents and profiles…");
    [self runCLI:@"client-targets" arguments:@[] completion:^(Json result, NSString* failure) {
        if (epoch != self.setupEpoch) return;
        if (failure || !result.value("ok", false)) { self.setupStatus.stringValue = failure ? failure : ui_text("Could not discover agent settings."); return; }
        NSDictionary* bundles = @{@"codex": @"com.openai.codex", @"opencode": @"ai.opencode.desktop", @"command-code": @"ai.commandcode.desktop", @"hermes": @"com.nousresearch.hermes"};
        for (const auto& row : result.at("targets")) {
            NSString* client = [NSString stringWithUTF8String:row.at("client").get<std::string>().c_str()];
            NSString* label = [NSString stringWithUTF8String:row.at("label").get<std::string>().c_str()];
            NSString* path = [NSString stringWithUTF8String:row.at("config").get<std::string>().c_str()];
            const bool detected = row.value("detected", false) || [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:bundles[client]] != nil;
            NSMutableDictionary* target = [@{@"client": client, @"label": label, @"config": path} mutableCopy];
            const NSInteger index = static_cast<NSInteger>(self.setupTargets.count); [self.setupTargets addObject:target];
            NSStackView* group = [[NSStackView alloc] init]; group.orientation = NSUserInterfaceLayoutOrientationVertical; group.alignment = NSLayoutAttributeLeading; group.spacing = 4;
            NSStackView* controls = [NSStackView stackViewWithViews:@[]]; controls.spacing = 12;
            NSString* title = detected ? label : [NSString stringWithFormat:ui_text("%@ — not detected"), label];
            NSButton* check = [NSButton checkboxWithTitle:title target:self action:@selector(selectSetupTarget:)]; check.tag = index;
            live_caption(check, @"title", ^{ return detected ? label : [NSString stringWithFormat:ui_text("%@ — not detected"), label]; });
            NSButton* file = live_button("Choose settings file…", self, @selector(chooseSetupConfig:)); file.tag = index;
            [controls addArrangedSubview:check]; [controls addArrangedSubview:file];
            NSTextField* location = [NSTextField labelWithString:path.length ? path : ui_text("Choose a settings file for this custom installation.")]; location.selectable = YES; location.lineBreakMode = NSLineBreakByTruncatingMiddle;
            if (!path.length) live_text(location, @"stringValue", "Choose a settings file for this custom installation.");
            location.font = [NSFont systemFontOfSize:11]; location.toolTip = path;
            [group addArrangedSubview:controls]; [group addArrangedSubview:location]; [self.setupAgents addArrangedSubview:group];
            [group.widthAnchor constraintEqualToAnchor:self.setupAgents.widthAnchor constant:-24].active = YES;
            [location.widthAnchor constraintEqualToAnchor:group.widthAnchor].active = YES;
            target[@"pathLabel"] = location;
            file.hidden = self.setupAdvanced.state != NSControlStateValueOn;
            location.hidden = file.hidden;
            [self.setupAdvancedViews addObject:file]; [self.setupAdvancedViews addObject:location];
            [self.setupTargetButtons addObject:check]; [self.setupFileButtons addObject:file];
        }
        live_text(self.setupStatus, @"stringValue", "Choose connections to add or update. Unchecked agents keep their existing configuration.");
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
    self.setupConnect.enabled = selectedAgent && self.setupRoot.length && !self.setupConnecting && !project_management_busy.load();
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
    self.setupPlans = [NSMutableArray array];
    NSMutableSet* paths = [NSMutableSet set];
    for (NSButton* button in self.setupTargetButtons) if (button.state == NSControlStateValueOn) {
        NSDictionary* target = self.setupTargets[static_cast<NSUInteger>(button.tag)];
        if ([paths containsObject:target[@"config"]]) {
            release_project_operation(); live_text(self.setupStatus, @"stringValue", "Choose a different settings file for each target."); return;
        }
        [paths addObject:target[@"config"]];
        [self.setupPlans addObject:[@{@"client": target[@"client"], @"label": target[@"label"], @"config": target[@"config"]} mutableCopy]];
    }
    ++self.setupEpoch;
    self.setupConnectRoot = self.setupRoot; self.setupReport = [NSMutableString string];
    self.setupConnecting = YES; live_text(self.setupStatus, @"stringValue", "Checking existing settings…");
    [self continueSetupConnection:0 applying:NO];
}
- (void)continueSetupConnection:(NSUInteger)index applying:(BOOL)applying {
    if (index == self.setupPlans.count) {
        if (!applying) { [self continueSetupConnection:0 applying:YES]; return; }
        self.setupStatus.stringValue = [self.setupReport stringByAppendingString:ui_text("Restart the selected agents to activate MCP, then ask them to call shin_status.")];
        self.setupConnecting = NO; return;
    }
    NSMutableDictionary* plan = self.setupPlans[index];
    NSMutableArray* args = [@[@"--root", self.setupConnectRoot, @"--client", plan[@"client"], @"--config", plan[@"config"]] mutableCopy];
    if (applying) [args addObjectsFromArray:@[@"--revision", plan[@"revision"]]];
    [self runCLI:@"client-connect" arguments:args completion:^(Json result, NSString* failure) {
        if (failure || !result.value("ok", false)) {
            NSString* reason = failure;
            if (!reason && result.contains("error") && result.at("error").is_object())
                reason = [NSString stringWithUTF8String:result.at("error").value("message", "").c_str()];
            if (!reason.length) reason = ui_text("Could not update agent settings.");
            NSString* prefix = applying ? self.setupReport : ui_text("No agent settings were changed.\n");
            self.setupStatus.stringValue = [NSString stringWithFormat:@"%@%@: %@", prefix, plan[@"label"], reason];
            self.setupConnecting = NO; return;
        }
        if (!applying) plan[@"revision"] = [NSString stringWithUTF8String:result.at("revision").get<std::string>().c_str()];
        else [self.setupReport appendFormat:@"%@: %@\n", plan[@"label"], result.value("changed", false) ? ui_text("Configured") : ui_text("Already configured")];
        [self continueSetupConnection:index + 1 applying:applying];
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
            ui_text("Project search is ready. Now reconnect your agent and call shin_status there to verify its connection.");
    }];
}
- (void)enableSetupLogin:(id)sender {
    (void)sender;
    if (!self.setupLogin.enabled) return;
    self.setupLogin.enabled = NO; const NSUInteger epoch = self.setupEpoch;
    [self runCLI:@"service-install" arguments:@[] completion:^(Json result, NSString* failure) {
        if (epoch != self.setupEpoch) return; self.setupLogin.enabled = YES;
        self.setupStatus.stringValue = !failure && result.value("ok", false) ?
            ui_text("Background service will start at login. Monitor login startup is controlled in its app menu.") :
            (failure ? failure : ui_text("Could not enable login startup. Check the service diagnostics."));
    }];
}
