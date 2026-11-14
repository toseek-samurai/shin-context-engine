#pragma once
// Disposable native host qualification: no installed home, service, grant or
// live user preference store is opened. Only the explicit output owns fixture
// preferences. Exercise the production window procedures.
void save_quick_test_frame(const fs::path& path) {
    RECT bounds{}; GetClientRect(quick_popup, &bounds);
    struct Surface {
        HDC dc = CreateCompatibleDC(nullptr); HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr;
        ~Surface() { if (previous) SelectObject(dc, previous); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
    } surface;
    BITMAPINFO pixels{}; pixels.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pixels.bmiHeader.biWidth = bounds.right; pixels.bmiHeader.biHeight = -bounds.bottom;
    pixels.bmiHeader.biPlanes = 1; pixels.bmiHeader.biBitCount = 32; pixels.bmiHeader.biCompression = BI_RGB;
    void* storage = nullptr;
    surface.bitmap = CreateDIBSection(surface.dc, &pixels, DIB_RGB_COLORS, &storage, nullptr, 0);
    if (!surface.dc || !surface.bitmap) throw std::runtime_error("Cannot allocate quick status render fixture.");
    surface.previous = SelectObject(surface.dc, surface.bitmap);
    RedrawWindow(quick_popup, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (!PrintWindow(quick_popup, surface.dc, PW_CLIENTONLY)) throw std::runtime_error("Cannot render native quick status controls.");
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Cannot encode quick status.");
    ComPtr<IWICBitmap> bitmap;
    checked(factory->CreateBitmapFromHBITMAP(surface.bitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap), "Cannot map quick status pixels.");
    ComPtr<IWICStream> stream; checked(factory->CreateStream(&stream), "Cannot create quick status PNG stream.");
    checked(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Cannot open quick status PNG.");
    ComPtr<IWICBitmapEncoder> encoder; checked(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Cannot create quick status PNG encoder.");
    checked(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Cannot initialize quick status PNG encoder.");
    ComPtr<IWICBitmapFrameEncode> frame; checked(encoder->CreateNewFrame(&frame, nullptr), "Cannot create quick status PNG frame.");
    checked(frame->Initialize(nullptr), "Cannot initialize quick status PNG frame.");
    checked(frame->SetSize(static_cast<UINT>(bounds.right), static_cast<UINT>(bounds.bottom)), "Cannot size quick status PNG.");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    checked(frame->SetPixelFormat(&format), "Cannot select quick status PNG format.");
    ComPtr<IWICBitmapSource> converted;
    checked(WICConvertBitmapSource(format, bitmap.Get(), &converted), "Cannot convert quick status PNG pixels.");
    checked(frame->WriteSource(converted.Get(), nullptr), "Cannot write quick status PNG pixels.");
    checked(frame->Commit(), "Cannot commit quick status PNG frame."); checked(encoder->Commit(), "Cannot commit quick status PNG.");
}
int window_shell_test(const fs::path& output) {
    fs::create_directories(output);
    Json checks = Json::array();
    const auto check = [&](const char* name, const auto& action) {
        action(); checks.push_back(name);
    };
    const auto require = [](bool ok, const char* message) { if (!ok) throw std::runtime_error(message); };
    const auto rectangle = [](HWND window) { RECT bounds{}; GetWindowRect(window, &bounds); return bounds; };
    check("desktop_native_frame_and_task_switching", [&] {
        const auto style = GetWindowLongPtrW(panel, GWL_STYLE), extended = GetWindowLongPtrW(panel, GWL_EXSTYLE);
        require((style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW && !(style & WS_POPUP), "Desktop has no ordinary native frame.");
        require((extended & WS_EX_APPWINDOW) && !(extended & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)), "Desktop is excluded from normal task switching.");
        require(!GetWindow(panel, GW_OWNER), "Desktop unexpectedly belongs to a transient popup.");
        wchar_t caption[64]{}; GetWindowTextW(panel, caption, 64);
        require(std::wstring(caption) == L"Shin Monitor", "Desktop caption contains a private home identity.");
        show_panel();
        require(IsWindowVisible(panel), "Desktop did not open.");
    });
    check("desktop_survives_focus_loss_and_escape", [&] {
        SendMessageW(panel, WM_ACTIVATE, WA_INACTIVE, 0);
        require(IsWindowVisible(panel), "Desktop disappeared on focus loss.");
        updates_page = true;
        SendMessageW(panel, WM_KEYDOWN, VK_ESCAPE, 0);
        require(IsWindowVisible(panel) && !updates_page, "Escape did not return from a detail page without hiding the desktop.");
    });
    check("desktop_close_and_placement_round_trip", [&] {
        ShowWindow(panel, SW_RESTORE);
        MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST), &info);
        const int width = std::min(640, static_cast<int>(info.rcWork.right - info.rcWork.left));
        const int height = std::min(480, static_cast<int>(info.rcWork.bottom - info.rcWork.top));
        const int x = info.rcWork.left + std::min(43, static_cast<int>(info.rcWork.right - info.rcWork.left) - width);
        const int y = info.rcWork.top + std::min(37, static_cast<int>(info.rcWork.bottom - info.rcWork.top) - height);
        SetWindowPos(panel, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto moved = rectangle(panel);
        SendMessageW(panel, WM_EXITSIZEMOVE, 0, 0);
        const auto remembered = settings.at("windows_monitor_window");
        require(remembered.at("width").get<int>() == width && remembered.at("height").get<int>() == height &&
            remembered.at("x").get<int>() == x && remembered.at("y").get<int>() == y, "Moved/resized geometry was not saved.");
        SendMessageW(panel, WM_CLOSE, 0, 0);
        require(!IsWindowVisible(panel) && IsWindow(panel) && IsWindow(quick_popup), "Close destroyed the tray host.");
        restoring_desktop_placement = true;
        SetWindowPos(panel, nullptr, info.rcWork.left + 20, info.rcWork.top + 20, width + 10, height + 10, SWP_NOZORDER | SWP_NOACTIVATE);
        restoring_desktop_placement = false; desktop_placement_ready = false;
        show_panel();
        const auto restored = rectangle(panel);
        require(EqualRect(&moved, &restored), "Desktop placement did not survive closing and reopening.");
    });
    check("desktop_minimize_maximize_and_reopen", [&] {
        const auto normal = rectangle(panel);
        ShowWindow(panel, SW_MAXIMIZE); capture_desktop_placement(true);
        require(IsZoomed(panel) && settings.at("windows_monitor_window").at("maximized").get<bool>(), "Maximized state was not retained.");
        ShowWindow(panel, SW_MINIMIZE);
        require(IsIconic(panel) && !desktop_renderable(), "Minimized desktop still requests graphics frames.");
        show_panel(); require(!IsIconic(panel) && IsZoomed(panel) && desktop_renderable(), "Reopening lost the maximized desktop state.");
        ShowWindow(panel, SW_RESTORE);
        const auto restored = rectangle(panel);
        require(EqualRect(&normal, &restored), "Maximize/minimize lost the normal desktop rectangle.");
    });
    check("quick_panel_is_independent_and_dismisses_only_itself", [&] {
        const auto desktop = rectangle(panel);
        show_quick_panel();
        require(IsWindowVisible(quick_popup) && IsWindowVisible(panel), "Quick status replaced the desktop.");
        require((GetWindowLongPtrW(quick_popup, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0, "Quick status became a taskbar window.");
        const auto after = rectangle(panel);
        require(EqualRect(&desktop, &after), "Opening quick status moved the desktop.");
        SendMessageW(quick_popup, WM_ACTIVATE, WA_INACTIVE, 0);
        require(!IsWindowVisible(quick_popup) && IsWindowVisible(panel), "Quick focus dismissal hid the desktop.");
    });
    check("quick_panel_keyboard_and_native_controls", [&] {
        show_quick_panel();
        wchar_t type[32]{}; GetClassNameW(quick_open, type, 32);
        require(std::wstring(type) == L"Button" || std::wstring(type) == L"BUTTON", "Quick actions are not native accessible buttons.");
        SetFocus(quick_open);
        MSG tab{}; tab.hwnd = quick_open; tab.message = WM_KEYDOWN; tab.wParam = VK_TAB;
        require(route_quick_keyboard(tab) && GetFocus() == quick_update, "Tab cannot reach the update action.");
        MSG escape{}; escape.hwnd = quick_update; escape.message = WM_KEYDOWN; escape.wParam = VK_ESCAPE;
        require(route_quick_keyboard(escape) && !IsWindowVisible(quick_popup) && IsWindowVisible(panel), "Escape on a quick button hid the wrong surface.");
        show_quick_panel(); SetFocus(quick_open);
        MSG enter{}; enter.hwnd = quick_open; enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
        require(route_quick_keyboard(enter) && !IsWindowVisible(quick_popup) && IsWindowVisible(panel), "Enter did not open the desktop.");
        require(GetFocus() == panel, "Desktop activation left keyboard focus in the hidden quick popup.");
    });
    check("quick_actions_remain_visible_in_short_work_area", [&] {
        for (int height : {96, 140, 180, 248}) {
            SetWindowPos(quick_popup, nullptr, 0, 0, 360, height, SWP_NOZORDER | SWP_NOACTIVATE);
            layout_quick_panel(); const auto bounds = rectangle(quick_popup);
            for (auto control : {quick_open, quick_update}) {
                const auto child = rectangle(control);
                require(child.left >= bounds.left && child.top >= bounds.top && child.right <= bounds.right &&
                    child.bottom <= bounds.bottom && child.bottom > child.top, "A quick action escaped the short work area.");
            }
        }
        close_quick_panel();
    });
    check("quick_update_opens_persistent_desktop_without_repositioning", [&] {
        release_check = std::make_unique<WindowsReleaseCheck>([] { return Json{{"ok", true}, {"available", false}}; });
        const auto before = rectangle(panel);
        show_quick_panel(); SendMessageW(quick_popup, WM_COMMAND, MAKEWPARAM(2, BN_CLICKED), reinterpret_cast<LPARAM>(quick_update));
        const auto after = rectangle(panel);
        require(updates_page && IsWindowVisible(panel) && !IsWindowVisible(quick_popup) && EqualRect(&before, &after), "Update action did not preserve the desktop host.");
        SendMessageW(panel, WM_ACTIVATE, WA_INACTIVE, 0);
        require(IsWindowVisible(panel), "Update page disappeared when focus moved.");
        updates_page = false; release_check.reset();
    });
    check("single_instance_reopen_restores_hidden_desktop", [&] {
        close_panel(); SendMessageW(quick_popup, reopen_message, 0, 0);
        require(IsWindowVisible(panel) && !IsIconic(panel), "Existing-instance activation did not restore Monitor.");
    });
    check("second_process_reuses_existing_desktop", [&] {
        close_panel();
        const auto result = windows_command(current_executable_path(), {"--desktop-preview", fs::absolute(output).string()});
        require(result.value("existing_instance", false), "Second process did not detect the existing Monitor.");
        MSG message{};
        require(PeekMessageW(&message, quick_popup, reopen_message, reopen_message, PM_REMOVE) != FALSE,
            "Second process did not request activation of the existing desktop.");
        DispatchMessageW(&message);
        require(desktop_renderable(), "Second-process activation did not restore the existing desktop.");
    });
    check("qualification_lock_collision_fails_closed", [&] {
        const auto marker = output / "result.json";
        const bool existed = fs::exists(marker);
        const auto before = existed ? read_text(marker, kMaximumStateBytes, false) : std::string{};
        DWORD completion = STILL_ACTIVE;
        const auto result = windows_command(current_executable_path(),
            {"--window-shell-test", fs::absolute(output).string()}, std::chrono::seconds(120), {}, &completion);
        require(completion == 1, "Qualification lock collision did not fail the child process.");
        require(!result.value("ok", true) &&
            result.value("error", Json::object()).value("code", "") == "window-shell-fixture-busy",
            "Qualification collision failed for a reason other than its active fixture owner.");
        if (existed)
            require(fs::exists(marker) && read_text(marker, kMaximumStateBytes, false) == before,
                "Qualification collision replaced the existing result marker.");
        else require(!fs::exists(marker), "Qualification collision published a result without running its checks.");
    });
    check("background_start_keeps_both_surfaces_hidden", [&] {
        close_quick_panel(); close_panel(); present_monitor_start(true);
        require(!IsWindowVisible(panel) && !IsWindowVisible(quick_popup) && !desktop_renderable(), "Background startup opened a desktop surface.");
        present_monitor_start(false);
        require(desktop_renderable() && !IsWindowVisible(quick_popup), "Explicit startup did not open the desktop.");
    });
    check("malformed_placement_falls_back_to_visible_desktop", [&] {
        const auto saved = settings.at("windows_monitor_window");
        for (const auto& invalid : {Json::array(), Json{{"width", "wide"}, {"height", Json::array()}},
                Json{{"x", std::numeric_limits<std::uint64_t>::max()}, {"y", -10000000}, {"width", 600},
                    {"height", 400}, {"dpi", false}, {"maximized", "yes"}}}) {
            settings["windows_monitor_window"] = invalid; desktop_placement_ready = false;
            restore_desktop_placement();
            const auto bounds = rectangle(panel);
            MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST), &info);
            require(bounds.left >= info.rcWork.left && bounds.top >= info.rcWork.top && bounds.right <= info.rcWork.right &&
                bounds.bottom <= info.rcWork.bottom && bounds.right > bounds.left && bounds.bottom > bounds.top,
                "Malformed saved state restored the desktop outside its work area.");
        }
        settings["windows_monitor_window"] = saved; desktop_placement_ready = false; show_panel();
    });
    check("quick_panel_en_ru_and_content_size_stability", [&] {
        for (auto language : {shin::ui::Language::en, shin::ui::Language::ru}) {
            shin::ui::set_language(language); refresh_quick_panel();
            wchar_t text[128]{}; GetWindowTextW(quick_open, text, 128);
            require(std::wstring(text) == (language == shin::ui::Language::ru ? L"Открыть Monitor" : L"Open Monitor"),
                "Quick action lost the selected language.");
            const auto before = rectangle(panel);
            render_frame(synthetic_view()); checked(swap_chain->Present(0, 0), "Cannot render desktop shell fixture.");
            const auto after = rectangle(panel);
            require(EqualRect(&before, &after), "Desktop content auto-resized the user's window.");
            save_frame(output / (language == shin::ui::Language::en ? "desktop-en.png" : "desktop-ru.png"));
            show_quick_panel();
            save_quick_test_frame(output / (language == shin::ui::Language::en ? "quick-en.png" : "quick-ru.png"));
            close_quick_panel();
        }
    });
    check("update_handoff_saves_desktop_placement", [&] {
        close_quick_panel();
        show_panel();
        ShowWindow(panel, SW_RESTORE);
        capture_desktop_placement(true);
        ShowWindow(panel, SW_MAXIMIZE);
        require(settings.at("windows_monitor_window").value("maximized",false)==false,
            "Update handoff fixture did not begin with a different saved state.");
        SendMessageW(panel, update_handoff_message, 0, 0);
        require(!IsWindow(panel), "Update handoff left the original Monitor running.");
        require(preferences->load().value().at("windows_monitor_window").value("maximized",false),
            "Update handoff did not save the current maximized state.");
    });
    std::ofstream(output / "result.json") << Json{{"ok", true}, {"checks", checks}, {"source_fixture_only", true}}.dump(2);
    return 0;
}
