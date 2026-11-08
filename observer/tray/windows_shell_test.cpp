#include "windows_shell.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace shell=shin::tray::windows_shell;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
void expect_rect(const RECT& actual,const RECT& expected,const char* name) {
    if(actual.left!=expected.left||actual.top!=expected.top||actual.right!=expected.right||actual.bottom!=expected.bottom) {
        std::cerr<<name<<": expected ["<<expected.left<<','<<expected.top<<','<<expected.right<<','<<expected.bottom
            <<"], received ["<<actual.left<<','<<actual.top<<','<<actual.right<<','<<actual.bottom<<"]\n";
        throw std::runtime_error("unexpected native shell rectangle");
    }
}
void test_quick_bounds() {
    struct Case {const char* name;RECT anchor,work;SIZE size;int margin;RECT expected;};
    const Case cases[]{
        {"bottom taskbar",{1840,1040,1864,1064},{0,0,1920,1040},{520,620},8,{1344,412,1864,1032}},
        {"top taskbar",{1800,8,1824,32},{0,40,1920,1080},{520,620},8,{1304,48,1824,668}},
        {"prefer above when both sides fit",{600,400,624,424},{0,0,1200,900},{300,200},8,{324,192,624,392}},
        {"top-left icon",{16,8,40,32},{0,0,1200,900},{300,200},8,{8,40,308,240}},
        {"left taskbar",{8,400,32,424},{40,0,1200,900},{300,200},8,{48,224,348,424}},
        {"right taskbar",{1168,400,1192,424},{0,0,1160,900},{300,200},8,{852,224,1152,424}},
        {"negative monitor",{-80,880,-56,904},{-1920,-200,0,880},{520,620},8,{-576,252,-56,872}},
        {"offscreen anchor",{9000,9000,9024,9024},{0,0,800,600},{300,200},8,{492,392,792,592}},
        {"tiny work area",{0,50,10,60},{-10,20,30,50},{520,620},8,{-2,28,22,42}},
        {"one pixel screen",{5,7,6,8},{5,6,6,7},{520,620},8,{5,6,6,7}},
        {"negative margin",{700,600,724,624},{0,0,800,600},{300,200},-8,{424,400,724,600}},
        {"invalid desired size",{50,50,74,74},{0,0,200,200},{0,-10},8,{73,41,74,42}},
    };
    for(const auto& item:cases) {
        const auto actual=shell::quick_bounds(item.anchor,item.work,item.size,item.margin);
        expect_rect(actual,item.expected,item.name);
        require(actual.right>actual.left&&actual.bottom>actual.top,"quick popup lost its positive size");
        require(actual.left>=item.work.left&&actual.top>=item.work.top&&actual.right<=item.work.right&&actual.bottom<=item.work.bottom,
            "quick popup escaped its monitor work area");
    }
    expect_rect(shell::quick_bounds({}, {10,20,10,20},{520,620},8),{10,20,10,20},"unavailable work area");
}
void test_desktop_bounds() {
    struct Case {const char* name;RECT saved,work;UINT from,to;SIZE minimum,initial;RECT expected;};
    const Case cases[]{
        {"saved96",{100,120,820,880},{0,0,3840,2160},96,96,{480,360},{720,760},{100,120,820,880}},
        {"saved96 to144",{100,120,820,880},{0,0,3840,2160},96,144,{720,540},{1080,1140},{100,120,1180,1260}},
        {"saved96 to192",{100,120,820,880},{0,0,3840,2160},96,192,{960,720},{1440,1520},{100,120,1540,1640}},
        {"saved144 to96",{100,100,1180,1240},{0,0,1920,1080},144,96,{480,360},{720,760},{100,100,820,860}},
        {"DPI rounding",{10,20,611,471},{0,0,1920,1080},144,96,{1,1},{720,760},{10,20,411,321}},
        {"missing saved centers initial",{0,0,0,0},{0,0,1920,1080},96,96,{480,360},{720,760},{600,160,1320,920}},
        {"initial already target pixels",{0,0,0,0},{0,0,3840,2160},96,144,{720,540},{1080,1140},{1380,510,2460,1650}},
        {"inverted saved rect",{500,500,300,400},{0,0,1920,1080},96,96,{480,360},{720,760},{600,160,1320,920}},
        {"zero saved width",{100,100,100,900},{0,0,1920,1080},96,96,{480,360},{720,760},{600,160,1320,920}},
        {"negative monitor clamp",{-1700,-100,-1100,600},{-1600,0,0,900},96,96,{480,360},{720,760},{-1600,0,-1000,700}},
        {"removed monitor clamp",{3000,2000,3600,2500},{0,0,1920,1080},96,96,{480,360},{720,760},{1320,580,1920,1080}},
        {"oversized saved fills work",{-100,-100,2900,1900},{0,0,1920,1080},96,96,{480,360},{720,760},{0,0,1920,1080}},
        {"minimum bounded to tiny screen",{200,250,201,251},{100,200,420,440},96,96,{480,360},{720,760},{100,200,420,440}},
        {"zero DPI defaults96",{10,20,730,780},{0,0,3840,2160},0,144,{720,540},{1080,1140},{10,20,1090,1160}},
        {"invalid size hints",{0,0,0,0},{0,0,101,99},96,96,{0,-1},{-3,0},{50,49,51,50}},
        {"extreme DPI bounded",{-2147483647-1,-2147483647-1,2147483647,2147483647},{0,0,1920,1080},1,(std::numeric_limits<UINT>::max)(),{480,360},{720,760},{0,0,1920,1080}},
    };
    for(const auto& item:cases) {
        const auto actual=shell::desktop_bounds(item.saved,item.work,item.from,item.to,item.minimum,item.initial);
        expect_rect(actual,item.expected,item.name);
        require(actual.right>actual.left&&actual.bottom>actual.top,"desktop lost its positive size");
        require(actual.left>=item.work.left&&actual.top>=item.work.top&&actual.right<=item.work.right&&actual.bottom<=item.work.bottom,
            "desktop escaped its monitor work area");
    }
    constexpr auto low=(std::numeric_limits<LONG>::min)();
    expect_rect(shell::desktop_bounds({low+10,low+10,low+730,low+770},{low,low,low+1000,low+800},96,96,{480,360},{720,760}),
        {low+10,low+10,low+730,low+770},"extreme negative monitor coordinates");
    expect_rect(shell::desktop_bounds({}, {10,20,5,10},96,96,{480,360},{720,760}),{10,20,10,20},"invalid desktop work area");
}
void test_native_window_styles() {
    struct Class {
        HINSTANCE instance=GetModuleHandleW(nullptr);
        std::wstring name=L"SyntheticShinShell-"+std::to_wstring(GetCurrentProcessId());
        Class() {
            WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=instance;type.lpszClassName=name.c_str();
            require(RegisterClassW(&type)!=0,"native shell fixture class registration failed");
        }
        ~Class() {UnregisterClassW(name.c_str(),instance);}
    } type;
    struct Window {HWND value;~Window(){if(value)DestroyWindow(value);}};
    Window desktop{CreateWindowExW(shell::desktop_extended_style,type.name.c_str(),L"Synthetic desktop shell",shell::desktop_style,
        100,100,720,760,nullptr,nullptr,type.instance,nullptr)};
    Window quick{CreateWindowExW(shell::quick_extended_style,type.name.c_str(),L"Synthetic quick shell",shell::quick_style,
        300,200,280,180,nullptr,nullptr,type.instance,nullptr)};
    require(desktop.value&&quick.value&&desktop.value!=quick.value,"independent native shell windows were not created");
    const auto desktop_style=static_cast<DWORD>(GetWindowLongPtrW(desktop.value,GWL_STYLE));
    const auto desktop_extended=static_cast<DWORD>(GetWindowLongPtrW(desktop.value,GWL_EXSTYLE));
    require((desktop_style&WS_OVERLAPPEDWINDOW)==WS_OVERLAPPEDWINDOW&&(desktop_style&WS_POPUP)==0,"desktop is missing its normal move/resize/minimize/maximize frame");
    require((desktop_extended&WS_EX_APPWINDOW)!=0&&(desktop_extended&(WS_EX_TOPMOST|WS_EX_TOOLWINDOW))==0,"desktop has quick-popup ownership/style semantics");
    require(GetWindow(desktop.value,GW_OWNER)==nullptr&&GetAncestor(desktop.value,GA_ROOT)==desktop.value,"desktop is owned by another window");
    const auto quick_style=static_cast<DWORD>(GetWindowLongPtrW(quick.value,GWL_STYLE));
    const auto quick_extended=static_cast<DWORD>(GetWindowLongPtrW(quick.value,GWL_EXSTYLE));
    require((quick_style&WS_POPUP)!=0&&(quick_style&(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX))==0,"quick panel inherited the desktop frame");
    require((quick_extended&(WS_EX_TOOLWINDOW|WS_EX_TOPMOST))==(WS_EX_TOOLWINDOW|WS_EX_TOPMOST)&&
        (quick_extended&WS_EX_APPWINDOW)==0,"quick panel is not a topmost native tool popup");
    RECT actual{};require(GetWindowRect(desktop.value,&actual)!=FALSE,"desktop bounds unavailable");
    expect_rect(actual,{100,100,820,860},"native desktop outer frame");
    require(SetWindowPos(desktop.value,nullptr,140,160,840,600,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"native desktop move/resize failed");
    require(GetWindowRect(desktop.value,&actual)!=FALSE,"moved desktop bounds unavailable");
    expect_rect(actual,{140,160,980,760},"native desktop moved and resized");
    require(GetWindowRect(quick.value,&actual)!=FALSE,"quick popup bounds unavailable");
    expect_rect(actual,{300,200,580,380},"quick window remains independent of desktop move");
    require(!IsWindowVisible(desktop.value)&&!IsWindowVisible(quick.value),"native shell fixtures unexpectedly took over the desktop");
}
int main() {
    try {
        test_quick_bounds();test_desktop_bounds();test_native_window_styles();
        std::cout<<"Windows quick geometry, DPI-restored desktop placement and native HWND shell styles passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
