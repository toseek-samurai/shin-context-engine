#pragma once
#include "view.h"
#include <ftxui/dom/elements.hpp>
#include <array>

namespace shin::observer::tui {
struct HitTargets {
    std::array<ftxui::Box, 5> tabs;
    std::array<ftxui::Box, 3> metrics, ranges;
    std::array<ftxui::Box, 16> projects;
    std::array<std::string, 16> roots;
    ftxui::Box language, back;
    std::size_t project_count = 0;
    HitTargets();
};
// Uses the same model in interactive and deterministic render tests.
ftxui::Element sand_view(const Snapshot&, State&, int columns, int height,
                         HitTargets&, bool ascii = false, bool monochrome = false);
} // namespace shin::observer::tui
