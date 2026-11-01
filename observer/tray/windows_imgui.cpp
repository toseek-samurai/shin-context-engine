#include "imgui.h"
#include "imgui_impl_win32.h"
#include "windows_dpi.h"
#include <cfloat>
namespace {
float dpi_scale=1;
float logical_coordinate(float value) {return value<=-FLT_MAX?value:value/dpi_scale;}
}
void shin_imgui_win32_scale(float scale) {dpi_scale=scale>0?scale:1;}
// Adapt the pinned upstream backend at its public mouse-event boundary. Scaling
// only after NewFrame would let event trickling process physical clicks first.
// Keep event order, source, capture, tracking, wheel and keyboard behavior intact.
#define AddMousePosEvent(x,y) AddMousePosEvent(logical_coordinate(x),logical_coordinate(y))
#include "imgui_impl_win32.cpp"
#undef AddMousePosEvent
