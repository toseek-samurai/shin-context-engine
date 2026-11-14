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

windows_update::UpdateResult windows_update_result;
Json windows_setup_status=Json::object();
Clock::time_point windows_update_poll;
std::future<void> windows_update_operation;
std::unique_ptr<void,decltype(&CloseHandle)> windows_update_process(nullptr,&CloseHandle);
std::string windows_update_review_error;
std::string windows_update_launch_error;

template<class Action> void run_windows_update_action(Action&& action) {
    windows_update_launch_error.clear();
    try { action(); }
    catch(const Error& error) { windows_update_launch_error=error.code; }
    catch(const std::exception&) { windows_update_launch_error="update-helper-launch-failed"; }
}

Json read_windows_setup_result(const fs::path& home) {
    const auto path = home/"windows-setup";
    if (!fs::exists(path)) return Json::object();
    StateStore state(path, false);
    auto preparation = state.load().value_or(Json::object());
    const bool current = preparation.value("version", "") == SHIN_OBSERVER_VERSION &&
        preparation.value("core_version", "") == SHIN_VERSION;
    if (state.writer_present()) {
        if (!current) preparation = Json::object();
        preparation["phase"] = "preparing";
        preparation["ok"] = false;
        return preparation;
    }
    if (preparation.empty()) return preparation;
    if (!current)
        return {{"phase","review-required"},{"error",{{"code","installation-preparation-unavailable"}}}};
    if (preparation.value("phase", "") == "preparing") {
        preparation["phase"] = "review-required";
        preparation["ok"] = false;
        preparation["error"] = {{"code","installation-preparation-unavailable"}};
    }
    return preparation;
}

void check_windows_update_binding(const fs::path& folder,const std::string& identity,const fs::path& home) {
    if(folder.parent_path()!=home/"updates/windows" || identity.empty())
        throw Error("update-stage-changed","Update state changed.");
}

void review_windows_installation(const fs::path& folder,const std::string& identity,const fs::path& prefix,const fs::path& home) {
    check_windows_update_binding(folder,identity,home);
    StateStore store(folder,true,identity);
    store.recover_pending(); // Refuses a live helper.
    auto saved=store.load().value();
    if(!windows_update::observe_update(saved,SHIN_OBSERVER_VERSION,false).needs_review) return;
    if(directory_identity(prefix)!=directory_identity(fs::path(saved.at("prefix").get<std::string>())))
        throw Error("update-review-changed","The installation or review changed.");
    const auto manifest=windows_update::verify_saved_update_installation(prefix,saved);
    const auto& version=manifest.at("version").get_ref<const std::string&>();
    if(version!=SHIN_OBSERVER_VERSION || manifest.at("core_version")!=SHIN_VERSION)
        throw Error("update-installation-changed","Open the installed Monitor to review this update.");
    if(version==saved.at("offer").at("version").get_ref<const std::string&>()) {
        windows_update::record_installed_update(saved,version,{{"ok",false}});
    } else {
        windows_update::acknowledge_previous_installation(saved,version);
    }
    store.save(saved);
}

void poll_windows_installation(bool force=false) {
    if(test_mode) return;
    if(windows_update_operation.valid() && windows_update_operation.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {windows_update_operation.get();}
        catch(const Error& error) {windows_update_review_error=error.code;}
        catch(...) {windows_update_review_error="update-outcome-unknown";}
        force=true;
    }
    if(!force && Clock::now()<windows_update_poll) return;
    windows_update_poll=Clock::now()+std::chrono::seconds(1);
    try { windows_setup_status=read_windows_setup_result(management.home); }
    catch(...) {
        windows_setup_status={{"phase","review-required"},{"error",{{"code","installation-preparation-unavailable"}}}};
    }
    try {
        const fs::path folder(settings.value("windows_update_folder",""));
        if(folder.empty()) { windows_update_result={};return; }
        const auto identity=settings.value("windows_update_identity","");
        check_windows_update_binding(folder,identity,management.home);
        StateStore store(folder,false,identity);
        auto saved=store.load().value_or(Json::object());
        const bool starting=saved.value("phase",Json())=="reviewed" && windows_update_process &&
            WaitForSingleObject(windows_update_process.get(),0)==WAIT_TIMEOUT;
        if(!starting) windows_update_process.reset();
        windows_update_result=windows_update::observe_update(std::move(saved),SHIN_OBSERVER_VERSION,
            starting || store.writer_present());
    } catch(...) {
        windows_update_result={{{"phase","review-required"},{"error_code","update-state-unavailable"}},false,true};
    }
}

bool windows_update_waits_for_monitor() {
    const auto& receipt=windows_update_result.receipt;
    if(!windows_update_result.active || windows_update_operation.valid() ||
        receipt.value("phase",Json())!="waiting-monitor") return false;
    const auto pid=receipt.value("monitor_pid",Json());
    const auto start=receipt.value("monitor_created",Json());
    if(!pid.is_number_unsigned() || pid.get<std::uint64_t>()!=GetCurrentProcessId() || !start.is_number_unsigned()) return false;
    FILETIME created{},exited{},kernel{},user{};
    return GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user) &&
        start.get<std::uint64_t>()==((static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime);
}

bool windows_install_available() {
    return windows_update::can_install_update(windows_update_result,windows_setup_status,windows_update_operation.valid());
}

enum class WindowsUpdateAction { none, review, prepare };

const Json& windows_project_preparation_result() {
    const auto& receipt=windows_update_result.receipt;
    const auto prepared=receipt.find("project_preparation");
    if(prepared!=receipt.end() && prepared->is_object() && !windows_setup_status.contains("version") &&
        windows_setup_status.value("phase","")!="preparing") return *prepared;
    return windows_setup_status;
}

WindowsUpdateAction windows_update_action() {
    if (windows_update_result.active || windows_update_operation.valid() ||
        windows_setup_status.value("phase","")=="preparing") return WindowsUpdateAction::none;
    if (windows_update_result.needs_review) return WindowsUpdateAction::review;
    const auto& preparation=windows_project_preparation_result();
    return !preparation.empty() && !preparation.value("ok",false) ? WindowsUpdateAction::prepare : WindowsUpdateAction::none;
}

void install_windows_update(const Json& offer) {
    using shin::ui::tr;
    poll_windows_installation(true);
    if(!windows_install_available()) {
        if(windows_update_result.needs_review) throw Error("update-review-required","Review the previous update before retrying.");
        throw Error("update-in-progress","An update or project preparation is already in progress.");
    }
    if(test_mode || !offer.value("available",false)) return;
    modal=true;
    const auto confirmed=MessageBoxW(panel,wide(tr("Download, verify and install this update? Windows Installer will request UAC. Projects, settings and history are preserved.")).c_str(),L"Shin",MB_YESNO|MB_ICONQUESTION);
    modal=false;if(confirmed!=IDYES) return;
    poll_windows_installation(true);
    if(!windows_install_available())
        throw Error("update-review-changed","The update or preparation state changed. Review it again.");
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
    windows_update_poll={};
    windows_update_review_error.clear();
    const auto executable=folder/"updater.exe";
    const std::wstring arguments=L"--review \""+folder.wstring()+L"\"";
    SHELLEXECUTEINFOW start{};start.cbSize=sizeof(start);start.fMask=SEE_MASK_NOCLOSEPROCESS;start.hwnd=panel;
    start.lpVerb=L"open";start.lpFile=executable.c_str();start.lpParameters=arguments.c_str();start.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&start)) {
        {StateStore store(folder,true);store.recover_pending();auto state=store.load().value();state["phase"]="review-required";state["error_code"]="update-helper-launch-failed";store.save(state);}
        throw Error("update-helper-launch-failed","Could not open the update progress window.");
    }
    windows_update_process.reset(start.hProcess);
    poll_windows_installation();
}

void render_windows_updates(const WindowsUpdateSnapshot& state) {
    using shin::ui::tr;
    ui_actions.clear();
    poll_windows_installation();
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
        const bool download = update_button(tr("Download and install"), 202,windows_install_available());
        ImGui::PopStyleColor(4);
        if (download && !test_mode) run_windows_update_action([&] {install_windows_update(state.offer);});
        if (update_button(tr("Release notes"), 204)) open_update_link(state.offer, "release_url");
        if (update_button(tr("Technical details"), 209)) windows_update_details_open = !windows_update_details_open;
        if (windows_update_details_open && update_button(tr("Download checksum"), 203)) open_update_link(state.offer, "checksum_url");
    } else {
        ImGui::TextWrapped("%s", busy ? tr("Shin continues to work while the release is checked. You can leave this window open or close it.") :
            state.state == WindowsUpdateState::failed ? tr("GitHub is unavailable or no complete release feed could be read. Shin continues to work offline. Try again later.") :
            tr("Checks public Windows releases on GitHub. Installation starts only after your confirmation."));
    }
    const auto& receipt=windows_update_result.receipt;
    const auto& preparation=windows_project_preparation_result();
    if(!receipt.empty() || !preparation.empty()) {
        ImGui::Separator();
        if(!receipt.empty()) {
            ImGui::TextWrapped("%s",tr("Last update result…"));
            const std::map<std::string,const char*> phases{{"reviewed",tr("Checking reviewed release…")},{"checking",tr("Checking reviewed release…")},
                {"downloading",tr("Downloading update…")},{"verifying",tr("Verifying download…")},{"waiting-monitor",tr("Waiting for Monitor to close…")},
                {"preparing-projects",tr("Preparing projects…")},
                {"installing",tr("Windows Installer is installing the update. Confirm UAC to continue.")},
                {"completed",tr("Update completed. Restart persistent MCP clients.")},{"acknowledged",tr("Previous installation confirmed.")}};
            const auto phase=receipt.value("phase",Json());
            const auto found=phase.is_string() ? phases.find(phase.get_ref<const std::string&>()) : phases.end();
            ImGui::TextWrapped("%s",windows_update_result.needs_review || found==phases.end() ?
                tr("Update was not confirmed. Check the saved result before retrying.") : found->second);
            const auto code=receipt.value("error_code",Json());
            if(code.is_string() && !code.get_ref<const std::string&>().empty())
                ImGui::TextWrapped("%s",code.get_ref<const std::string&>().c_str());
        }
        if(!preparation.empty()) {
            if(receipt.empty()) ImGui::TextWrapped("%s",tr("Project preparation"));
            const auto phase=preparation.value("phase","");
            ImGui::TextWrapped("%s",preparation.value("ok",false)?tr("Projects ready to use"):
                phase=="preparing" || (windows_update_result.active && phase.empty()) ?
                    tr("Preparing projects…"):tr("Project preparation needs attention."));
            const auto code=preparation.value("error",Json::object()).value("code","");
            if(!code.empty()) ImGui::TextWrapped("%s",code.c_str());
        }
        for(const auto& project:preparation.value("projects",Json::array())) {
            const auto ok=project.value("ok",false);
            ImGui::TextColored(ok?green:red,"%s",ok?tr("ready"):tr("Needs attention"));
            ImGui::SameLine();ImGui::TextWrapped("%s",project.value("root","").c_str());
            if(!ok) ImGui::TextWrapped("%s",project.value("error_code","").c_str());
        }
        if(update_button(tr("Copy diagnostic report"),206))
            platform_copy(Json{{"update",receipt},{"project_preparation",preparation}}.dump(2));
        const auto action=windows_update_action();
        if(action!=WindowsUpdateAction::none && update_button(action==WindowsUpdateAction::prepare ?
            tr("Retry project preparation"):tr("Review previous update"),207) && !test_mode) {
            const fs::path folder(settings.value("windows_update_folder",""));
            const auto prefix=current_executable_path().parent_path().parent_path();
            const auto identity=settings.value("windows_update_identity","");
            const auto home=management.home;
            windows_update_review_error.clear();
            windows_update_operation=std::async(std::launch::async,[folder,identity,prefix,home,action]{
                if(action==WindowsUpdateAction::review) {
                    review_windows_installation(folder,identity,prefix,home);
                } else {
                    const auto manifest=windows_update::verify_installed(prefix,SHIN_OBSERVER_VERSION);
                    prepare_installed_windows_projects(prefix,home,manifest);
                }
            });
        }
        if(windows_update_operation.valid()) ImGui::TextWrapped("%s",tr("Operation in progress…"));
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
