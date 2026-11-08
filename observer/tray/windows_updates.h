#pragma once

// Release checks read public metadata only. Installation is a separate explicit
// handoff to the staged helper and native Windows Installer.
Json fetch_windows_release() {
    struct Close { void operator()(void* value) const { if (value) WinHttpCloseHandle(value); } };
    using Internet = std::unique_ptr<void, Close>;
    const auto require = [](bool ok) {
        if (!ok) throw Error("update-check-failed", "The public release feed could not be read.");
    };
    const auto user_agent = wide(std::string("Shin-Monitor/") + SHIN_OBSERVER_VERSION);
    Internet session(WinHttpOpen(user_agent.c_str(),
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(session != nullptr);
    require(WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 3000) != FALSE);
    Internet connection(WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    require(connection != nullptr);
    Internet request(WinHttpOpenRequest(connection.get(), L"GET",
        L"/repos/toseek-samurai/shin-context-engine/releases?per_page=30", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    require(request != nullptr);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    require(WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) != FALSE);
    require(WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) != FALSE);
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    require(WinHttpSendRequest(request.get(), L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE);
    require(WinHttpReceiveResponse(request.get(), nullptr) != FALSE);
    DWORD status = 0, size = sizeof(status);
    require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE);
    require(status == 200);
    std::string text;
    std::array<char, 8192> buffer{};
    for (;;) {
        require(Clock::now() < deadline);
        DWORD read = 0;
        require(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read) != FALSE);
        if (!read) break;
        require(text.size() + read <= kMaximumReleaseFeedBytes);
        text.append(buffer.data(), read);
    }
    return select_windows_release(parse_release_feed(text), SHIN_OBSERVER_VERSION);
}

enum class WindowsUpdateState { idle, checking, current, offer, failed };
struct WindowsUpdateSnapshot { WindowsUpdateState state = WindowsUpdateState::idle; Json offer = Json::object(); };
class WindowsReleaseCheck {
public:
    explicit WindowsReleaseCheck(std::function<Json()> fetch = fetch_windows_release) : fetch(std::move(fetch)) {}
    WindowsUpdateSnapshot snapshot() {
        std::lock_guard lock(mutex);
        return value;
    }
    void check() {
        {
            std::lock_guard lock(mutex);
            if (value.state == WindowsUpdateState::checking) return;
            value = {WindowsUpdateState::checking, Json::object()};
        }
        worker = std::jthread([this] {
            WindowsUpdateSnapshot result;
            try {
                result.offer = fetch();
                result.state = result.offer.value("available", false) ? WindowsUpdateState::offer : WindowsUpdateState::current;
            } catch (const std::exception&) { result.state = WindowsUpdateState::failed; }
            { std::lock_guard lock(mutex); value = std::move(result); }
            PostMessageW(panel, WM_NULL, 0, 0);
        });
    }
private:
    std::mutex mutex;
    WindowsUpdateSnapshot value;
    std::function<Json()> fetch;
    std::jthread worker;
};
std::unique_ptr<WindowsReleaseCheck> release_check;
bool windows_update_details_open = false;

void open_updates(bool check) {
    updates_page = true; management_page = false;
    if (!release_check) release_check = std::make_unique<WindowsReleaseCheck>();
    if(check) release_check->check();
    show_panel();
}

bool update_button(const char* label, int id, bool enabled = true) {
    ImGui::PushID(id); ImGui::BeginDisabled(!enabled);
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float width = std::min(available, ImGui::CalcTextSize(label).x + 24);
    const float height = ImGui::GetIO().DisplaySize.y < 600 ? 30.0f : 36.0f;
    const bool pressed = ImGui::Button(label, {width, height});
    if (ImGui::IsItemVisible()) {
        const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        const auto clip = ImGui::GetWindowDrawList()->GetClipRectMax();
        ui_actions.push_back({label, "", lo, {std::min(hi.x, clip.x) - lo.x, std::min(hi.y, clip.y) - lo.y},
            id, enabled, false, ImGui::GetItemID()});
    }
    ImGui::EndDisabled(); ImGui::PopID();
    return pressed;
}

void open_update_link(const Json& offer, const char* field) {
    // Links come only from select_windows_release's exact repository/asset checks.
    const auto url = wide(offer.at(field).get<std::string>());
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(panel, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        MessageBoxW(panel, wide(shin::ui::tr("Cannot open the release in your browser.")).c_str(), L"Shin", MB_OK | MB_ICONWARNING);
}

fs::path windows_update_folder;
Json windows_update_result=Json::object();
Clock::time_point windows_update_poll;
std::future<Json> windows_update_review;
std::string windows_update_review_error;
std::string windows_update_launch_error;

template<class Action> void run_windows_update_action(Action&& action) {
    windows_update_launch_error.clear();
    try { action(); }
    catch(const Error& error) { windows_update_launch_error=error.code; }
    catch(const std::exception&) { windows_update_launch_error="update-helper-launch-failed"; }
}

Json windows_project_preparation(const fs::path& prefix, bool check_only) {
    const auto manifest=windows_update::verify_installed(prefix,SHIN_OBSERVER_VERSION);
    std::vector<std::string> arguments{"prepare-projects","--home",management.home.string()};
    if(check_only) arguments.push_back("--check");
    const auto result=windows_command(prefix/"bin/shin-ce.exe",arguments,std::chrono::seconds(1105));
    if(result.value("version","")!=manifest.at("version").get_ref<const std::string&>() ||
        result.value("core_version","")!=manifest.at("core_version").get_ref<const std::string&>())
        throw Error("update-installation-changed","Project preparation belongs to another installation.");
    return result;
}
Json windows_setup_result(Json preparation) {
    return {{"kind","setup-only"},{"phase",preparation.value("phase","review-required")},
        {"error_code",preparation.value("error",Json::object()).value("code","")},
        {"project_preparation",std::move(preparation)}};
}
void read_windows_setup_result() {
    const auto path=management.home/"windows-setup";
    if(!fs::exists(path)) return;
    StateStore state(path,false);auto preparation=state.load();
    if(!preparation) return;
    if(!state.writer_present() && (preparation->value("phase","")=="preparing" ||
        preparation->value("version","")!=SHIN_OBSERVER_VERSION || preparation->value("core_version","")!=SHIN_VERSION)) {
        (*preparation)["phase"]="review-required";
        (*preparation)["error"]={{"code","installation-preparation-unavailable"}};
    }
    windows_update_result=windows_setup_result(std::move(*preparation));
}

Json review_windows_installation(const fs::path& folder,const std::string& identity,const fs::path& prefix) {
    if(directory_identity(folder,true)!=identity) throw Error("update-stage-changed","Update state changed.");
    StateStore store(folder,true);store.recover_pending(); // Refuses a live helper.
    auto saved=store.load().value();
    if(saved.value("schema_version","")!="shin.windows-update.v1" || directory_identity(prefix)!=directory_identity(fs::path(saved.at("prefix").get<std::string>())))
        throw Error("update-review-changed","The installation or review changed.");
    shin::windows::ReadFile file(prefix/"shin-kit.json");file.require_machine_owned();
    const auto manifest=parse_json(file.read(kMaximumStateBytes));
    const auto version=manifest.at("version").get<std::string>();
    if(version==saved.at("offer").at("version").get_ref<const std::string&>()) {
        const auto code=saved.value("installer_exit_code",DWORD(-1));
        const auto prepared=windows_project_preparation(prefix,code!=0 && code!=3010 && code!=1641);
        saved["project_preparation"]=prepared;
        // Independently verify the installed payload even when project preparation
        // reports pending work. Its failure remains visible below the update result.
        windows_update::verify_installed(prefix,version);
        saved["projects_pending"]=!prepared.value("ok",false);
        saved["phase"]="completed";saved["installed_version"]=version;saved.erase("error_code");
    } else {
        const std::set<std::string> before_install{"reviewed","checking","downloading","verifying","waiting-monitor"};
        const std::set<std::string> no_install{"update-uac-cancelled","update-offer-changed","update-download-invalid","update-helper-launch-failed","update-monitor-running"};
        if(version!=saved.at("from_version").get_ref<const std::string&>() || (!before_install.contains(saved.value("phase","")) &&
            !(saved.value("phase","")=="review-required" && no_install.contains(saved.value("error_code","")))))
            throw Error("update-outcome-unknown","Review Windows Installer before retrying. Its outcome could not be confirmed.");
        saved["phase"]="acknowledged";
    }
    store.save(saved);return saved;
}

void poll_windows_installation() {
    if(test_mode || Clock::now()<windows_update_poll) return;
    windows_update_poll=Clock::now()+std::chrono::seconds(1);
    if(windows_update_folder.empty()) {
        const auto saved=settings.value("windows_update_folder","");
        if(saved.empty()) {
            try {read_windows_setup_result();} catch(...) {windows_update_result={{"kind","setup-only"},{"phase","review-required"},{"error_code","update-state-unavailable"}};}
            return;
        }
        const fs::path folder(saved);
        if(folder.parent_path()!=management.home/"updates/windows") return;
        windows_update_folder=folder;
    }
    try {
        if(directory_identity(windows_update_folder,true)!=settings.value("windows_update_identity","")) throw Error("update-stage-changed","Update state changed.");
        StateStore store(windows_update_folder,false);
        windows_update_result=store.load().value_or(Json::object());
        if(windows_update_result.value("schema_version","")!="shin.windows-update.v1") throw Error("update-state-invalid","Unknown update state.");
        const auto phase=windows_update_result.value("phase","");
        if((phase=="completed" || phase=="acknowledged") && windows_update_result.value("installed_version","")!=SHIN_OBSERVER_VERSION)
            read_windows_setup_result();
        FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
        const auto identity=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
        if(windows_update_result.value("phase","")=="waiting-monitor" && windows_update_result.value("monitor_pid",0UL)==GetCurrentProcessId() &&
            windows_update_result.value("monitor_created",0ULL)==identity) PostMessageW(panel,WM_APP+4,0,0);
    } catch(...) { windows_update_result={{"phase","review-required"},{"error_code","update-state-unavailable"}}; }
}
void install_windows_update(const Json& offer) {
    using shin::ui::tr;
    poll_windows_installation();
    const auto previous=windows_update_result.value("phase","");
    if(!previous.empty() && previous!="completed" && previous!="acknowledged" && previous!="review-required")
        throw Error("update-in-progress","An update is already in progress. Review its saved result.");
    if(previous=="review-required") throw Error("update-review-required","Review the previous update before retrying.");
    if(test_mode || !offer.value("available",false)) return;
    modal=true;
    const auto confirmed=MessageBoxW(panel,wide(tr("Download, verify and install this update? Windows Installer will request UAC. Projects, settings and history are preserved.")).c_str(),L"Shin",MB_YESNO|MB_ICONQUESTION);
    modal=false;if(confirmed!=IDYES) return;
    const auto prefix=current_executable_path().parent_path().parent_path();
    windows_update::verify_registered_msi_installation(prefix);
    ensure_private_directory(management.home/"updates");ensure_private_directory(management.home/"updates/windows");
    const auto folder=management.home/"updates/windows"/("update-"+digest(std::to_string(now_ms())+std::to_string(GetCurrentProcessId())).substr(0,24));
    windows_update::stage_update_helper(prefix,folder);
    FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    const auto identity=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    { StateStore store(folder,true);store.recover_pending();
      store.save({{"schema_version","shin.windows-update.v1"},{"phase","reviewed"},{"offer",offer},{"prefix",prefix.string()},
        {"from_version",SHIN_OBSERVER_VERSION},{"monitor_pid",GetCurrentProcessId()},{"monitor_created",identity},{"language",shin::ui::language()==shin::ui::Language::ru?"ru":"en"}}); }
    settings["windows_update_folder"]=folder.string();settings["windows_update_identity"]=directory_identity(folder,true);save_settings();
    windows_update_folder=folder;windows_update_poll={};
    const auto executable=folder/"updater.exe";
    const std::wstring arguments=L"--review \""+folder.wstring()+L"\"";
    SHELLEXECUTEINFOW start{};start.cbSize=sizeof(start);start.fMask=SEE_MASK_NOCLOSEPROCESS;start.hwnd=panel;
    start.lpVerb=L"open";start.lpFile=executable.c_str();start.lpParameters=arguments.c_str();start.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&start)) {
        {StateStore store(folder,true);store.recover_pending();auto state=store.load().value();state["phase"]="review-required";state["error_code"]="update-helper-launch-failed";store.save(state);}
        throw Error("update-helper-launch-failed","Could not open the update progress window.");
    }
    if(start.hProcess) CloseHandle(start.hProcess);
    poll_windows_installation();
}

void render_windows_updates(const WindowsUpdateSnapshot& state) {
    using shin::ui::tr;
    ui_actions.clear();
    if(windows_update_review.valid() && windows_update_review.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {windows_update_result=windows_update_review.get();windows_update_poll={};}
        catch(const Error& error) {windows_update_review_error=error.code;}
        catch(...) {windows_update_review_error="update-outcome-unknown";}
    }
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Shin updates", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto size = ImGui::GetIO().DisplaySize;
    const bool compact_view = size.y < 600 || size.x < 400;
    ImGui::PushFont(nullptr, compact_view ? 14.0f : 17.0f);
    ImGui::GetWindowDrawList()->AddRect({.5f, .5f}, {size.x - .5f, size.y - .5f}, ImGui::GetColorU32(sand), 9);
    const auto origin = ImGui::GetCursorScreenPos();
    draw_brand({origin.x, origin.y + 3}, 32);
    ImGui::Dummy({32, 40}); ImGui::SameLine(); ImGui::BeginGroup();
    ImGui::PushFont(nullptr, size.x < 400 ? 17.0f : 24.0f);
    ImGui::TextUnformatted(tr("Shin updates")); ImGui::PopFont();
    ImGui::PushFont(nullptr, 13);
    ImGui::TextColored(muted, "%s · v%s", tr("Current version"), SHIN_OBSERVER_VERSION);
    ImGui::PopFont(); ImGui::EndGroup();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    bool automatic=settings.value("automatic_update_checks",true);
    if(ImGui::Checkbox(tr("Automatically check for updates"),&automatic) && !test_mode) { settings["automatic_update_checks"]=automatic;save_settings(); }
    const float back_width = ImGui::CalcTextSize(tr("Back to Monitor")).x + 24;
    const float check_width = ImGui::CalcTextSize(tr("Check again")).x + 24;
    const bool wrap_footer = back_width + check_width + ImGui::GetStyle().ItemSpacing.x > ImGui::GetContentRegionAvail().x;
    const float button_height = size.y < 600 ? 30.0f : 36.0f;
    const float footer_height = wrap_footer ? 2 * button_height + ImGui::GetStyle().ItemSpacing.y + 2 : button_height + 2;
    const float footer = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - footer_height;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, raised);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9);
    ImGui::BeginChild("update-content", {0, std::max(1.0f, footer - ImGui::GetCursorPosY() - 9)}, ImGuiChildFlags_Borders);
    const bool busy = state.state == WindowsUpdateState::checking;
    const bool offer = state.state == WindowsUpdateState::offer;
    const char* title = busy ? tr("Checking for updates…") : offer ? tr("A Shin update is available") :
        state.state == WindowsUpdateState::current ? tr("Shin is up to date") :
        state.state == WindowsUpdateState::failed ? tr("Could not check for updates") : tr("Check for updates…");
    ImGui::PushTextWrapPos(); ImGui::TextColored(offer ? blue : ink, "%s", title); ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if(!windows_update_launch_error.empty()) {
        ImGui::TextWrapped("%s",tr("Could not start the update. Monitor is still running."));
        ImGui::TextWrapped("%s",windows_update_launch_error.c_str());
        if(update_button(tr("Copy diagnostic report"),208))
            platform_copy(Json{{"command","update-launch"},{"ok",false},{"error_code",windows_update_launch_error}}.dump(2));
    }
    if (offer) {
        ImGui::TextColored(blue, "v%s  ->  v%s", SHIN_OBSERVER_VERSION, state.offer.at("version").get_ref<const std::string&>().c_str());
        ImGui::TextWrapped("%s", tr("Shin downloads and verifies the MSI, then opens Windows Installer. Confirm UAC to install. The installer owns progress and rollback; projects, settings and history stay in place."));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, blue);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{blue.x * .86f, blue.y * .86f, blue.z * .86f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{blue.x * .72f, blue.y * .72f, blue.z * .72f, 1});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
        poll_windows_installation();
        const auto phase=windows_update_result.value("phase","");
        const bool download = update_button(tr("Download and install"), 202,!windows_update_review.valid() && (phase.empty() || phase=="completed" || phase=="acknowledged"));
        ImGui::PopStyleColor(4);
        if (download) run_windows_update_action([&] {install_windows_update(state.offer);});
        if (update_button(tr("Release notes"), 204)) open_update_link(state.offer, "release_url");
        if (update_button(tr("Technical details"), 209)) windows_update_details_open = !windows_update_details_open;
        if (windows_update_details_open && update_button(tr("Download checksum"), 203)) open_update_link(state.offer, "checksum_url");
    } else {
        ImGui::TextWrapped("%s", busy ? tr("Shin continues to work while the release is checked. You can leave this window open or close it.") :
            state.state == WindowsUpdateState::failed ? tr("GitHub is unavailable or no complete release feed could be read. Shin continues to work offline. Try again later.") :
            tr("Checks public Windows releases on GitHub. Installation starts only after your confirmation."));
    }
    poll_windows_installation();
    if(!windows_update_result.empty()) {
        ImGui::Separator();
        const bool setup_only=windows_update_result.value("kind","")=="setup-only";
        const auto phase=windows_update_result.value("phase","");
        ImGui::TextWrapped("%s",setup_only?tr("Project preparation"):tr("Last update result…"));
        const std::map<std::string,const char*> phases{{"reviewed",tr("Checking reviewed release…")},{"checking",tr("Checking reviewed release…")},
            {"downloading",tr("Downloading update…")},{"verifying",tr("Verifying download…")},{"waiting-monitor",tr("Waiting for Monitor to close…")},
            {"preparing-projects",tr("Preparing projects…")},{"preparing",tr("Preparing projects…")},
            {"installing",tr("Windows Installer is installing the update. Confirm UAC to continue.")},
            {"completed",tr("Update completed. Restart persistent MCP clients.")},{"acknowledged",tr("Operation completed.")}};
        const auto found=phases.find(windows_update_result.value("phase",""));
        ImGui::TextWrapped("%s",setup_only?(phase=="completed"?tr("Projects ready to use"):
            phase=="preparing"?tr("Preparing projects…"):tr("Project preparation needs attention.")):
            found==phases.end()?tr("Update was not confirmed. Check the saved result before retrying."):found->second);
        const bool projects_pending=windows_update_result.value("projects_pending",false);
        if(projects_pending) ImGui::TextWrapped("%s",tr("Project preparation needs attention."));
        const auto code=windows_update_result.value("error_code","");
        if(!code.empty()) ImGui::TextWrapped("%s",code.c_str());
        for(const auto& project:windows_update_result.value("project_preparation",Json::object()).value("projects",Json::array())) {
            const auto ok=project.value("ok",false);
            ImGui::TextColored(ok?green:red,"%s",ok?tr("ready"):tr("Needs attention"));
            ImGui::SameLine();ImGui::TextWrapped("%s",project.value("root","").c_str());
            if(!ok) ImGui::TextWrapped("%s",project.value("error_code","").c_str());
        }
        if(update_button(tr("Copy diagnostic report"),206)) platform_copy(windows_update_result.dump(2));
        if((projects_pending || (phase!="completed" && phase!="acknowledged")) && update_button((setup_only || projects_pending)?tr("Retry project preparation"):tr("Review previous update"),207,!windows_update_review.valid()) && !test_mode) {
            const auto folder=windows_update_folder,prefix=current_executable_path().parent_path().parent_path();
            const auto identity=settings.value("windows_update_identity","");windows_update_review_error.clear();
            windows_update_review=std::async(std::launch::async,[folder,identity,prefix,setup_only]{
                return setup_only?windows_setup_result(windows_project_preparation(prefix,false)):review_windows_installation(folder,identity,prefix);
            });
        }
        if(windows_update_review.valid()) ImGui::TextWrapped("%s",tr("Operation in progress…"));
        if(!windows_update_review_error.empty()) ImGui::TextWrapped("%s",windows_update_review_error.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    ImGui::SetCursorPosY(footer);
    if (update_button(tr("Back to Monitor"), 201)) { updates_page = false; if (!test_mode) show_panel(); }
    if (!wrap_footer) ImGui::SameLine();
    if (update_button(tr("Check again"), 205, !busy) && !test_mode) release_check->check();
    ImGui::PopFont();
    ImGui::End();
}
