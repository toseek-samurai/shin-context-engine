// Included in ShinApp: network and install run in the native CLI, never AppKit.
- (void)showUpdateOffer:(id)sender {
    (void)sender;
    [self beginUpdateCheck:YES];
}
- (void)presentUpdateOffer {
    if (!_updateOffer.value("available",false)) return;
    const auto offered=_updateOffer; // Pin the exact release shown during the modal review.
    NSAlert* alert=[NSAlert new];alert.messageText=ui_text("A Shin update is available");
    alert.informativeText=[NSString stringWithFormat:ui_text("Version %@ is available. An update window will show progress while Monitor closes and the kit is replaced. Projects, settings and history stay in place. Reconnect persistent MCP clients afterwards."),diagnostic_text_content(offered.at("version").get<std::string>())];
    [alert addButtonWithTitle:ui_text("Download and install")];[alert addButtonWithTitle:ui_text("Later")];[alert addButtonWithTitle:ui_text("Release notes")];
    const auto choice=[alert runModal];
    if(choice==NSAlertThirdButtonReturn) { [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:diagnostic_text_content(offered.at("release_url").get<std::string>())]];return; }
    if(choice!=NSAlertFirstButtonReturn)return;
    bool expected=false;
    if(!project_management_busy.compare_exchange_strong(expected,true)) {
        NSAlert* busy=[NSAlert new];busy.messageText=ui_text("Finish the current Shin operation before updating.");[busy runModal];return;
    }
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"ShinUpdatePending"];
    [self runCLI:@"update-install" arguments:@[@"--ui-version",diagnostic_text_content(offered.at("version").get<std::string>()),@"--revision",diagnostic_text_content(offered.at("revision").get<std::string>()),@"--wait-pid",[NSString stringWithFormat:@"%d",getpid()],@"--ui-language",shin::ui::language()==shin::ui::Language::ru ? @"ru" : @"en"] completion:^(Json result,NSString* failure) {
        release_project_operation();[[NSUserDefaults standardUserDefaults] removeObjectForKey:@"ShinUpdatePending"];
        const auto error=result.is_object() ? result.value("error",Json()) : Json();
        if(error.is_object() && error.value("code",Json())=="update-offer-changed") {
            _updateOffer=Json::object();release_update_version.clear();self.lastUpdateCheck=nil;
            self.nextUpdateCheck=NSDate.date;
        }
        NSAlert* problem=[NSAlert new];problem.messageText=ui_text("Update was not started");
        problem.informativeText=update_failure_text(result,failure);[problem runModal];
    }];
}
- (void)checkForUpdates:(id)sender { (void)sender;[self beginUpdateCheck:YES]; }
- (void)beginUpdateCheck:(BOOL)manual {
    if(self.updateChecking)return;
    // Explicit review always fetches the current offer; only background checks
    // may reuse the recent-check interval. Confirmation still precedes install.
    if(!manual && self.lastUpdateCheck && -self.lastUpdateCheck.timeIntervalSinceNow<60)return;
    self.updateChecking=YES;self.lastUpdateCheck=NSDate.date;self.nextUpdateCheck=[NSDate dateWithTimeIntervalSinceNow:6*3600];
    [self runCLI:@"update-check" arguments:@[] completion:^(Json result,NSString* failure) {
        self.updateChecking=NO;
        if(!failure && result.value("ok",false)) {
            _updateOffer=result;release_update_version=result.value("version","");
            if(manual) {
                if(result.value("available",false)) [self presentUpdateOffer];
                else {NSAlert* a=[NSAlert new];a.messageText=ui_text("Shin is up to date");[a runModal];}
            }
        } else if(manual) {
            NSAlert* a=[NSAlert new];a.messageText=ui_text("Could not check for updates");
            a.informativeText=ui_text("GitHub is unavailable, the release is incomplete, or this is not an owned installation. Shin continues to work offline. Try again later.");[a runModal];
        }
    }];
}
- (void)toggleAutomaticUpdates:(id)sender {
    (void)sender;
    const BOOL disabled=[[NSUserDefaults standardUserDefaults] boolForKey:@"ShinAutomaticUpdatesDisabled"];
    [[NSUserDefaults standardUserDefaults] setBool:!disabled forKey:@"ShinAutomaticUpdatesDisabled"];
    if(disabled)self.nextUpdateCheck=NSDate.date;
}
- (void)showLastUpdateResult:(id)sender { (void)sender;[self showUpdateResult]; }
- (void)showUpdateResult {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"ShinUpdatePending"];
    [self runCLI:@"update-status" arguments:@[] completion:^(Json result,NSString* failure) {
        if(!failure && (result.value("phase","")=="idle" || result.value("phase","")=="reviewed")) {
            NSAlert* a=[NSAlert new];a.messageText=ui_text("No pending update result");[a runModal];return;
        }
        const bool installed=!failure && result.value("phase","")=="installed" && result.value("ok",false);
        NSAlert* alert=[NSAlert new];alert.messageText=installed ? ui_text("Shin was updated") : ui_text("Update needs review");
        alert.informativeText=installed ? ui_text("The installed kit was verified. Reconnect persistent MCP clients to use the new version.") :
            ui_text("The update did not confirm completion. The saved result includes the candidate location and native recovery outcome. Do not repeat installation before reviewing it.");
        [alert addButtonWithTitle:ui_text("OK")];if(!installed)[alert addButtonWithTitle:ui_text("Copy diagnostic report")];
        if(!installed && result.value("can_acknowledge",false))[alert addButtonWithTitle:ui_text("Acknowledge verified result")];
        const auto choice=[alert runModal];
        if(choice==NSAlertThirdButtonReturn) {
            [self runCLI:@"update-acknowledge" arguments:@[@"--revision",diagnostic_text_content(result.value("review_revision",""))] completion:^(Json acknowledged,NSString* problem) {
                if(problem || !acknowledged.value("ok",false)) { NSAlert* a=[NSAlert new];a.messageText=ui_text("Update needs review");[a runModal]; }
            }];
        }
        if(choice==NSAlertSecondButtonReturn) {
            [NSPasteboard.generalPasteboard clearContents];[NSPasteboard.generalPasteboard setString:diagnostic_text_content(result.dump(2)) forType:NSPasteboardTypeString];
        }
    }];
}
