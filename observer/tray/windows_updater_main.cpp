#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <commctrl.h>
#include <shlobj.h>
#include "shin/observer/observer.h"
#include "shin/observer/local_updates.h"
#include "shin/windows/file.h"
#include "../ui/language.h"
#include "../ui/project_progress.h"
#include "windows_download.h"
#include "windows_installation.h"
#include <future>
#include <mutex>
#include <vector>
#include <array>
#include <algorithm>
#include <fstream>
#include <exception>
#include <limits>

using namespace shin::observer;
namespace {
HWND window=nullptr,heading=nullptr,label=nullptr,current_status=nullptr,bar=nullptr;
constexpr COLORREF background_color=RGB(38,42,39),ink_color=RGB(236,237,224),sand_color=RGB(228,204,162);
HBRUSH background=CreateSolidBrush(background_color);
COLORREF status_color=RGB(130,192,240);
std::wstring displayed_plan,displayed_status;
std::vector<const char*> steps{"Checking reviewed release…","Downloading update…","Verifying download…","Waiting for Monitor to close…","Windows Installer is installing the update. Confirm UAC to continue.","Preparing projects…"};
std::size_t active_step=0;
bool success=false;
std::mutex mutex;
std::string description;
unsigned percent=0;
bool terminal=false;
fs::path monitor_path;
std::wstring wide(const std::string& value) { return windows_update::wide_url(value); }
void present_plan(const std::wstring& next) {
    if(!label || next==displayed_plan || next.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) return;
    DWORD selection_start=0,selection_end=0;
    SendMessageW(label,EM_GETSEL,reinterpret_cast<WPARAM>(&selection_start),reinterpret_cast<LPARAM>(&selection_end));
    const auto first_visible=SendMessageW(label,EM_GETFIRSTVISIBLELINE,0,0);
    SendMessageW(label,WM_SETREDRAW,FALSE,0);
    if(SetWindowTextW(label,next.c_str())) {
        const auto bound=static_cast<DWORD>(next.size());
        SendMessageW(label,EM_SETSEL,static_cast<WPARAM>(std::min(selection_start,bound)),
            static_cast<LPARAM>(std::min(selection_end,bound)));
        const auto lines=SendMessageW(label,EM_GETLINECOUNT,0,0);
        const auto logical_lines=static_cast<std::size_t>(std::count(next.begin(),next.end(),L'\n'))+1;
        // EDIT counts soft-wrapped display lines, not just newline-separated
        // rows. Bound against both so a narrow/high-DPI journal keeps its place.
        if(lines>0 && static_cast<std::size_t>(lines)>=logical_lines &&
            static_cast<std::size_t>(lines)<=next.size()+1) {
            const auto restored=std::clamp<LRESULT>(first_visible,0,lines-1);
            const auto visible=SendMessageW(label,EM_GETFIRSTVISIBLELINE,0,0);
            if(visible>=0 && visible<lines) SendMessageW(label,EM_LINESCROLL,0,restored-visible);
        }
        displayed_plan=next;
    }
    SendMessageW(label,WM_SETREDRAW,TRUE,0);
    InvalidateRect(label,nullptr,TRUE);
}
void layout_update_window(HWND target) {
    if(!heading || !label || !current_status || !bar) return;
    RECT bounds{};GetClientRect(target,&bounds);
    const int height=static_cast<int>(bounds.bottom);
    const bool compact_layout=height<320;
    const int margin=compact_layout?12:24;
    const int width=std::max(1,static_cast<int>(bounds.right)-2*margin);
    const int heading_height=compact_layout?28:36,button_height=compact_layout?30:34;
    const int progress_height=compact_layout?8:10,status_height=compact_layout?52:56;
    const int button_y=height-margin-button_height,progress_y=button_y-8-progress_height;
    const int status_y=progress_y-8-status_height,label_y=margin+heading_height+8;
    MoveWindow(heading,margin,margin,width,heading_height,TRUE);
    MoveWindow(label,margin,label_y,width,std::max(1,status_y-label_y-8),TRUE);
    MoveWindow(current_status,margin,status_y,width,status_height,TRUE);
    MoveWindow(bar,margin,progress_y,width,progress_height,TRUE);
    const auto button=GetDlgItem(target,1);
    if(button) {
        const auto text=wide(shin::ui::tr("Open panel"));
        HDC dc=GetDC(button);SIZE measured{};
        const auto previous=SelectObject(dc,reinterpret_cast<HGDIOBJ>(SendMessageW(button,WM_GETFONT,0,0)));
        GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&measured);
        SelectObject(dc,previous);ReleaseDC(button,dc);
        MoveWindow(button,margin,button_y,std::min(width,static_cast<int>(measured.cx)+28),button_height,TRUE);
    }
}
#include "windows_command.h"
void update(const char* text,unsigned progress=0,bool done=false) {
    {std::lock_guard lock(mutex);description=shin::ui::tr(text);percent=progress;terminal=done;
     for(std::size_t i=0;i<steps.size();++i) if(text==std::string_view(steps[i])) active_step=i;
     success=done && std::string_view(text)=="Update completed. Restart persistent MCP clients.";}
    if(window) PostMessageW(window,WM_APP,0,0);
}
void show_failure(const std::exception& error) {
    update("Update was not confirmed. Check the saved result before retrying.",0,true);
    const auto* typed=dynamic_cast<const Error*>(&error);
    {std::lock_guard lock(mutex);description+="\n"+(typed?typed->code:std::string("update-failed"));}
    if(window) PostMessageW(window,WM_APP,0,0);
}
std::optional<fs::path> installed_root() {
    std::array<wchar_t,32768> buffer{};DWORD size=static_cast<DWORD>(buffer.size()*sizeof(wchar_t));
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"Software\\Shin\\DesktopPreview",L"InstallRoot",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,buffer.data(),&size)!=ERROR_SUCCESS) return {};
    return fs::canonical(fs::path(buffer.data()));
}
void install(const fs::path& folder) {
    std::unique_ptr<StateStore> state;
    Json receipt;
    fs::path installation;
    bool owns_attempt=false,restore_pending=false,monitor_closed=false;
    std::exception_ptr failure;
    std::string restore_error, preparation_error;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Com {HRESULT result;~Com(){if(SUCCEEDED(result)) CoUninitialize();}} apartment{com};
    try {
        if(FAILED(com)) throw Error("update-installer-unavailable","Windows desktop components are unavailable.");
        update("Checking reviewed release…");
        state=std::make_unique<StateStore>(folder,true);state->recover_pending();
        receipt=state->load().value();
        if(receipt.value("schema_version","")!="shin.windows-update.v1" || receipt.value("phase","")!="reviewed")
            throw Error("update-review-invalid","Update review is unavailable.");
        owns_attempt=true;
        const auto prefix=installed_root();
        if(!prefix || *prefix!=fs::canonical(fs::path(receipt.at("prefix").get<std::string>())))
            throw Error("update-installation-changed","The MSI installation changed after review.");
        installation=*prefix;
        const auto before=windows_update::verify_installed(*prefix,receipt.at("from_version").get<std::string>());
        if(before.at("version")!=receipt.at("from_version")) throw Error("update-installation-changed","The installation changed. Restart Monitor before updating.");
        {std::lock_guard lock(mutex);monitor_path=*prefix/"bin/shin-tray.exe";}
        const auto save=[&](const char* phase) {receipt["phase"]=phase;state->save(receipt);};
        update("Checking reviewed release…");save("checking");
        const auto releases=parse_release_feed(windows_update::download(L"https://api.github.com/repos/toseek-samurai/shin-context-engine/releases?per_page=30",kMaximumReleaseFeedBytes));
        const auto offer=select_windows_release(releases,receipt.at("from_version"));
        if(!offer.value("available",false) || offer.at("revision")!=receipt.at("offer").at("revision"))
            throw Error("update-offer-changed","The release changed. Check again before installing.");
        save("downloading");update("Downloading update…");
        const auto total=offer.at("bytes").get<std::uint64_t>();
        const auto payload=windows_update::download(windows_update::wide_url(offer.at("archive_url")),static_cast<std::size_t>(total),
            [&](std::uint64_t bytes) {update("Downloading update…",static_cast<unsigned>(bytes*100/total));});
        save("verifying");update("Verifying download…",100);
        const auto checksum=windows_update::download(windows_update::wide_url(offer.at("checksum_url")),512);
        windows_update::verify(offer,payload,checksum);
        shin::windows::Directory directory(folder,shin::windows::Directory::Access::private_owner);
        if(!directory.create("installer.msi",payload)) throw Error("update-stage-conflict","Update staging already contains an installer.");
        receipt["installer_sha256"]=offer.at("sha256");
        const DWORD parent=receipt.at("monitor_pid").get<DWORD>();
        std::unique_ptr<void,decltype(&CloseHandle)> monitor(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent),&CloseHandle);
        if(monitor) {
            FILETIME created{},exited{},kernel{},user{};const bool known=GetProcessTimes(monitor.get(),&created,&exited,&kernel,&user)!=FALSE;
            const auto identity=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
            if(!known) throw Error("update-monitor-running","Monitor identity could not be confirmed.");
            DWORD status=WAIT_OBJECT_0;
            if(identity==receipt.value("monitor_created",0ULL)) {
                // Publish the close request only after the original process is
                // identified and pinned. A failed probe must leave Monitor open.
                save("waiting-monitor");update("Waiting for Monitor to close…");
                status=WaitForSingleObject(monitor.get(),30000);
            }
            if(status!=WAIT_OBJECT_0) throw Error("update-monitor-running","Close Monitor before installation.");
        } else if(GetLastError()!=ERROR_INVALID_PARAMETER)
            throw Error("update-monitor-running","Monitor shutdown could not be confirmed.");
        restore_pending=true;monitor_closed=true;
        if(!directory.create_empty_file("installer.log")) throw Error("update-stage-conflict","Update staging already contains an installer log.");
        receipt["installer_log"]="installer.log";
        save("installing");update("Windows Installer is installing the update. Confirm UAC to continue.");
        DWORD code=1;
        {
            // Pin the verified file and every parent against replacement through
            // elevation and the complete Installer operation.
            shin::windows::ReadFile pinned(folder/"installer.msi");pinned.require_user_owned();
            shin::windows::ReadFile log(folder/"installer.log",shin::windows::ReadFile::Sharing::concurrent_writer);log.require_user_owned();
            if(pinned.stamp().bytes!=total || digest(pinned.read(static_cast<std::size_t>(total)))!=offer.at("sha256").get_ref<const std::string&>())
                throw Error("update-stage-changed","The verified installer changed.");
            windows_update::InstallerReadAccess installer_access(folder/"installer.msi");
            std::array<wchar_t,32768> system{};const UINT length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
            if(!length || length>=system.size()) throw Error("update-installer-unavailable","Windows Installer is unavailable.");
            const auto executable=fs::path(system.data())/"msiexec.exe";
            // The owner already confirmed in Monitor. Native passive UI shows
            // real Installer progress without also launching the wizard's setup.
            const std::wstring arguments=L"/i \""+(folder/"installer.msi").wstring()+L"\" /passive /norestart /L*V \""+(folder/"installer.log").wstring()+L"\"";
            SHELLEXECUTEINFOW start{};start.cbSize=sizeof(start);start.fMask=SEE_MASK_NOCLOSEPROCESS;start.hwnd=window;
            start.lpVerb=L"runas";start.lpFile=executable.c_str();start.lpParameters=arguments.c_str();start.nShow=SW_SHOWNORMAL;
            if(!ShellExecuteExW(&start) || !start.hProcess) throw Error(GetLastError()==ERROR_CANCELLED?"update-uac-cancelled":"update-installer-unavailable","Windows Installer did not start.");
            WaitForSingleObject(start.hProcess,INFINITE);GetExitCodeProcess(start.hProcess,&code);CloseHandle(start.hProcess);
            receipt["installer_exit_code"]=code;state->save(receipt);
            if(!installer_access.restore()) throw Error("update-installer-access-unavailable","Installer payload privacy could not be restored.");
        }
        receipt["installer_exit_code"]=code;
        const auto outcome=windows_update::msi_outcome(code);
        if(outcome==windows_update::MsiOutcome::unconfirmed)
            throw Error("update-msi-failed","Windows Installer did not confirm success. Review its result before retrying.");
        receipt["restart_required"]=outcome==windows_update::MsiOutcome::reboot_required;
        // Independently inspect the installed manifest, never infer success from
        // progress or from the absence of an Installer dialog.
        const auto manifest=windows_update::verify_installed(*prefix,offer.at("version").get<std::string>());
        const fs::path home=folder.parent_path().parent_path().parent_path();
        if(folder.parent_path()!=home/"updates/windows") throw Error("update-stage-changed","Update home changed.");
        const auto& version=manifest.at("version").get_ref<const std::string&>();
        // Persist the verified MSI outcome before preparing projects. If the
        // helper is interrupted, Monitor can retry preparation from this receipt.
        windows_update::record_installed_update(receipt,version,{{"ok",false}});
        state->save(receipt);
        update("Preparing projects…");
        try {
            auto prepared=prepare_installed_windows_projects(*prefix,home,manifest);
            windows_update::record_installed_update(receipt,version,std::move(prepared));
        } catch(const Error& error) {
            preparation_error=error.code;
        } catch(const std::exception&) {
            preparation_error="installation-preparation-unavailable";
        }
        if(!preparation_error.empty())
            windows_update::record_installed_update(receipt,version,
                {{"ok",false},{"error",{{"code",preparation_error}}}});
        try {directory.remove("installer.msi",static_cast<std::size_t>(total),payload);} catch(...) { /* Verified installation remains successful; keep the staged file for review. */ }
    } catch(const std::exception& error) {
        failure=std::current_exception();
        const auto* typed=dynamic_cast<const Error*>(&error);
        if(owns_attempt) try {receipt["phase"]="review-required";receipt["error_code"]=typed?typed->code:"update-failed";state->save(receipt);} catch(...) {}
        if(restore_pending) {std::lock_guard lock(mutex);monitor_path.clear();}
    }
    try {
        fs::path restored;
        windows_update::restore_update_monitor(restore_pending,[&] {
            {std::lock_guard lock(mutex);monitor_path.clear();}
            windows_update::verify_saved_update_installation(installation,receipt);
            restored=installation/"bin/shin-tray.exe";
        },[&] {
            {std::lock_guard lock(mutex);monitor_path=restored;}
            if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",restored.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
                throw Error("monitor-launch-failed","The installed Monitor could not be reopened.");
        });
        if(state && monitor_closed) {receipt["monitor_launch_accepted"]=!restore_pending;state->save(receipt);}
    } catch(const std::exception& error) {
        const auto* typed=dynamic_cast<const Error*>(&error);
        restore_error=typed?typed->code:std::string("monitor-launch-failed");
        if(state) try {receipt["monitor_restore_error"]=restore_error;state->save(receipt);} catch(...) {}
    }
    // Keep the progress window alive until recovery finishes. A terminal Close
    // must not destroy its owner HWND while Monitor is being relaunched.
    if(failure) {
        try {std::rethrow_exception(failure);} catch(const std::exception& error) {show_failure(error);}
    } else {
        update("Update completed. Restart persistent MCP clients.",100,true);
        if(receipt.value("projects_pending",false)) {
            std::lock_guard lock(mutex);description+="\n"+std::string(shin::ui::tr("Project preparation needs attention."));
            if(!preparation_error.empty()) description+="\n"+preparation_error;
        }
    }
    if(!restore_error.empty()) {
        {std::lock_guard lock(mutex);description+="\n"+restore_error;}
        if(window) PostMessageW(window,WM_APP,0,0);
    }
}
// Called by the successful MSI wizard's normal-user completion action. The
// Installer transaction has finished; no project data is changed by SYSTEM.
void finish_setup() {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Com {HRESULT result;~Com(){if(SUCCEEDED(result)) CoUninitialize();}} apartment{com};
    try {
        if(FAILED(com)) throw Error("update-installer-unavailable","Windows desktop components are unavailable.");
        const auto prefix=installed_root();
        if(!prefix || directory_identity(*prefix)!=directory_identity(current_executable_path().parent_path().parent_path()))
            throw Error("update-msi-installation-required","Finish setup from the installed MSI kit.");
        update("Verifying installed files…");
        const auto manifest=windows_update::verify_installed(*prefix,SHIN_OBSERVER_VERSION);
        PWSTR profile=nullptr;
        if(FAILED(SHGetKnownFolderPath(FOLDERID_Profile,0,nullptr,&profile))) throw Error("home-unavailable","User profile is unavailable.");
        const fs::path home=fs::path(profile)/".shin-ce";CoTaskMemFree(profile);
        {std::lock_guard lock(mutex);monitor_path=*prefix/"bin/shin-tray.exe";}
        update("Preparing projects…");
        const auto result=prepare_installed_windows_projects(*prefix,home,manifest);
        if(!result.value("ok",false)) throw Error("installation-project-not-ready","Some projects require review.",result);
        update("Update completed. Restart persistent MCP clients.",100,true);
        // Opening Monitor is the MSI wizard's explicit, checked completion action.
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",monitor_path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
            throw Error("monitor-launch-failed","Setup finished, but Monitor could not be opened.");
        PostMessageW(window,WM_CLOSE,0,0);
    } catch(const std::exception& error) {show_failure(error);}
}
LRESULT CALLBACK procedure(HWND target,UINT message,WPARAM key,LPARAM data) {
    switch(message) {
    case WM_APP: {
        std::lock_guard lock(mutex);
        const bool downloading=std::string_view(steps.at(active_step))=="Downloading update…";
        std::string plan;
        for(std::size_t i=0;i<steps.size();++i) plan+=(success||i<active_step?"✓  ":i==active_step?(terminal?"×  ":"●  "):"○  ")+std::string(shin::ui::tr(steps[i]))+"\r\n";
        plan+="\r\n";
        // Keep full recovery details selectable. EDIT expects CRLF line ends;
        // status/error producers may supply LF without the carriage return.
        for(const char character:description) {
            if(character=='\n' && plan.back()!='\r') plan+='\r';
            plan+=character;
        }
        present_plan(wide(plan));
        auto status=description;if(downloading&&!terminal) status+=" · "+std::to_string(percent)+"%";
        const auto next_status=wide(status);
        status_color=terminal?(success?RGB(130,212,155):RGB(245,140,131)):RGB(130,192,240);
        if(next_status!=displayed_status && SetWindowTextW(current_status,next_status.c_str())) displayed_status=next_status;
        const bool marquee=!terminal && !downloading;
        const auto style=GetWindowLongPtrW(bar,GWL_STYLE);
        SetWindowLongPtrW(bar,GWL_STYLE,marquee?style|PBS_MARQUEE:style&~static_cast<LONG_PTR>(PBS_MARQUEE));
        SendMessageW(bar,PBM_SETMARQUEE,marquee,40);
        if(downloading || terminal) SendMessageW(bar,PBM_SETPOS,percent,0);
        EnableWindow(GetDlgItem(target,1),terminal&&!monitor_path.empty());
        if(terminal && IsWindowVisible(target) && !IsIconic(target) && !SetForegroundWindow(target)) {
            FLASHWINFO flash{sizeof(flash),target,FLASHW_TRAY,3,0};FlashWindowEx(&flash);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(key),reinterpret_cast<HWND>(data)==current_status?status_color:ink_color);
        SetBkColor(reinterpret_cast<HDC>(key),background_color);
        return reinterpret_cast<LRESULT>(background);
    case WM_SIZE: layout_update_window(target);return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};HDC dc=BeginPaint(target,&paint);RECT bounds{};GetClientRect(target,&bounds);
        HPEN pen=CreatePen(PS_SOLID,1,sand_color);
        const auto previous_pen=SelectObject(dc,pen),previous_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Rectangle(dc,0,0,bounds.right,bounds.bottom);
        SelectObject(dc,previous_brush);SelectObject(dc,previous_pen);DeleteObject(pen);EndPaint(target,&paint);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(key)==1) {
            fs::path selected;{std::lock_guard lock(mutex);selected=monitor_path;}
            if(!selected.empty() && reinterpret_cast<INT_PTR>(ShellExecuteW(target,L"open",selected.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32) {
                {std::lock_guard lock(mutex);description+="\nmonitor-launch-failed";}
                PostMessageW(target,WM_APP,0,0);
            }
        }
        return 0;
    case WM_CLOSE: {std::lock_guard lock(mutex);if(terminal) DestroyWindow(target);else ShowWindow(target,SW_MINIMIZE);return 0;}
    case WM_DESTROY: PostQuitMessage(0);return 0;
    default:return DefWindowProcW(target,message,key,data);
    }
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    int count=0;auto raw=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!raw) return 2;
    std::vector<std::wstring> args(raw,raw+count);LocalFree(raw);
    const bool completion=args.size()==1;
    if(!completion && (args.size()!=3 || args[1]!=L"--review")) return 2;
    try {
        fs::path folder;
        if(completion) {
            steps={"Verifying installed files…","Preparing projects…"};
            shin::ui::set_language(PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?shin::ui::Language::ru:shin::ui::Language::en);
        } else {
            folder=fs::canonical(fs::path(args[2]));
            StateStore review(folder,false);const auto value=review.load();
            if(!value || value->value("phase","")!="reviewed") return 2;
            shin::ui::set_language(value->value("language", "en")=="ru"?shin::ui::Language::ru:shin::ui::Language::en);
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
        WNDCLASSW type{};type.hInstance=instance;type.lpfnWndProc=procedure;type.lpszClassName=L"ShinWindowsUpdater";
        type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hbrBackground=background;
        if(!RegisterClassW(&type)) return 2;
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
        const int width=monitor.rcWork.right>monitor.rcWork.left?std::min(640,static_cast<int>(monitor.rcWork.right-monitor.rcWork.left)-48):640;
        const int height=monitor.rcWork.bottom>monitor.rcWork.top?std::min(640,static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top)-48):640;
        window=CreateWindowW(type.lpszClassName,wide(shin::ui::tr("Shin updates")).c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,width,height,nullptr,nullptr,instance,nullptr);
        if(!window) return 2;
        heading=CreateWindowW(L"STATIC",wide(shin::ui::tr("Shin updates")).c_str(),WS_CHILD|WS_VISIBLE,24,22,580,36,window,nullptr,instance,nullptr);
        label=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            24,76,580,410,window,nullptr,instance,nullptr);
        current_status=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_NOPREFIX,24,446,580,56,window,nullptr,instance,nullptr);
        bar=CreateWindowW(PROGRESS_CLASSW,L"",WS_CHILD|WS_VISIBLE|PBS_MARQUEE,24,510,580,10,window,nullptr,instance,nullptr);
        const auto button=CreateWindowW(L"BUTTON",wide(shin::ui::tr("Open panel")).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            24,550,160,34,window,reinterpret_cast<HMENU>(1),instance,nullptr);EnableWindow(button,FALSE);
        RECT client{};GetClientRect(window,&client);
        HFONT font=CreateFontW(client.bottom<320?-14:-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        HFONT title_font=CreateFontW(-24,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(title_font),TRUE);
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageW(current_status,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageW(bar,PBM_SETBARCOLOR,0,RGB(130,192,240));SendMessageW(bar,PBM_SETBKCOLOR,0,RGB(68,73,60));
        layout_update_window(window);
        ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
        auto worker=std::async(std::launch::async,[folder,completion]{if(completion) finish_setup();else install(folder);});
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
        worker.get();DeleteObject(title_font);DeleteObject(font);DeleteObject(background);return 0;
    } catch(...) {return 2;}
}
