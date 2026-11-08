#pragma once
#include <algorithm>
#include <cstdint>
#include <windows.h>

namespace shin::tray::windows_shell {
inline constexpr DWORD desktop_style=WS_OVERLAPPEDWINDOW;
inline constexpr DWORD desktop_extended_style=WS_EX_APPWINDOW;
inline constexpr DWORD quick_style=WS_POPUP;
inline constexpr DWORD quick_extended_style=WS_EX_TOOLWINDOW|WS_EX_TOPMOST;

namespace detail {
using Coordinate=std::int64_t;
inline bool valid(const RECT& rect) {return rect.right>rect.left&&rect.bottom>rect.top;}
inline RECT empty_at(const RECT& work) {return {work.left,work.top,work.left,work.top};}
inline RECT placed(Coordinate x,Coordinate y,Coordinate width,Coordinate height,
                   Coordinate left,Coordinate top,Coordinate right,Coordinate bottom) {
    x=std::clamp(x,left,right-width);y=std::clamp(y,top,bottom-height);
    return {static_cast<LONG>(x),static_cast<LONG>(y),static_cast<LONG>(x+width),static_cast<LONG>(y+height)};
}
inline Coordinate scaled(Coordinate size,UINT from,UINT to,Coordinate maximum) {
    const auto source=from?from:96U,target=to?to:96U;
    const auto value=(static_cast<std::uint64_t>(size)*target+source/2U)/source;
    return static_cast<Coordinate>((std::min)(value,static_cast<std::uint64_t>(maximum)));
}
}

inline RECT quick_bounds(const RECT& anchor,const RECT& work,SIZE desired,int margin) {
    using detail::Coordinate;
    if(!detail::valid(work)) return detail::empty_at(work);
    const Coordinate available_width=static_cast<Coordinate>(work.right)-work.left;
    const Coordinate available_height=static_cast<Coordinate>(work.bottom)-work.top;
    const Coordinate gap=(std::max)(margin,0);
    const Coordinate inset_x=(std::min)(gap,(available_width-1)/2);
    const Coordinate inset_y=(std::min)(gap,(available_height-1)/2);
    const Coordinate left=work.left+inset_x,top=work.top+inset_y,right=work.right-inset_x,bottom=work.bottom-inset_y;
    const Coordinate width=std::clamp<Coordinate>(desired.cx,1,right-left);
    const Coordinate height=std::clamp<Coordinate>(desired.cy,1,bottom-top);
    const Coordinate anchor_left=(std::min)(anchor.left,anchor.right),anchor_right=(std::max)(anchor.left,anchor.right);
    const Coordinate anchor_top=(std::min)(anchor.top,anchor.bottom),anchor_bottom=(std::max)(anchor.top,anchor.bottom);
    Coordinate x=anchor_right-width,y=(std::min)(bottom,anchor_top-gap)-height;
    if(anchor_right<=work.left) {
        x=anchor_right+gap;y=anchor_bottom-height;
    } else if(anchor_left>=work.right) {
        x=anchor_left-gap-width;y=anchor_bottom-height;
    } else if(y<top) {
        const auto below=(std::max)(top,anchor_bottom+gap);
        if(below+height<=bottom) y=below;
    }
    return detail::placed(x,y,width,height,left,top,right,bottom);
}

// minimum/initial are already in target-DPI physical pixels. Absolute monitor
// coordinates are not scaled: only dimensions stored at saved_dpi are restored.
inline RECT desktop_bounds(const RECT& saved,const RECT& work,UINT saved_dpi,UINT target_dpi,SIZE minimum,SIZE initial) {
    using detail::Coordinate;
    if(!detail::valid(work)) return detail::empty_at(work);
    const Coordinate available_width=static_cast<Coordinate>(work.right)-work.left;
    const Coordinate available_height=static_cast<Coordinate>(work.bottom)-work.top;
    const Coordinate min_width=std::clamp<Coordinate>(minimum.cx,1,available_width);
    const Coordinate min_height=std::clamp<Coordinate>(minimum.cy,1,available_height);
    const bool restore=detail::valid(saved);
    const Coordinate width=std::clamp<Coordinate>(restore?
        detail::scaled(static_cast<Coordinate>(saved.right)-saved.left,saved_dpi,target_dpi,available_width):initial.cx,
        min_width,available_width);
    const Coordinate height=std::clamp<Coordinate>(restore?
        detail::scaled(static_cast<Coordinate>(saved.bottom)-saved.top,saved_dpi,target_dpi,available_height):initial.cy,
        min_height,available_height);
    const Coordinate x=restore?saved.left:work.left+(available_width-width)/2;
    const Coordinate y=restore?saved.top:work.top+(available_height-height)/2;
    return detail::placed(x,y,width,height,work.left,work.top,work.right,work.bottom);
}
}
