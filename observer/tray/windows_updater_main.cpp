#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <commctrl.h>
#include <shlobj.h>
#include "shin/observer/observer.h"
#include "shin/observer/local_updates.h"
#include "shin/windows/file.h"
#include "../ui/language.h"
#include "windows_download.h"
#include "windows_installation.h"
#include <future>
#include <mutex>
#include <vector>
#include <array>
#include <fstream>

using namespace shin::observer;
namespace {
HWND window=nullptr,label=nullptr,bar=nullptr;
HBRUSH background=CreateSolidBrush(RGB(38,42,39));
std::vector<const char*> steps{"Checking reviewed release…","Downloading update…","Verifying download…","Waiting for Monitor to close…","Windows Installer is installing the update. Confirm UAC to continue.","Preparing projects…"};
std::size_t active_step=0;
bool success=false;
std::mutex mutex;
std::string description;
unsigned percent=0;
bool terminal=false;
fs::path monitor_path;
std::wstring wide(const std::string& value) { return windows_update::wide_url(value); }
#include "windows_command.h"
Json prepare_projects(const fs::path& prefix,const fs::path& home,const Json& manifest) {
    shin::windows::ReadFile cli(prefix/"bin/shin-ce.exe");cli.require_machine_owned();
    const auto result=windows_command(prefix/"bin/shin-ce.exe",{"prepare-projects","--home",home.string()},std::chrono::seconds(1105));
    if(result.value("version","")!=manifest.at("version").get_ref<const std::string&>() ||
        result.value("core_version","")!=manifest.at("core_version").get_ref<const std::string&>())
        throw Error("update-installation-changed","Project preparation belongs to another installation.");
    return result;
}
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
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Com {HRESULT result;~Com(){if(SUCCEEDED(result)) CoUninitialize();}} apartment{com};
    try {
        if(FAILED(com)) throw Error("update-installer-unavailable","Windows desktop components are unavailable.");
        state=std::make_unique<StateStore>(folder,true);state->recover_pending();
        receipt=state->load().value();
        if(receipt.value("schema_version","")!="shin.windows-update.v1" || receipt.value("phase","")!="reviewed")
            throw Error("update-review-invalid","Update review is unavailable.");
        const auto prefix=installed_root();
        if(!prefix || *prefix!=fs::canonical(fs::path(receipt.at("prefix").get<std::string>())))
            throw Error("update-installation-changed","The MSI installation changed after review.");
        const auto before=parse_json(read_text(*prefix/"shin-kit.json",kMaximumStateBytes,false));
        if(before.at("version")!=receipt.at("from_version")) throw Error("update-installation-changed","The installation changed. Restart Monitor before updating.");
        monitor_path=*prefix/"bin/shin-tray.exe";
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
        receipt["installer_sha256"]=offer.at("sha256");save("waiting-monitor");update("Waiting for Monitor to close…");
        const DWORD parent=receipt.at("monitor_pid").get<DWORD>();
        HANDLE monitor=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent);
        if(monitor) {
            FILETIME created{},exited{},kernel{},user{};const bool known=GetProcessTimes(monitor,&created,&exited,&kernel,&user)!=FALSE;
            const auto identity=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
            const auto status=known && identity==receipt.value("monitor_created",0ULL)?WaitForSingleObject(monitor,30000):WAIT_OBJECT_0;
            CloseHandle(monitor);if(status!=WAIT_OBJECT_0) throw Error("update-monitor-running","Close Monitor before installation.");
        }
        save("installing");update("Windows Installer is installing the update. Confirm UAC to continue.");
        DWORD code=1;
        {
            // Pin the verified file and every parent against replacement through
            // elevation and the complete Installer operation.
            shin::windows::ReadFile pinned(folder/"installer.msi");pinned.require_user_owned();
            if(pinned.stamp().bytes!=total || digest(pinned.read(static_cast<std::size_t>(total)))!=offer.at("sha256").get_ref<const std::string&>())
                throw Error("update-stage-changed","The verified installer changed.");
            std::array<wchar_t,32768> system{};const UINT length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
            if(!length || length>=system.size()) throw Error("update-installer-unavailable","Windows Installer is unavailable.");
            const auto executable=fs::path(system.data())/"msiexec.exe";
            // The owner already confirmed in Monitor. Native passive UI shows
            // real Installer progress without also launching the wizard's setup.
            const std::wstring arguments=L"/i \""+(folder/"installer.msi").wstring()+L"\" /passive /norestart";
            SHELLEXECUTEINFOW start{};start.cbSize=sizeof(start);start.fMask=SEE_MASK_NOCLOSEPROCESS;start.hwnd=window;
            start.lpVerb=L"runas";start.lpFile=executable.c_str();start.lpParameters=arguments.c_str();start.nShow=SW_SHOWNORMAL;
            if(!ShellExecuteExW(&start) || !start.hProcess) throw Error(GetLastError()==ERROR_CANCELLED?"update-uac-cancelled":"update-installer-unavailable","Windows Installer did not start.");
            WaitForSingleObject(start.hProcess,INFINITE);GetExitCodeProcess(start.hProcess,&code);CloseHandle(start.hProcess);
        }
        receipt["installer_exit_code"]=code;
        if(code!=0 && code!=3010 && code!=1641) throw Error("update-msi-failed","Windows Installer did not confirm success. Review its result before retrying.");
        receipt["restart_required"]=code==3010 || code==1641;
        // Independently inspect the installed manifest, never infer success from
        // progress or from the absence of an Installer dialog.
        const auto manifest=windows_update::verify_installed(*prefix,offer.at("version").get<std::string>());
        const fs::path home=folder.parent_path().parent_path().parent_path();
        if(folder.parent_path()!=home/"updates/windows") throw Error("update-stage-changed","Update home changed.");
        save("preparing-projects");update("Preparing projects…");
        receipt["project_preparation"]=prepare_projects(*prefix,home,manifest);
        if(!receipt.at("project_preparation").value("ok",false))
            throw Error("installation-project-not-ready","Some projects require review. The installed kit and project policy are preserved.");
        receipt["installed_version"]=manifest.at("version");save("completed");
        try {directory.remove("installer.msi",static_cast<std::size_t>(total),payload);} catch(...) { /* Verified installation remains successful; keep the staged file for review. */ }
        update("Update completed. Restart persistent MCP clients.",100,true);
    } catch(const std::exception& error) {
        const auto* typed=dynamic_cast<const Error*>(&error);
        if(state) try {receipt["phase"]="review-required";receipt["error_code"]=typed?typed->code:"update-failed";state->save(receipt);} catch(...) {}
        show_failure(error);
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
        monitor_path=*prefix/"bin/shin-tray.exe";
        update("Preparing projects…");
        const auto result=prepare_projects(*prefix,home,manifest);
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
        std::string plan="SHIN\n\n";
        for(std::size_t i=0;i<steps.size();++i) plan+=(success||i<active_step?"[+] ":i==active_step?"[>] ":"[ ] ")+std::string(shin::ui::tr(steps[i]))+"\n\n";
        plan+=description;if(downloading&&!terminal) plan+=" · "+std::to_string(percent)+"%";
        SetWindowTextW(label,wide(plan).c_str());
        const bool marquee=!terminal && !downloading;
        const auto style=GetWindowLongPtrW(bar,GWL_STYLE);
        SetWindowLongPtrW(bar,GWL_STYLE,marquee?style|PBS_MARQUEE:style&~static_cast<LONG_PTR>(PBS_MARQUEE));
        SendMessageW(bar,PBM_SETMARQUEE,marquee,40);
        if(downloading || terminal) SendMessageW(bar,PBM_SETPOS,percent,0);
        EnableWindow(GetDlgItem(target,1),terminal);
        if(terminal && IsWindowVisible(target) && !IsIconic(target) && !SetForegroundWindow(target)) {
            FLASHWINFO flash{sizeof(flash),target,FLASHW_TRAY,3,0};FlashWindowEx(&flash);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(key),RGB(236,237,224));SetBkColor(reinterpret_cast<HDC>(key),RGB(38,42,39));
        return reinterpret_cast<LRESULT>(background);
    case WM_COMMAND:
        if(LOWORD(key)==1 && !monitor_path.empty()) ShellExecuteW(target,L"open",monitor_path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
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
        window=CreateWindowW(type.lpszClassName,L"Shin updates",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,640,580,nullptr,nullptr,instance,nullptr);
        if(!window) return 2;
        label=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,24,28,580,410,window,nullptr,instance,nullptr);
        bar=CreateWindowW(PROGRESS_CLASSW,L"",WS_CHILD|WS_VISIBLE|PBS_MARQUEE,24,450,580,20,window,nullptr,instance,nullptr);
        const auto button=CreateWindowW(L"BUTTON",wide(shin::ui::tr("Open panel")).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            24,490,260,32,window,reinterpret_cast<HMENU>(1),instance,nullptr);EnableWindow(button,FALSE);
        HFONT font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
        auto worker=std::async(std::launch::async,[folder,completion]{if(completion) finish_setup();else install(folder);});
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
        worker.get();DeleteObject(font);DeleteObject(background);return 0;
    } catch(...) {return 2;}
}
