#pragma once

// Public metadata only. The reviewed MSI and checksum are opened in the user's
// browser; this route never downloads, executes or elevates an installer.
Json fetch_windows_release() {
    struct Close { void operator()(void* value) const { if (value) WinHttpCloseHandle(value); } };
    using Internet = std::unique_ptr<void, Close>;
    const auto require = [](bool ok) {
        if (!ok) throw Error("update-check-failed", "The public release feed could not be read.");
    };
    Internet session(WinHttpOpen(L"Shin-Monitor/" SHIN_OBSERVER_VERSION,
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

void open_updates() {
    updates_page = true;
    if (!release_check) release_check = std::make_unique<WindowsReleaseCheck>();
    release_check->check();
    show_panel();
}

bool update_button(const char* label, int id, bool enabled = true) {
    ImGui::PushID(id); ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, {ImGui::GetContentRegionAvail().x, 28});
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

void render_windows_updates(const WindowsUpdateSnapshot& state) {
    using shin::ui::tr;
    ui_actions.clear();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Shin updates", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto origin = ImGui::GetCursorScreenPos();
    draw_brand({origin.x, origin.y + 3}, 32);
    ImGui::Dummy({32, 40}); ImGui::SameLine(); ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetIO().DisplaySize.x < 400 ? 17.0f : 22.0f);
    ImGui::TextUnformatted(tr("Shin updates")); ImGui::PopFont();
    ImGui::PushFont(nullptr, 11);
    ImGui::TextColored(muted, "%s · v%s", tr("Current version"), SHIN_OBSERVER_VERSION);
    ImGui::PopFont(); ImGui::EndGroup();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    const float footer = ImGui::GetWindowHeight() - 42;
    ImGui::BeginChild("update-content", {0, std::max(1.0f, footer - ImGui::GetCursorPosY() - 6)});
    const bool busy = state.state == WindowsUpdateState::checking;
    const bool offer = state.state == WindowsUpdateState::offer;
    const char* title = busy ? tr("Checking for updates…") : offer ? tr("A Shin update is available") :
        state.state == WindowsUpdateState::current ? tr("Shin is up to date") :
        state.state == WindowsUpdateState::failed ? tr("Could not check for updates") : tr("Check for updates…");
    ImGui::PushTextWrapPos(); ImGui::TextColored(offer ? sand : ink, "%s", title); ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (offer) {
        ImGui::TextColored(sand, "v%s  ->  v%s", SHIN_OBSERVER_VERSION, state.offer.at("version").get_ref<const std::string&>().c_str());
        ImGui::TextWrapped("%s", tr("Download the MSI and its checksum. Verify the download, close Monitor, then run the installer and confirm Windows UAC. Projects, settings and history stay in place."));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, sand); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ink);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, muted); ImGui::PushStyleColor(ImGuiCol_Text, background);
        const bool download = update_button(tr("Download MSI"), 202);
        ImGui::PopStyleColor(4);
        if (download) open_update_link(state.offer, "archive_url");
        if (update_button(tr("Download checksum"), 203)) open_update_link(state.offer, "checksum_url");
        if (update_button(tr("Release notes"), 204)) open_update_link(state.offer, "release_url");
    } else {
        ImGui::TextWrapped("%s", busy ? tr("Shin continues to work while the release is checked. You can leave this window open or close it.") :
            state.state == WindowsUpdateState::failed ? tr("GitHub is unavailable or no complete release feed could be read. Shin continues to work offline. Try again later.") :
            tr("Checks public Windows releases on GitHub. Installation starts only when you run the downloaded MSI."));
    }
    ImGui::Spacing();
    if (update_button(tr("Check again"), 205, !busy) && !test_mode) release_check->check();
    ImGui::EndChild();
    ImGui::SetCursorPosY(footer);
    if (update_button(tr("Back to Monitor"), 201)) { updates_page = false; if (!test_mode) show_panel(); }
    ImGui::End();
}
