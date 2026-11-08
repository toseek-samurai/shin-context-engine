#pragma once
// Windows-only host fragment. The desktop and notification-area flyout have
// independent HWNDs; only the desktop hosts the shared ImGui presentation.
constexpr wchar_t quick_window_class[] = L"ShinMonitorQuickStatus";
HWND quick_popup = nullptr;
HWND quick_heading = nullptr, quick_status = nullptr, quick_version = nullptr;
HWND quick_open = nullptr, quick_update = nullptr;
HFONT quick_font = nullptr, quick_heading_font = nullptr;
int quick_font_dpi = 0;
HBRUSH quick_brush = nullptr;
HICON desktop_icon = nullptr;
bool desktop_placement_ready = false, restoring_desktop_placement = false;
RECT desktop_normal_bounds{};
UINT desktop_normal_dpi = 96;
bool desktop_maximized = false;

bool activate_existing_monitor(const std::wstring& identity, bool background_start) {
    if (const auto existing = FindWindowW(quick_window_class, identity.c_str())) {
        if (!background_start) {
            DWORD process = 0; GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process); PostMessageW(existing, reopen_message, 0, 0);
        }
        return true;
    }
    return false;
}
bool desktop_renderable() { return panel && IsWindowVisible(panel) && !IsIconic(panel); }
void update_monitor_visibility() {
    if (monitor) monitor->visible = desktop_renderable() ||
        (quick_popup && IsWindowVisible(quick_popup));
}
void close_quick_panel() {
    if (quick_popup) ShowWindow(quick_popup, SW_HIDE);
    update_monitor_visibility();
}
bool route_quick_keyboard(MSG& message) {
    if (!quick_popup || !IsWindowVisible(quick_popup) ||
        (message.hwnd != quick_popup && !IsChild(quick_popup, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { close_quick_panel(); return true; }
    return IsDialogMessageW(quick_popup, &message) != FALSE;
}
void capture_desktop_placement(bool persist) {
    if (!desktop_placement_ready || restoring_desktop_placement || !panel) return;
    if (!IsIconic(panel)) {
        desktop_maximized = IsZoomed(panel) != FALSE;
        if (!desktop_maximized) {
            RECT bounds{};
            if (GetWindowRect(panel, &bounds) && bounds.right > bounds.left && bounds.bottom > bounds.top) {
                desktop_normal_bounds = bounds;
                desktop_normal_dpi = GetDpiForWindow(panel);
            }
        }
    }
    if (!persist) return;
    settings["windows_monitor_window"] = {{"x", desktop_normal_bounds.left}, {"y", desktop_normal_bounds.top},
        {"width", desktop_normal_bounds.right - desktop_normal_bounds.left},
        {"height", desktop_normal_bounds.bottom - desktop_normal_bounds.top},
        {"dpi", desktop_normal_dpi}, {"maximized", desktop_maximized}};
    save_settings();
}
void restore_desktop_placement() {
    RECT saved{};
    UINT saved_dpi = 96;
    desktop_maximized = false;
    const auto placement = settings.value("windows_monitor_window", Json::object());
    const auto integer = [&](const char* key, int fallback) {
        if (!placement.is_object() || !placement.contains(key) || !placement.at(key).is_number_integer()) return fallback;
        if (placement.at(key).is_number_unsigned()) {
            const auto value = placement.at(key).get<std::uint64_t>();
            return value <= 1000000 ? static_cast<int>(value) : fallback;
        }
        const auto value = placement.at(key).get<std::int64_t>();
        return value >= -1000000 && value <= 1000000 ? static_cast<int>(value) : fallback;
    };
    const int width = integer("width", 0), height = integer("height", 0);
    if (width > 0 && height > 0) {
        saved.left = integer("x", 0); saved.top = integer("y", 0);
        saved.right = saved.left + width; saved.bottom = saved.top + height;
        const int dpi = integer("dpi", 96);
        if (dpi >= 48 && dpi <= 768) saved_dpi = static_cast<UINT>(dpi);
        if (placement.contains("maximized") && placement.at("maximized").is_boolean())
            desktop_maximized = placement.at("maximized").get<bool>();
    }
    const auto display = MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(display, &info)) throw std::runtime_error("Cannot locate the Monitor desktop.");
    const UINT dpi = static_cast<UINT>(std::lround(ImGui_ImplWin32_GetDpiScaleForMonitor(display) * 96));
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const RECT bounds = shin::tray::windows_shell::desktop_bounds(saved, info.rcWork, saved_dpi, dpi,
        {scale(480), scale(360)}, {scale(720), scale(760)});
    restoring_desktop_placement = true;
    SetWindowPos(panel, HWND_NOTOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    restoring_desktop_placement = false;
    desktop_normal_bounds = bounds; desktop_normal_dpi = dpi; desktop_placement_ready = true;
}
void close_panel() {
    capture_desktop_placement(true);
    ShowWindow(panel, SW_HIDE); monitor_motion.initialized = false;
    update_monitor_visibility();
}
void show_panel() {
    close_quick_panel();
    if (!desktop_placement_ready) restore_desktop_placement();
    ShowWindow(panel, IsIconic(panel) ? SW_RESTORE : desktop_maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    SetActiveWindow(panel); SetFocus(panel);
    SetForegroundWindow(panel);
    update_monitor_visibility();
}
void present_monitor_start(bool background_start) { if (!background_start) show_panel(); }
void refresh_quick_panel() {
    if (!quick_popup) return;
    const auto set = [](HWND control, const std::wstring& text) {
        const int length = GetWindowTextLengthW(control);
        std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(control, current.data(), length + 1); current.resize(static_cast<std::size_t>(length));
        if (current != text) SetWindowTextW(control, text.c_str());
    };
    set(quick_heading, L"Shin Monitor");
    const auto state = preview_mode ? synthetic_view() : monitor ? monitor->snapshot() : ViewState{};
    std::string status = shin::ui::tr(state.connected ? "Shin: service connected" : "Shin: no fresh service response");
    const auto data = state.data.is_object() ? state.data : Json::object();
    const auto projects = data.value("telemetry", Json::object()).value("project_states", Json::array());
    if (state.connected) status += "\r\n" + std::string(shin::ui::tr("Projects")) + ": " + std::to_string(projects.size());
    set(quick_status, wide(status));
    set(quick_version, wide(std::string(shin::ui::tr("Current version")) + " · v" + SHIN_OBSERVER_VERSION));
    set(quick_open, wide(shin::ui::tr("Open Monitor")));
    set(quick_update, wide(shin::ui::tr("Check for updates…")));
}
void layout_quick_panel() {
    if (!quick_popup || !quick_open) return;
    const int dpi = static_cast<int>(GetDpiForWindow(quick_popup));
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    RECT client{}; GetClientRect(quick_popup, &client);
    const int height = static_cast<int>(client.bottom);
    const int margin = std::min(px(16), height / 16), gap = std::min(px(8), height / 20);
    const int width = std::max(1, static_cast<int>(client.right) - 2 * margin);
    if (quick_font_dpi != dpi) {
        const auto previous_font = quick_font, previous_heading = quick_heading_font;
        quick_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        quick_heading_font = CreateFontW(-px(21), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(quick_heading, WM_SETFONT, reinterpret_cast<WPARAM>(quick_heading_font), TRUE);
        for (auto child : {quick_status, quick_version, quick_open, quick_update})
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(quick_font), TRUE);
        if (previous_font) DeleteObject(previous_font);
        if (previous_heading) DeleteObject(previous_heading);
        quick_font_dpi = dpi;
    }
    const int footer = height >= px(220) ? px(24) : 0;
    const int button = std::min(px(36), std::max(1, (height - 2 * margin - gap) / 2));
    const int update_y = height - margin - footer - (footer ? gap : 0) - button;
    const int open_y = update_y - gap - button;
    const int heading = open_y >= px(104) ? px(32) : 0;
    const int status_y = margin + (heading ? heading + gap : 0);
    const int status = std::min(px(46), std::max(0, open_y - gap - status_y));
    ShowWindow(quick_heading, heading ? SW_SHOWNA : SW_HIDE);
    ShowWindow(quick_status, status >= px(20) ? SW_SHOWNA : SW_HIDE);
    ShowWindow(quick_version, footer ? SW_SHOWNA : SW_HIDE);
    MoveWindow(quick_heading, margin, margin, width, heading, TRUE);
    MoveWindow(quick_status, margin, status_y, width, status, TRUE);
    MoveWindow(quick_open, margin, open_y, width, button, TRUE);
    MoveWindow(quick_update, margin, update_y, width, button, TRUE);
    MoveWindow(quick_version, margin, height - margin - footer, width, footer, TRUE);
}
void show_quick_panel() {
    NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
    identifier.hWnd = panel; identifier.uID = tray.uID;
    RECT anchor{};
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &anchor))) {
        POINT point{}; GetCursorPos(&point);
        anchor = {point.x, point.y, point.x + 1, point.y + 1};
    }
    const auto display = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(display, &info)) return;
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(display);
    const RECT bounds = shin::tray::windows_shell::quick_bounds(anchor, info.rcWork,
        {static_cast<LONG>(360 * scale), static_cast<LONG>(248 * scale)}, static_cast<int>(8 * scale));
    refresh_quick_panel();
    SetWindowPos(quick_popup, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_SHOWWINDOW);
    layout_quick_panel(); SetForegroundWindow(quick_popup); SetFocus(quick_open);
    update_monitor_visibility();
}
LRESULT CALLBACK quick_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    try {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == 1) { close_quick_panel(); management_page = updates_page = false; show_panel(); return 0; }
        if (LOWORD(wparam) == 2) { close_quick_panel(); open_updates(); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && !modal) close_quick_panel();
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { close_quick_panel(); return 0; }
        break;
    case WM_CLOSE: close_quick_panel(); return 0;
    case reopen_message: show_panel(); return 0;
    case WM_DPICHANGED: {
        const auto* bounds = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout_quick_panel(); return 0;
    }
    case WM_CTLCOLORSTATIC: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(236, 237, 224)); SetBkColor(dc, RGB(38, 42, 39)); return reinterpret_cast<LRESULT>(quick_brush);
    }
    case WM_ERASEBKGND: {
        RECT bounds{}; GetClientRect(window, &bounds); FillRect(reinterpret_cast<HDC>(wparam), &bounds, quick_brush); return 1;
    }
    default: break;
    }
    } catch (const std::exception& error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"Shin Monitor", MB_OK | MB_ICONWARNING);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void initialize_quick_panel(HINSTANCE instance, const std::wstring& identity) {
    quick_brush = CreateSolidBrush(RGB(38, 42, 39));
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = quick_window_proc; wc.hInstance = instance;
    wc.lpszClassName = quick_window_class; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = quick_brush;
    wc.style = CS_DROPSHADOW;
    if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register the quick status panel.");
    quick_popup = CreateWindowExW(shin::tray::windows_shell::quick_extended_style | WS_EX_CONTROLPARENT,
        quick_window_class, identity.c_str(), shin::tray::windows_shell::quick_style,
        0, 0, 360, 248, nullptr, nullptr, instance, nullptr);
    if (!quick_popup) throw std::runtime_error("Cannot create the quick status panel.");
    const auto control = [&](const wchar_t* type, DWORD style, int id) {
        const auto child = CreateWindowExW(0, type, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
            quick_popup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!child) throw std::runtime_error("Cannot create quick status controls.");
        return child;
    };
    quick_heading = control(L"STATIC", SS_LEFT, 10); quick_status = control(L"STATIC", SS_LEFT, 11);
    quick_version = control(L"STATIC", SS_LEFT, 12);
    quick_open = control(L"BUTTON", WS_TABSTOP | BS_DEFPUSHBUTTON, 1);
    quick_update = control(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, 2);
    layout_quick_panel(); refresh_quick_panel();
}
