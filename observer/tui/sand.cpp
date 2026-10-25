#include "sand.h"
#include "chart.h"
#include "../ui/project_diagnostics.h"
#include <ftxui/dom/canvas.hpp>
#include <ftxui/screen/string.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace shin::observer::tui {
namespace {
using namespace ftxui;
// FTXUI resolves RGB colors against current terminal capabilities at creation.
// Construct on the render thread, after the app's terminal setup, never during
// static initialization (which can permanently reduce Sand to monochrome).
struct Palette {
    Color background = Color::RGB(29, 31, 28);
    Color selection = Color::RGB(54, 60, 46), rule_ink = Color::RGB(98, 107, 85);
    Color sand = Color::RGB(228, 204, 162), ink = Color::RGB(240, 239, 227), muted = Color::RGB(176, 181, 166);
    Color green = Color::RGB(130, 212, 155), amber = Color::RGB(244, 192, 98);
    Color red = Color::RGB(245, 140, 131), blue = Color::RGB(130, 192, 240), idle = Color::RGB(165, 183, 202);
};
const Json& field(const Json& value, const char* key) {
    static const Json absent;
    const auto at = value.is_object() ? value.find(key) : value.end();
    return at == value.end() ? absent : *at;
}
std::string string(const Json& value) { return value.is_string() ? value.get<std::string>() : ui::tr("unknown"); }
std::string number(const Json& value, double divisor = 1, int precision = -1) {
    if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0) return "—";
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision >= 0 ? precision : divisor == 1 ? 0 : 1) << value.get<double>() / divisor;
    return ui::decimal(out.str());
}
Color tone_color(Tone tone, const Palette& colors) {
    switch (tone) {
    case Tone::heading: case Tone::selected: return colors.sand;
    case Tone::good: return colors.green;
    case Tone::warning: return colors.amber;
    case Tone::error: return colors.red;
    case Tone::info: return colors.blue;
    case Tone::idle: return colors.idle;
    case Tone::muted: return colors.muted;
    default: return colors.ink;
    }
}
struct Painter {
    int columns;
    bool ascii, monochrome;
    Palette colors{};
    Element label(std::string_view value, Tone tone = Tone::normal, int width = -1) const {
        auto element = text(terminal_text(value, width < 0 ? columns : width));
        if (!monochrome) element = element | color(tone_color(tone, colors));
        if (tone == Tone::heading || tone == Tone::selected) element = element | bold;
        if (width >= 0) element = element | size(WIDTH, EQUAL, width);
        return element;
    }
    Element section(std::string_view value) const {
        return hbox({label(ascii ? "-- " : "── ", Tone::muted), label(value, Tone::heading, std::min(columns - 5, ftxui::string_width(std::string(value)))),
            label(" "), separator() | (monochrome ? nothing : color(colors.rule_ink)) | flex});
    }
    Element pair(std::string_view left, std::string_view right, Tone tone = Tone::muted) const {
        return hbox({label(left, tone), filler(), label(right, tone)});
    }
};
Element line_chart(const Chart& data, const Painter& paint, int height, bool connected) {
    const int gutter = 8, width = std::max(8, paint.columns - gutter - 1);
    const int plot_height = std::clamp(height, 2, 10);
    const auto tint = paint.monochrome ? Color::Default : !connected ? paint.colors.muted :
        data.unit == "MiB" ? paint.colors.blue : data.unit == "ms" ? paint.colors.sand : paint.colors.green;
    Canvas canvas(width * 2, plot_height * 4);
    const int xmax = canvas.width() - 1, ymax = canvas.height() - 1;
    const auto grid_color = paint.monochrome ? Color::Default : paint.colors.rule_ink;
    for (int row : {0, plot_height / 2, plot_height - 1})
        canvas.DrawText(0, row * 4, std::string(static_cast<std::size_t>(width), '.'), grid_color);
    std::optional<std::pair<int, int>> previous;
    std::size_t observed = 0;
    for (const auto& point : data.points) {
        if (!point.value) { previous.reset(); continue; }
        ++observed;
        const double span = std::max(1.0, data.end - data.begin);
        const int x = std::clamp(static_cast<int>(std::lround((point.at - data.begin) / span * xmax)), 0, xmax);
        const int y = std::clamp(static_cast<int>(std::lround((1 - *point.value / data.maximum) * ymax)), 0, ymax);
        if (paint.ascii) {
            if (point.connect && previous) {
                const int steps = std::max(1, std::abs(x - previous->first));
                for (int i = 0; i <= steps; ++i) {
                    const int xx = previous->first + (x - previous->first) * i / steps;
                    const int yy = previous->second + (y - previous->second) * i / steps;
                    canvas.DrawText(xx / 2 * 2, yy / 4 * 4, "*", tint);
                }
            }
            canvas.DrawText(x / 2 * 2, y / 4 * 4, "*", tint);
        } else {
            if (point.connect && previous) canvas.DrawPointLine(previous->first, previous->second, x, y, tint);
            canvas.DrawPointOn(x, y);
            canvas.Style(x / 2 * 2, y / 4 * 4, [&](Cell& cell) { cell.foreground_color = tint; });
        }
        previous = std::pair{x, y};
    }
    Elements axis;
    for (int row = 0; row < plot_height; ++row) {
        const bool tick = row == 0 || row == plot_height / 2 || row == plot_height - 1;
        const double value = row == 0 ? data.maximum : row == plot_height - 1 ? 0 : data.maximum / 2;
        std::string value_text = tick ? number(value) : "";
        axis.push_back(paint.label(terminal_text(value_text, 5, true) + (paint.ascii ? " | " : " │ "), Tone::muted, gutter));
    }
    auto plot = hbox({vbox(std::move(axis)), ftxui::canvas(std::move(canvas))});
    const char* period = data.retained ? ui::tr("Interval averages; gaps are not zeroes.") : ui::tr("Live observations; gaps are not zeroes.");
    Elements result{plot};
    const std::string left = data.retained ? ui::tr("period start") : "-60 s";
    result.push_back(paint.pair(left, connected ? ui::tr("now") : ui::tr("last observation")));
    if (!observed) result.push_back(paint.label(ui::tr("No measurements for this period"), Tone::muted));
    else result.push_back(paint.label(std::string(period) + " · " + data.unit, Tone::muted));
    if (data.partial) result.push_back(paint.label(ui::tr("Partial measurements; invalid rows omitted."), Tone::warning));
    return vbox(std::move(result));
}
Element project_table(const Snapshot& snapshot, State& state, const Painter& paint, HitTargets& hits, int capacity) {
    const auto& rows = field(field(snapshot.service, "telemetry"), "project_states");
    if (!rows.is_array() || rows.empty()) return vbox({
        paint.label(ui::tr("No observed projects."), Tone::muted),
        paint.label("shin-ce project-add --root PATH", Tone::heading)});
    select_project(snapshot, state, 0);
    const bool wide = paint.columns >= 94;
    const int name_width = wide ? 28 : 25, status_width = wide ? 20 : 18;
    const int coverage_width = std::max(8, paint.columns - name_width - status_width - (wide ? 24 : 0) - 2);
    Elements result;
    Elements header{paint.label("  "), paint.label(ui::tr("Project"), Tone::muted, name_width),
        paint.label(ui::tr("Search"), Tone::muted, status_width)};
    if (wide) header.push_back(paint.label(ui::tr("Observer"), Tone::muted, 24));
    header.push_back(paint.label(ui::tr("Coverage"), Tone::muted, coverage_width));
    result.push_back(hbox(std::move(header)));
    std::size_t selected = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) if (field(rows[i], "root") == state.project) selected = i;
    const int count = static_cast<int>(std::min<std::size_t>(16, rows.size()));
    capacity = std::clamp(capacity, 1, count);
    const bool show_position = count > capacity && capacity > 1;
    if (show_position) --capacity;
    const int start = std::clamp(static_cast<int>(selected) - capacity / 2, 0, count - capacity);
    for (int i = start; i < start + capacity; ++i) {
        const auto& row = rows[static_cast<std::size_t>(i)];
        const bool chosen = field(row, "root") == state.project;
        const auto status = project_status(row, fresh(snapshot));
        const auto& coverage = field(row, "source_coverage");
        const bool partial = field(coverage, "state") == "partial";
        const std::string coverage_text = !fresh(snapshot) ? ui::tr("unknown") :
            partial ? ui::tr("! Partial") : field(coverage, "state") == "complete" ? ui::tr("Complete") :
            field(coverage, "state").is_string() ? ui::tr(string(field(coverage, "state")).c_str()) : "—";
        Elements cells{paint.label(chosen ? (paint.ascii ? "> " : "› ") : "  ", Tone::selected),
            paint.label(string(field(row, "name")), chosen ? Tone::selected : Tone::normal, name_width),
            paint.label(status.text, status.tone, status_width)};
        if (wide) cells.push_back(paint.label(ui::tr(string(field(row, "phase")).c_str()), Tone::muted, 24));
        cells.push_back(paint.label(coverage_text, partial && fresh(snapshot) ? Tone::warning : Tone::muted, coverage_width));
        const auto at = hits.project_count++;
        hits.roots[at] = string(field(row, "root"));
        auto element = hbox(std::move(cells)) | reflect(hits.projects[at]);
        if (chosen) element = element | (paint.monochrome ? inverted : bgcolor(paint.colors.selection));
        result.push_back(element);
    }
    if (show_position) result.push_back(paint.label(
        std::to_string(start + 1) + "–" + std::to_string(start + capacity) + " / " + std::to_string(count), Tone::muted));
    return vbox(std::move(result));
}
Element text_page(const Snapshot& snapshot, State& state, const Painter& paint, int available) {
    std::vector<Line> body;
    for (const auto& line : lines(snapshot, state)) {
        auto remaining = terminal_text(line.text, 4096);
        if (remaining.empty()) body.push_back(line);
        while (!remaining.empty()) {
            auto part = terminal_text(remaining, paint.columns - 2);
            if (part.size() < remaining.size()) {
                const auto space = part.rfind(' ');
                if (space != std::string::npos && space > part.size() / 2) part.resize(space);
            }
            if (part.empty()) break;
            body.push_back({part, line.tone});
            remaining.erase(0, part.size());
            while (!remaining.empty() && remaining.front() == ' ') remaining.erase(0, 1);
        }
    }
    state.scroll = std::clamp(state.scroll, 0, std::max(0, static_cast<int>(body.size()) - available));
    Elements visible;
    for (int i = 0; i < available && state.scroll + i < static_cast<int>(body.size()); ++i) {
        const auto& line = body[static_cast<std::size_t>(state.scroll + i)];
        visible.push_back(line.tone == Tone::heading ? paint.section(line.text) : paint.label(line.text, line.tone));
    }
    return vbox(std::move(visible));
}
}
HitTargets::HitTargets() {
    const ftxui::Box absent{1, 0, 1, 0};
    tabs.fill(absent); metrics.fill(absent); ranges.fill(absent); projects.fill(absent);
    language = back = absent;
}
ftxui::Element sand_view(const Snapshot& snapshot, State& state, int columns, int height,
                         HitTargets& hits, bool ascii, bool monochrome) {
    using namespace ftxui;
    hits = HitTargets{};
    columns = std::clamp(columns, 1, 512); height = std::clamp(height, 1, 256);
    const Painter paint{std::max(1, columns - 2), ascii, monochrome};
    if (columns < 60 || height < 12)
        return paint.label(ui::tr("Shin: resize to at least 60 x 12. q quits."), Tone::warning) |
            size(WIDTH, EQUAL, columns) | size(HEIGHT, EQUAL, height);
    const auto& telemetry = field(snapshot.service, "telemetry");
    const bool online = fresh(snapshot);
    std::string age = "—";
    if (snapshot.received != Clock::time_point{}) age = number(
        std::chrono::duration<double>(Clock::now() - snapshot.received).count());
    const auto connection = online ? ui::tr("LIVE") : ui::tr("OFFLINE");
    const auto language = ui::language() == ui::Language::en ? "L RU" : "L EN";
    const std::string connection_text = std::string(online ? "● " : "○ ") + connection + " · " +
        ui::tr("Observation age: ") + age + ui::tr(" s") +
        (columns >= 100 ? std::string(" · ") + ui::tr("read-only monitor") : "");
    Elements header{
        hbox({paint.label("SHIN  Context Engine", Tone::heading), filler(),
            paint.label(SHIN_OBSERVER_VERSION, Tone::muted), paint.label("   "),
            paint.label(language, Tone::heading) | reflect(hits.language)}),
        hbox({paint.label(connection_text, online ? Tone::good : Tone::warning), filler(),
            paint.label(std::string(ui::tr("Uptime: ")) + (online ? number(field(telemetry, "uptime_ms"), 1000) + ui::tr(" s") : "—"), Tone::muted)})};
    const std::array names{ui::tr("Overview"), ui::tr("Projects"), ui::tr("Timing"), ui::tr("Storage"), ui::tr("Models"), ui::tr("Sessions")};
    Elements tabs;
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto element = paint.label(std::to_string(i + 1) + " " + names[i],
            static_cast<int>(i) == state.tab ? Tone::selected : Tone::muted, paint.columns / 6) | reflect(hits.tabs[i]);
        if (static_cast<int>(i) == state.tab) element = element | underlined;
        tabs.push_back(element);
    }
    Elements body;
    const int available = height - 7;
    if (state.help) body.push_back(text_page(snapshot, state, paint, available));
    else if (state.tab == 0) {
        if (!online && height >= 22) {
            body.push_back(paint.label(ui::tr("Service offline / observation unavailable"), Tone::warning));
            body.push_back(paint.label(ui::tr("Retained values below are from the last successful observation."), Tone::muted));
        }
        if (height >= 22) {
            const auto& samples = field(telemetry, "samples");
            const Json rss = samples.is_array() && !samples.empty() ? field(samples.back(), "rss_bytes") : Json();
            const std::array labels{ui::tr("Memory"), ui::tr("Active queries"), ui::tr("Search, p50")};
            const std::array values{
                online ? number(rss, 1048576) + " MiB" : "—",
                online ? number(field(snapshot.service, "query_workers")) : "—",
                online ? number(field(field(telemetry, "request_metrics"), "query_p50_ms"), 1.0, 1) + " ms" : "—"};
            Elements counters;
            for (std::size_t i = 0; i < labels.size(); ++i) {
                auto element = vbox({paint.label(labels[i], static_cast<int>(i) == state.metric ? Tone::heading : Tone::muted),
                    paint.label(values[i], static_cast<int>(i) == state.metric ? Tone::selected : Tone::normal)}) |
                    size(WIDTH, EQUAL, paint.columns / 3) | reflect(hits.metrics[i]);
                counters.push_back(element);
            }
            body.push_back(hbox(std::move(counters))); body.push_back(text(""));
            const auto data = chart(snapshot, state.metric, state.range);
            const std::array range_names{online ? ui::tr("Live") : ui::tr("Retained"), ui::tr("1 h"), ui::tr("24 h")};
            Elements ranges;
            for (std::size_t i = 0; i < range_names.size(); ++i)
                ranges.push_back(paint.label(std::string(" ") + range_names[i] + " ",
                    state.range == static_cast<int>(i) ? Tone::selected : Tone::muted) | reflect(hits.ranges[i]));
            body.push_back(hbox({paint.label(data.title, Tone::heading, std::max(1, paint.columns - 24)),
                filler(), hbox(std::move(ranges))}));
            body.push_back(line_chart(data, paint, std::clamp(height - 26 - (online ? 0 : 2), 2, 8), online));
            body.push_back(text(""));
        } else body.push_back(paint.label(ui::tr("Service overview"), Tone::heading));
        const auto& rows = field(telemetry, "project_states");
        body.push_back(paint.section(std::string(ui::tr("Projects")) + " · " + (rows.is_array() ? std::to_string(rows.size()) : "—")));
        const int used = height >= 22 ? 10 + std::clamp(height - 26 - (online ? 0 : 2), 2, 8) +
            (height >= 26 ? 1 : 0) + (online ? 0 : 2) : 3;
        body.push_back(project_table(snapshot, state, paint, hits, std::max(1, available - used)));
        const auto& history = field(telemetry, "history");
        if (height >= 26) body.push_back(paint.label(std::string(ui::tr("History: ")) +
            ui::tr(string(field(history, "status")).c_str()) + " · " + ui::tr("Retained records: ") + number(field(history, "rows")), Tone::muted));
        if (height >= 22) {
            const auto& responses = field(telemetry, "request_metrics");
            body.push_back(paint.label(std::string(ui::tr("Responses this run (ready/error/unobserved): ")) +
                number(field(responses, "responses_ready")) + " / " + number(field(responses, "response_errors")) +
                " / " + number(field(responses, "unobserved")), Tone::muted));
        }
    } else if (state.tab == 1 && !state.detail) {
        body.push_back(paint.section(ui::tr("Projects")));
        body.push_back(project_table(snapshot, state, paint, hits, available - 5));
        body.push_back(text(""));
        body.push_back(paint.label(ui::tr("Up/Down select  |  Enter details"), Tone::muted));
        body.push_back(paint.label(ui::tr("Search availability and source coverage are separate."), Tone::muted));
    } else {
        int text_height = available;
        if (state.tab == 1 && state.detail) {
            body.push_back(paint.label(ui::tr("Esc back"), Tone::heading) | reflect(hits.back));
            --text_height;
        }
        if (state.tab == 2 && height >= 26) {
            const auto data = chart(snapshot, 2, 0, state.traffic);
            body.push_back(paint.label(data.title + " · " + data.unit, Tone::heading));
            body.push_back(line_chart(data, paint, 4, online));
            text_height -= 7;
        }
        body.push_back(text_page(snapshot, state, paint, std::max(1, text_height)));
    }
    header.push_back(separator() | (monochrome ? nothing : color(paint.colors.rule_ink)));
    header.push_back(hbox(std::move(tabs)));
    header.push_back(text(""));
    header.push_back(vbox(std::move(body)) | yframe | size(HEIGHT, EQUAL, available));
    header.push_back(separator() | (monochrome ? nothing : color(paint.colors.rule_ink)));
    const auto hint = columns < 80 ? ui::tr("1-6 tabs  Enter details  ? help  q quit") :
        state.tab == 0 ? ui::tr("Enter details  g metric  h period  L EN/RU  ? help  q quit") :
        state.tab == 2 ? ui::tr("t recent/history  f traffic  j/k scroll  L EN/RU  q quit") :
        state.tab == 4 ? ui::tr("p usage project  j/k scroll  L EN/RU  ? help  q quit") :
        ui::tr("1-6 tabs  j/k scroll  Enter details  r refresh  ? help  q quit");
    header.push_back(paint.label(hint, Tone::muted));
    auto result = vbox(std::move(header)) | size(WIDTH, EQUAL, paint.columns) |
        size(HEIGHT, EQUAL, height) | center;
    if (!monochrome) result = result | bgcolor(paint.colors.background) | color(paint.colors.ink);
    return result;
}
} // namespace shin::observer::tui
