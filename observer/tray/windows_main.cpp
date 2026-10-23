#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "monitor_view.h"
#include <fstream>
#include <iostream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT reopen_message = WM_APP + 2;
constexpr UINT language_message = WM_APP + 3;
constexpr wchar_t window_class[] = L"ShinMonitorWindow";
HWND panel = nullptr, report = nullptr, report_edit = nullptr;
std::unique_ptr<Monitor> monitor;
std::unique_ptr<StateStore> preferences;
Json settings = Json::object();
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> swap_chain;
ComPtr<ID3D11RenderTargetView> target;
ComPtr<ID3D11ShaderResourceView> brand_texture;
ImFont* symbols = nullptr;
NOTIFYICONDATAW tray{};
UINT taskbar_created = 0;
bool compact = false, modal = false, test_mode = false;

void checked(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error(shin::ui::tr("Invalid UTF-8 in Monitor input."));
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    checked(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value), shin::ui::tr("Cannot locate the user directory."));
    const fs::path path(value); CoTaskMemFree(value); return path;
}
void save_settings() {
    if (preferences) preferences->save(settings);
}
std::string platform_system_language() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? "ru" : "en";
}
void platform_save_language(int preference) {
    settings["language"] = preference; save_settings();
    PostMessageW(panel, language_message, 0, 0);
}
void platform_save_view(int view) { settings["view"] = view; save_settings(); }
std::string platform_font() { return (known_folder(FOLDERID_Fonts) / "segoeui.ttf").string(); }
std::string platform_version() { return SHIN_OBSERVER_VERSION; }
bool platform_reduce_motion() {
    BOOL enabled = FALSE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || !enabled;
}
void draw_brand(ImVec2 origin, float size) {
    if (brand_texture) ImGui::GetWindowDrawList()->AddImage(
        ImTextureRef(reinterpret_cast<ImTextureID>(brand_texture.Get())), origin, {origin.x + size, origin.y + size},
        {0, 0}, {1, 1}, ImGui::GetColorU32(sand));
}
bool platform_copy(const std::string& value) {
    const auto text = wide(value);
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    void* buffer = GlobalLock(memory);
    if (!buffer) { GlobalFree(memory); return false; }
    memcpy(buffer, text.c_str(), bytes); GlobalUnlock(memory);
    if (!OpenClipboard(panel)) { GlobalFree(memory); return false; }
    const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!ok) GlobalFree(memory);
    return ok;
}
void platform_reveal(const std::string& value) {
    const auto path = wide(value);
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (item) { (void)SHOpenFolderAndSelectItems(item, 0, nullptr, 0); ILFree(item); }
}
UINT menu_choice(const std::vector<std::pair<UINT, std::wstring>>& items, UINT selected_id = 0) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    for (const auto& [id, title] : items) AppendMenuW(menu, MF_STRING | (id == selected_id ? MF_CHECKED : 0), id, title.c_str());
    POINT point{}; GetCursorPos(&point);
    modal = true;
    SetForegroundWindow(panel);
    const UINT result = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        point.x, point.y, 0, panel, nullptr));
    modal = false; DestroyMenu(menu); PostMessageW(panel, WM_NULL, 0, 0);
    return result;
}
void show_report() {
    if (!report) {
        report = CreateWindowExW(WS_EX_TOOLWINDOW, L"ShinMonitorReport", wide(shin::ui::tr("Shin · Diagnostics")).c_str(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 640, panel, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!report) throw std::runtime_error(shin::ui::tr("Cannot open diagnostic report."));
    }
    SetWindowTextW(report_edit, wide(diagnostic_report_text + "\r\n\r\n" + diagnostic_copy).c_str());
    ShowWindow(report, SW_SHOW); SetForegroundWindow(report);
}
void platform_action(int code) {
    if (code == 39) { show_report(); return; }
    if (code == 43) {
        ComPtr<IFileOpenDialog> dialog;
        checked(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)), shin::ui::tr("Cannot open model history picker."));
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        dialog->SetTitle(wide(shin::ui::tr("Shin numeric model history")).c_str());
        modal = true; const auto result = dialog->Show(panel); modal = false;
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        checked(result, shin::ui::tr("Model history selection failed."));
        ComPtr<IShellItem> item; checked(dialog->GetResult(&item), shin::ui::tr("Cannot read selected file."));
        PWSTR raw = nullptr; checked(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), shin::ui::tr("Cannot read file path."));
        const fs::path path(raw); CoTaskMemFree(raw);
        (void)read_model_usage(path, now_ms());
        monitor->set_usage_file(path); model_project.clear(); model_page = 0;
        settings["usage_file"] = path.string(); settings["model_project"] = ""; save_settings();
        return;
    }
    if (code == 40 && monitor) {
        const auto view = monitor->snapshot();
        const auto data = view.data.is_object() ? view.data : Json::object();
        const auto projects = data.value("telemetry", Json::object()).value("project_states", Json::array());
        const auto choices = shin::ui::model_project_choices(*view.usage, projects);
        std::vector<std::pair<UINT, std::wstring>> items;
        UINT chosen = 0, id = 100;
        for (const auto& row : choices) {
            items.emplace_back(id, wide(row.at("name").get<std::string>()));
            if (row.at("key") == model_project) chosen = id;
            ++id;
        }
        const auto choice = menu_choice(items, chosen);
        if (choice >= 100 && choice - 100 < choices.size()) {
            model_project = choices.at(choice - 100).at("key"); model_page = 0;
            settings["model_project"] = model_project; save_settings();
        }
        return;
    }
    UINT choice = 0;
    if (code == 44) choice = menu_choice({{20,wide(shin::ui::tr("Now")).c_str()},{23,wide(shin::ui::tr("Last hour")).c_str()},{21,wide(shin::ui::tr("24 hours")).c_str()},{22,wide(shin::ui::tr("7 days")).c_str()}},
        history_hours == 0 ? 20 : history_hours == 1 ? 23 : history_hours == 24 ? 21 : 22);
    else if (code == 62) choice = menu_choice({{63,wide(shin::ui::tr("All queries")).c_str()},{64,wide(shin::ui::tr("Unclassified")).c_str()},{65,wide(shin::ui::tr("Diagnostic")).c_str()}}, static_cast<UINT>(63 + timing_filter));
    else if (code == 66) choice = menu_choice({{67,wide(shin::ui::tr("Recent requests in this run")).c_str()},{68,wide(shin::ui::tr("All retained history")).c_str()}}, timing_retained ? 68 : 67);
    if (choice) activate(static_cast<int>(choice));
}
void draw_icon(Icon icon, ImVec2 origin, float size, ImVec4 color) {
    // Windows' own MDL2 icon font, not a second hand-drawn icon set.
    constexpr std::array<unsigned, static_cast<std::size_t>(Icon::count)> glyphs{
        0xe73e,0xe72c,0xe823,0xe90f,0xe7ba,0xe708,0xe769,0xeb55,0xe897,0xe81c,0xe8ab,0xe964,0xe8c8,0xe8b7,0xe76b,0xe9d9,0xe712};
    const unsigned cp = glyphs.at(static_cast<std::size_t>(icon));
    const char text[]{static_cast<char>(0xe0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 63)), static_cast<char>(0x80 | (cp & 63)), 0};
    ImGui::GetWindowDrawList()->AddText(symbols, size, origin, ImGui::GetColorU32(color), text);
}
void load_brand() {
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Cannot load the Shin mark.");
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(102), RT_RCDATA);
    if (!resource) throw std::runtime_error("Shin mark resource is missing.");
    const auto loaded = LoadResource(nullptr, resource);
    ComPtr<IWICStream> stream; checked(factory->CreateStream(&stream), "Cannot load mark stream.");
    checked(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(loaded)), SizeofResource(nullptr, resource)), "Cannot decode mark stream.");
    ComPtr<IWICBitmapDecoder> decoder; checked(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "Cannot decode mark.");
    ComPtr<IWICBitmapFrameDecode> frame; checked(decoder->GetFrame(0, &frame), "Cannot decode mark frame.");
    ComPtr<IWICBitmapScaler> scaler; checked(factory->CreateBitmapScaler(&scaler), "Cannot size mark.");
    checked(scaler->Initialize(frame.Get(), 64, 64, WICBitmapInterpolationModeFant), "Cannot resize mark.");
    ComPtr<IWICFormatConverter> converted; checked(factory->CreateFormatConverter(&converted), "Cannot convert mark.");
    checked(converted->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Cannot convert mark pixels.");
    std::array<BYTE, 64 * 64 * 4> pixels{};
    checked(converted->CopyPixels(nullptr, 64 * 4, static_cast<UINT>(pixels.size()), pixels.data()), "Cannot read mark pixels.");
    for (std::size_t i = 0; i < pixels.size(); i += 4) pixels[i] = pixels[i + 1] = pixels[i + 2] = 255;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 64; description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    checked(device->CreateTexture2D(&description, &initial, &texture), "Cannot create mark texture.");
    checked(device->CreateShaderResourceView(texture.Get(), nullptr, &brand_texture), "Cannot display Shin mark.");
}
HICON logo() {
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), shin::ui::tr("Cannot load the Shin icon."));
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource) throw std::runtime_error(shin::ui::tr("Shin icon resource is missing."));
    const auto loaded = LoadResource(nullptr, resource);
    ComPtr<IWICStream> stream; checked(factory->CreateStream(&stream), "Cannot load icon stream.");
    checked(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(loaded)), SizeofResource(nullptr, resource)), "Cannot decode icon stream.");
    ComPtr<IWICBitmapDecoder> decoder; checked(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "Cannot decode icon.");
    ComPtr<IWICBitmapFrameDecode> frame; checked(decoder->GetFrame(0, &frame), "Cannot decode icon frame.");
    ComPtr<IWICBitmapScaler> scaler; checked(factory->CreateBitmapScaler(&scaler), "Cannot size icon.");
    checked(scaler->Initialize(frame.Get(), 32, 32, WICBitmapInterpolationModeFant), "Cannot resize icon.");
    ComPtr<IWICFormatConverter> converted; checked(factory->CreateFormatConverter(&converted), "Cannot convert icon.");
    checked(converted->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Cannot convert icon pixels.");
    std::array<BYTE,32*32*4> pixels{};
    checked(converted->CopyPixels(nullptr, 128, static_cast<UINT>(pixels.size()), pixels.data()), "Cannot read icon pixels.");
    DWORD light = 0, size = sizeof(light);
    (void)RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    for (std::size_t i=0;i<pixels.size();i+=4) pixels[i]=pixels[i+1]=pixels[i+2]=light ? 0 : pixels[i+3];
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=32;
    info.bmiHeader.biHeight=-32; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr; HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if (!bitmap) throw std::runtime_error("Cannot allocate tray icon.");
    memcpy(bits,pixels.data(),pixels.size());
    HBITMAP mask=CreateBitmap(32,32,1,1,nullptr);
    ICONINFO icon{TRUE,0,0,mask,bitmap}; const auto result=CreateIconIndirect(&icon);
    DeleteObject(mask); DeleteObject(bitmap);
    if (!result) throw std::runtime_error("Cannot create tray icon.");
    return result;
}
void register_icon() {
    if (test_mode) return;
    tray.cbSize=sizeof(tray); tray.hWnd=panel; tray.uID=1;
    tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP; tray.uCallbackMessage=tray_message;
    if (!tray.hIcon) tray.hIcon=logo();
    wcscpy_s(tray.szTip,L"Shin Monitor");
    if (!Shell_NotifyIconW(NIM_ADD,&tray)) return; // Explorer may be restarting; the timer retries.
    tray.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&tray);
}
void close_panel() { ShowWindow(panel,SW_HIDE); monitor_motion.initialized=false; if(monitor) monitor->visible=false; }
void show_panel() {
    POINT point{}; GetCursorPos(&point); MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&info);
    const int width=std::min(compact?360:448,static_cast<int>(info.rcWork.right-info.rcWork.left)-24);
    const int height=std::min(compact?440:620,static_cast<int>(info.rcWork.bottom-info.rcWork.top)-24);
    const int x=std::clamp(static_cast<int>(point.x)-width/2,static_cast<int>(info.rcWork.left)+12,static_cast<int>(info.rcWork.right)-width-12);
    const int y=std::clamp(static_cast<int>(point.y)-height,static_cast<int>(info.rcWork.top)+12,static_cast<int>(info.rcWork.bottom)-height-12);
    SetWindowPos(panel,HWND_TOPMOST,x,y,width,height,SWP_SHOWWINDOW); SetForegroundWindow(panel);
    if(monitor) monitor->visible=true;
}
void render_frame(const ViewState& state) {
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); render(state); ImGui::Render();
    constexpr float clear[]{background.x,background.y,background.z,1};
    context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr); context->ClearRenderTargetView(target.Get(),clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
void make_target() {
    ComPtr<ID3D11Texture2D> back;
    checked(swap_chain->GetBuffer(0,IID_PPV_ARGS(&back)),"Cannot get Monitor render buffer.");
    checked(device->CreateRenderTargetView(back.Get(),nullptr,&target),"Cannot create Monitor render target.");
}
LRESULT CALLBACK report_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_CREATE) {
        report_edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0,0,1,1,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(report_edit,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE); return 0;
    }
    if(message==WM_SIZE) { MoveWindow(report_edit,0,0,LOWORD(lparam),HIWORD(lparam),TRUE);return 0; }
    if(message==WM_CLOSE) { ShowWindow(window,SW_HIDE);return 0; }
    return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message == language_message) {
        if (report) SetWindowTextW(report, wide(shin::ui::tr("Shin · Diagnostics")).c_str());
        return 0;
    }
    try {
        if(ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window,message,wparam,lparam)) return 1;
        if(taskbar_created && message==taskbar_created) { register_icon();return 0; }
        switch(message) {
        case reopen_message: show_panel(); return 0;
        case WM_SIZE:
            if(swap_chain && wparam!=SIZE_MINIMIZED) {
                context->OMSetRenderTargets(0,nullptr,nullptr); target.Reset();
                checked(swap_chain->ResizeBuffers(0,LOWORD(lparam),HIWORD(lparam),DXGI_FORMAT_UNKNOWN,0),"Cannot resize Monitor."); make_target();
            } return 0;
        case WM_ACTIVATE:
            if(LOWORD(wparam)==WA_INACTIVE && !modal) close_panel(); return 0;
        case WM_KEYDOWN: if(wparam==VK_ESCAPE) {close_panel();return 0;} break;
        case WM_CLOSE: close_panel(); return 0;
        case WM_DESTROY: PostQuitMessage(0);return 0;
        case WM_SETTINGCHANGE:
            if(!test_mode) {
                const auto next=logo(), previous=tray.hIcon;
                tray.hIcon=next;Shell_NotifyIconW(NIM_MODIFY,&tray);
                if(previous) DestroyIcon(previous);
            } return 0;
        case WM_TIMER:
            if(wparam==1 && monitor && !test_mode) {
                const auto state=monitor->snapshot();
                wcscpy_s(tray.szTip,state.connected?wide(shin::ui::tr("Shin: service connected")).c_str():wide(shin::ui::tr("Shin: no fresh service response")).c_str());
                if(!Shell_NotifyIconW(NIM_MODIFY,&tray)) register_icon();
            } return 0;
        case tray_message:
            if(LOWORD(lparam)==NIN_SELECT || LOWORD(lparam)==NIN_KEYSELECT) {
                if(IsWindowVisible(window)) close_panel();else show_panel();
            } else if(LOWORD(lparam)==WM_CONTEXTMENU) {
                const auto choice=menu_choice({{1,wide(shin::ui::tr("Open panel")).c_str()},{2,wide(shin::ui::tr("Compact view")).c_str()},{3,wide(shin::ui::tr("Quit Monitor")).c_str()}},compact?2:0);
                if(choice==1) show_panel();
                else if(choice==2) {compact=!compact;settings["compact"]=compact;save_settings();if(IsWindowVisible(window))show_panel();}
                else if(choice==3) DestroyWindow(window);
            } return 0;
        default: break;
        }
    } catch(const std::exception& error) {
        MessageBoxW(window,wide(error.what()).c_str(),L"Shin Monitor",MB_OK|MB_ICONWARNING);
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void save_frame(const fs::path& path) {
    ComPtr<ID3D11Texture2D> source; checked(swap_chain->GetBuffer(0,IID_PPV_ARGS(&source)),"Cannot read frame.");
    D3D11_TEXTURE2D_DESC description{};source->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;description.MiscFlags=0;
    ComPtr<ID3D11Texture2D> copy;checked(device->CreateTexture2D(&description,nullptr,&copy),"Cannot stage frame.");
    context->CopyResource(copy.Get(),source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped),"Cannot map frame.");
    struct Unmap { ID3D11DeviceContext* context; ID3D11Resource* resource; ~Unmap(){context->Unmap(resource,0);} } unmap{context.Get(),copy.Get()};
    std::size_t bright=0;
    for(UINT y=0;y<description.Height;++y) for(UINT x=0;x<description.Width;++x) {
        const auto* p=static_cast<BYTE*>(mapped.pData)+y*mapped.RowPitch+x*4;
        if(p[0]>110||p[1]>110||p[2]>110) ++bright;
    }
    if(bright<400) throw std::runtime_error("Blank Monitor render.");
    ComPtr<IWICImagingFactory> factory;checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Cannot encode frame.");
    ComPtr<IWICStream> stream;checked(factory->CreateStream(&stream),"Cannot create PNG stream.");
    checked(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Cannot open PNG output.");
    ComPtr<IWICBitmapEncoder> encoder;checked(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Cannot create PNG encoder.");
    checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Cannot initialize PNG.");
    ComPtr<IWICBitmapFrameEncode> frame;checked(encoder->CreateNewFrame(&frame,nullptr),"Cannot create PNG frame.");
    checked(frame->Initialize(nullptr),"Cannot initialize PNG frame.");checked(frame->SetSize(description.Width,description.Height),"Cannot size PNG frame.");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;checked(frame->SetPixelFormat(&format),"Cannot set PNG pixels.");
    ComPtr<IWICBitmap> bitmap;
    checked(factory->CreateBitmapFromMemory(description.Width,description.Height,GUID_WICPixelFormat32bppRGBA,
        mapped.RowPitch,mapped.RowPitch*description.Height,static_cast<BYTE*>(mapped.pData),&bitmap),"Cannot map PNG source pixels.");
    ComPtr<IWICBitmapSource> encoded;
    checked(WICConvertBitmapSource(format,bitmap.Get(),&encoded),"Cannot convert negotiated PNG format.");
    checked(frame->WriteSource(encoded.Get(),nullptr),"Cannot write PNG pixels.");
    checked(frame->Commit(),"Cannot commit PNG frame.");checked(encoder->Commit(),"Cannot commit PNG.");
}
int render_test(const fs::path& output) {
    fs::create_directories(output);
    ViewState state; state.connected=true;
    const Json project{{"id","shin"},{"name","Shin Context Engine"},{"root","C:\\Projects\\Shin"},
        {"phase","current"},{"active_issue",false},{"error_code",""},{"publications",12},{"preparations",12},{"last_tick_ms",42},{"authority","local"},{"query_ready",true}};
    state.data={{"version",SHIN_OBSERVER_VERSION},{"registry_error",""},{"enrollment_errors",Json::object()},
        {"query_workers",0},{"telemetry",{{"project_states",Json::array({project})},{"history",{{"status","ready"}}}}}};
    unsigned frames=0;
    unsigned cold_frames=0;
    for(const auto size: {ImVec2{448,620},ImVec2{432,460},ImVec2{360,440},ImVec2{280,340},ImVec2{280,240}}) {
        SetWindowPos(panel,nullptr,0,0,static_cast<int>(size.x),static_cast<int>(size.y),SWP_NOZORDER|SWP_NOACTIVATE);
        // No successful reply has ever arrived: different from a disconnected
        // monitor that still holds its last good snapshot.
        for(int view=0;view<3;++view) {
            dashboard_view=view; ViewState cold; cold.error="service-unavailable";
            for(int frame=0;frame<3;++frame) render_frame(cold);
            save_frame(output/("cold-"+std::to_string(++cold_frames)+".png"));
        }
        for(int view=0;view<3;++view) for(bool online:{false,true}) {
            dashboard_view=view;state.connected=online;
            for(int frame=0;frame<3;++frame) render_frame(state);
            for(const auto& action:ui_actions)
                if(action.origin.x < -1 || action.origin.y < -1 || action.origin.x+action.size.x>size.x+1 || action.origin.y+action.size.y>size.y+1)
                    throw std::runtime_error("Monitor action exceeds viewport: " + action.label + " view=" + std::to_string(view) +
                        " viewport=" + std::to_string(size.x) + "x" + std::to_string(size.y) +
                        " box=" + std::to_string(action.origin.x) + "," + std::to_string(action.origin.y) +
                        "," + std::to_string(action.size.x) + "," + std::to_string(action.size.y));
            save_frame(output/(std::to_string(++frames)+".png"));
        }
    }
    const auto icon=logo();DestroyIcon(icon);
    {
        struct Restore { ~Restore(){test_mode=true;} } restore;
        test_mode=false;
        taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
        register_icon();
        if(!Shell_NotifyIconW(NIM_MODIFY,&tray)) throw std::runtime_error("Notification icon was not registered.");
        Shell_NotifyIconW(NIM_DELETE,&tray);
        SendMessageW(panel,taskbar_created,0,0);
        if(!Shell_NotifyIconW(NIM_MODIFY,&tray)) throw std::runtime_error("Notification icon was not restored after TaskbarCreated.");
        Shell_NotifyIconW(NIM_DELETE,&tray);
        ShowWindow(panel,SW_SHOWNOACTIVATE);SendMessageW(panel,WM_ACTIVATE,WA_INACTIVE,0);
        if(IsWindowVisible(panel)) throw std::runtime_error("Popover did not dismiss on focus loss.");
        ShowWindow(panel,SW_SHOWNOACTIVATE);SendMessageW(panel,WM_KEYDOWN,VK_ESCAPE,0);
        if(IsWindowVisible(panel)) throw std::runtime_error("Popover did not dismiss on Escape.");
    }
    std::ofstream(output/"result.json")<<Json{{"frames",frames},{"cold_frames",cold_frames},{"nonblank",true},{"action_bounds",true},{"logo",true},
        {"taskbar_recreated_event",true},{"focus_dismissal",true},{"escape",true}}.dump(2);
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int exit_code=1;
    try {
        checked(com,shin::ui::tr("Cannot initialize Windows desktop components."));
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!argv) throw std::runtime_error(shin::ui::tr("Cannot read Monitor arguments."));
        std::vector<std::wstring> args(argv,argv+argc);LocalFree(argv);
        fs::path home=known_folder(FOLDERID_Profile)/".shin-ce", output;
        std::wstring caption=L"Shin Monitor test";
        if(args.size()==3 && args[1]==L"--render-test") {test_mode=true;output=args[2];}
        else if(args.size()==3 && args[1]==L"--home") home=args[2];
        else if(args.size()!=1) throw std::runtime_error(shin::ui::tr("Use shin-tray [--home DIRECTORY]."));
        if(!test_mode) {
            if(const auto installed=installed_local_home(current_executable_path())) {
                if(args.size()!=1 && fs::absolute(home)!=*installed) throw std::runtime_error(shin::ui::tr("Monitor belongs to another installed home."));
                home=*installed;
            }
            home=fs::absolute(home).lexically_normal();
            caption=L"Shin Monitor "+wide(digest(home.string()).substr(0,24));
            const auto base=known_folder(FOLDERID_LocalAppData)/"Shin";ensure_private_directory(base);
            const auto folder=base/("monitor-"+digest(home.string()).substr(0,24));ensure_private_directory(folder);
            try { preferences=std::make_unique<StateStore>(folder,true); }
            catch(const Error& error) {
                if(error.code=="writer-busy") {
                    if(const auto existing=FindWindowW(window_class,caption.c_str())) {
                        PostMessageW(existing,reopen_message,0,0);CoUninitialize();return 0;
                    }
                }
                throw;
            }
            preferences->recover_pending();
            settings=preferences->load().value_or(Json::object());
            apply_language_preference(settings.value("language",0));
            dashboard_view=std::clamp(settings.value("view",0),0,2);compact=settings.value("compact",false);
            model_project=settings.value("model_project","");
            monitor=std::make_unique<Monitor>(home,fs::path(settings.value("usage_file","")));
        }
        // Let DWM scale this fixed logical-size popover. It avoids mixed physical
        // mouse coordinates and preserves the same tested layout at every DPI.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=window_class;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassExW(&wc)) throw std::runtime_error(shin::ui::tr("Cannot register Monitor window."));
        wc.lpfnWndProc=report_proc;wc.lpszClassName=L"ShinMonitorReport";RegisterClassExW(&wc);
        panel=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,window_class,caption.c_str(),WS_POPUP,0,0,432,460,nullptr,nullptr,instance,nullptr);
        if(!panel) throw std::runtime_error(shin::ui::tr("Cannot create Monitor window."));
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=panel;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        const auto driver=test_mode?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE;
        auto result=D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap_chain,&device,nullptr,&context);
        if(FAILED(result) && !test_mode) result=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap_chain,&device,nullptr,&context);
        checked(result,shin::ui::tr("Direct3D 11 is unavailable."));make_target();load_brand();
        configure_imgui();
        ImFontConfig fallback;fallback.MergeMode=true;
        ImGui::GetIO().Fonts->AddFontFromFileTTF((known_folder(FOLDERID_Fonts)/"seguisym.ttf").string().c_str(),13,&fallback);
        symbols=ImGui::GetIO().Fonts->AddFontFromFileTTF((known_folder(FOLDERID_Fonts)/"segmdl2.ttf").string().c_str(),17);
        if(!symbols) throw std::runtime_error(shin::ui::tr("Windows icon font is unavailable."));
        ImGui_ImplWin32_Init(panel);ImGui_ImplDX11_Init(device.Get(),context.Get());
        if(test_mode) exit_code=render_test(output);
        else {
            taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");register_icon();SetTimer(panel,1,2000,nullptr);
            MSG message{};bool running=true;
            while(running) {
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT) {running=false;break;}
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
                if(!running) break;
                if(IsWindowVisible(panel)) {render_frame(monitor->snapshot());checked(swap_chain->Present(1,0),shin::ui::tr("Monitor graphics device was lost."));}
                MsgWaitForMultipleObjectsEx(0,nullptr,IsWindowVisible(panel)?(monitor_motion.active?16:50):INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            }
            exit_code=0;
        }
        ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
    } catch(const std::exception& error) {
        if(test_mode) std::cerr << error.what() << '\n';
        else MessageBoxW(nullptr,wide(error.what()).c_str(),L"Shin Monitor",MB_OK|MB_ICONERROR);
    }
    if(tray.hIcon) {Shell_NotifyIconW(NIM_DELETE,&tray);DestroyIcon(tray.hIcon);}
    monitor.reset();preferences.reset();
    if(report) DestroyWindow(report);
    target.Reset();swap_chain.Reset();context.Reset();device.Reset();
    if(panel && IsWindow(panel)) DestroyWindow(panel);
    if(SUCCEEDED(com)) CoUninitialize();
    return exit_code;
}
