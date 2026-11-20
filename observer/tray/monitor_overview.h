#pragma once

void history_chart(const shin::ui::RetainedCharts& history, shin::ui::HistoryMetric metric, bool connected, float height) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = 37.0f, plot_width = std::max(1.0f, width - label_width - 2);
    const float bottom = height - 21, amplitude = std::max(1.0f, bottom - 7);
    const bool memory = metric == shin::ui::HistoryMetric::memory;
    const bool search = metric == shin::ui::HistoryMetric::search_time;
    const auto tint = !connected ? muted : memory ? sand : search ? blue : green;
    auto* draw = ImGui::GetWindowDrawList();
    const double now = history.end, start = history.begin, interval = history.interval;
    const bool minutes = interval == 60000;
    const auto index = static_cast<std::size_t>(metric);
    const auto& points = history.series[index];
    const auto peak = history.peaks[index];
    double maximum = peak.value_or(0), minimum = maximum;
    if (memory) for (const auto& point : points) if (point.value) minimum = std::min(minimum, *point.value);
    const double step = memory || search ? 25 : maximum > 10 ? 10 : 1;
    minimum = memory && peak ? std::floor(minimum / step) * step : 0;
    maximum = std::max(minimum + step, std::ceil(maximum / step) * step);
    const auto to_y = [&](double value) { return origin.y + bottom - static_cast<float>((value - minimum) / (maximum - minimum)) * amplitude; };
    for (int i = 0; i < (memory ? 3 : 2); ++i) {
        const double value = memory ? maximum - (maximum - minimum) * i / 2 : (i == 0 ? maximum : minimum);
        const float y = to_y(value);
        draw->AddLineH(origin.x + label_width, origin.x + width, y, ImGui::GetColorU32(rule));
        if (peak) {
            const auto label = number(value, "");
            draw->AddText({origin.x, y - 6}, ImGui::GetColorU32(muted), label.c_str());
        }
    }
    ImVec2 previous{};
    for (const auto& observation : points) {
        if (!observation.value || now <= start) continue;
        const auto x = std::clamp(static_cast<float>((observation.at - start) / (now - start)), 0.0f, 1.0f) * plot_width;
        const auto color = ImGui::GetColorU32(tint);
        const ImVec2 point{origin.x + label_width + x, to_y(*observation.value)};
        if (observation.connect) {
            draw->AddQuadFilled(previous, point, {point.x, origin.y + bottom}, {previous.x, origin.y + bottom}, alpha_color(tint, .11f));
            draw->AddLine(previous, point, color, 1.8f);
        } else draw->AddCircleFilled(point, 2.0f, color);
        if (memory && observation.value == peak) draw->AddCircleFilled(point, 3.0f, color);
        previous = point;
    }
    if (peak) {
        const auto from = minutes ? clock_label(static_cast<std::int64_t>(start)) : day_label(static_cast<std::int64_t>(start));
        const auto until = minutes ? clock_label(static_cast<std::int64_t>(now)) : day_label(static_cast<std::int64_t>(now));
        draw->AddText({origin.x + label_width, origin.y + bottom + 4}, ImGui::GetColorU32(muted), from.c_str());
        draw->AddText({origin.x + width - ImGui::CalcTextSize(until.c_str()).x, origin.y + bottom + 4}, ImGui::GetColorU32(muted), until.c_str());
    } else {
        const auto empty = clipped(shin::ui::tr("No measurements for this period"), width - 12);
        draw->AddText({origin.x + 6, origin.y + bottom / 2}, ImGui::GetColorU32(muted), empty.c_str());
    }
    ImGui::Dummy({width, height});
    if (peak && now > start && ImGui::IsItemHovered()) {
        const double at = start + std::clamp((ImGui::GetIO().MousePos.x - origin.x - label_width) / plot_width, 0.0f, 1.0f) * (now - start);
        const auto bucket = std::find_if(points.begin(), points.end(), [&](const auto& point) {
            return at >= point.at && at < point.at + interval;
        });
        ImGui::BeginTooltip();
        if (bucket != points.end() && bucket->value) {
            const double time = bucket->at;
            const char* unit = memory ? shin::ui::tr("MiB") : search ? shin::ui::tr("ms") : shin::ui::tr(minutes ? "/ min" : "/ h");
            ImGui::Text("%s · %s", clock_label(static_cast<std::int64_t>(time)).c_str(), number(*bucket->value, unit).c_str());
            const float x = origin.x + label_width + std::clamp(static_cast<float>((time - start) / (now - start)), 0.0f, 1.0f) * plot_width;
            const float y = to_y(*bucket->value);
            draw->AddLineV(x, origin.y + 4, origin.y + bottom, alpha_color(muted, .5f));
            draw->AddCircleFilled({x, y}, 2.5f, ImGui::GetColorU32(connected ? sand : muted));
        } else ImGui::Text("%s · %s", clock_label(static_cast<std::int64_t>(at)).c_str(), shin::ui::tr("Not measured"));
        ImGui::EndTooltip();
    }
}

void history_state(const Json& history, bool connected, bool partial) {
    ImGui::PushFont(nullptr, 13);
    const auto state = history.value("status", "disabled");
    const bool gaps = history.value("dropped_current_run", 0ULL) || history.value("unconfirmed_current_run", 0ULL) || history.value("future_rows", 0ULL);
    const char* label = !connected ? shin::ui::tr("History: no fresh response") : state == "ready" ?
        (gaps ? shin::ui::tr("History: gaps present") : shin::ui::tr("History saved locally")) :
        state == "starting" ? shin::ui::tr("Connecting history") : shin::ui::tr("History unavailable");
    status_badge(!connected ? Icon::offline : state == "ready" ? (gaps ? Icon::blocked : Icon::history) :
        state == "starting" ? Icon::pending : Icon::blocked, label, connected && (gaps || state != "ready") ? amber : muted, 51, 15);
    ImGui::SameLine();
    if (connected && partial) ImGui::TextColored(muted, "%s", shin::ui::tr("Reduced detail"));
    else if (connected && state == "ready") ImGui::TextColored(muted, shin::ui::tr("History · records: %llu"), history.value("rows", 0ULL));
    else ImGui::TextColored(muted, "%s", shin::ui::tr("History"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("%sRecords: %llu · %.0f KiB\n%s\nQueued: %llu · Dropped: %llu · Pruned: %llu"),
        partial ? shin::ui::tr("Some details were shortened only in this response.\nRetained history and current statuses were preserved.\n") : "",
        history.value("rows", 0ULL), history.value("database_bytes", 0.0) / 1024,
        history.value("error_code", "").c_str(), history.value("pending", 0ULL), history.value("dropped_current_run", 0ULL), history.value("pruned_current_run", 0ULL));
    ImGui::PopFont();
}

void render_overview(const ViewState& view, const Json& telemetry, const Json& history) {
    using shin::ui::tr;
    using shin::ui::HistoryMetric;
    const auto metrics = telemetry.value("request_metrics", Json::object());
    const auto samples = telemetry.value("samples", Json::array());
    const auto recent = metrics.value("recent", Json::array());
    const auto retained = shin::ui::retained_charts(history, history_hours);
    const auto projection = telemetry.value("projection", Json::object());
    const bool reduced = history_hours && (retained.partial || (projection.is_object() &&
        shin::ui::count(projection.value("omitted_rows", Json::object()),
            history_hours == 1 ? "/history/minute_timeline" : "/history/timeline") != 0));
    auto peak = retained.peaks[static_cast<std::size_t>(HistoryMetric::memory)];
    if (!history_hours) for (const auto& row : samples)
        if (const auto rss = shin::ui::telemetry_measurement(row.at("rss_bytes")))
            peak = std::max(peak.value_or(0), *rss / 1048576);
    const bool compact = layout_height() < 570 || ImGui::GetIO().DisplaySize.x < 420;
    const bool minimal_charts = layout_height() <= 440;
    const bool visible = view.connected || !telemetry.empty();
    const bool live_memory = view.connected && !samples.empty() && samples.back().at("rss_bytes").is_number();
    const auto memory = live_memory ? number(samples.back().at("rss_bytes").get<double>() / 1048576, tr("MiB")) : "—";
    const bool time_known = visible && (history_hours ? !reduced && retained.mean_search_ms.has_value() :
        metrics.value("query_sample_count", 0ULL) > 0 && metrics.value("query_p50_ms", Json()).is_number());
    const auto duration = time_known ? number(history_hours ? *retained.mean_search_ms :
        metrics.at("query_p50_ms").get<double>(), tr("ms")) : "—";
    const auto requests = visible && (history_hours ? retained.available && !reduced : !metrics.empty()) ?
        std::to_string(history_hours ? retained.requests : telemetry.value("requests_started", 0ULL)) : "—";
    const char* period = !history_hours ? tr("Now ▾") : history_hours == 1 ? tr("Last hour ▾") :
        history_hours == 24 ? tr("24 hours ▾") : tr("7 days ▾");
    const float period_width = ImGui::CalcTextSize(period).x + 20;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    page_title(tr("Statistics"));
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 4,
        right - period_width));
    view_tab(period, 44, false, period_width);
    if (!view.connected && !telemetry.empty())
        ImGui::TextColored(muted, "%s", tr("Offline · last received data"));
    if (reduced) ImGui::TextColored(amber, "%s", tr("Reduced detail"));
    begin_surface("overview-memory", ImVec4{.159f, .166f, .130f, 1});
    if (ImGui::BeginTable("memory-summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("now", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("maximum", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableNextColumn();
        metric(tr("Memory now (service + workers)"), memory, ink, compact ? 26.0f : 32.0f);
        ImGui::TableNextColumn();
        metric(tr("Chart maximum"), visible && peak ? number(*peak, tr("MiB")) : "—", sand, 19.0f);
        ImGui::EndTable();
    }
    const float memory_height = minimal_charts ? 52.0f : compact ? 79.0f : 106.0f;
    if (history_hours) history_chart(retained, HistoryMetric::memory, view.connected, memory_height);
    else graph(samples, 0, view.connected, memory_height);
    end_surface();

    ImGui::Separator();
    const float activity_height = compact ? 53.0f : 63.0f;
    const float activity_value_size = compact ? 20.0f : 24.0f;
    if (ImGui::BeginTable("overview-activity", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        metric(tr("Requests"), requests, ink, activity_value_size);
        if (!minimal_charts && history_hours) history_chart(retained, HistoryMetric::requests, view.connected, activity_height);
        else if (!history_hours) ImGui::TextColored(muted, "%s", tr("Current service run"));
        ImGui::TableNextColumn();
        metric(tr(history_hours ? "Search time, mean" : "Search, p50"), duration, ink, activity_value_size);
        if (!minimal_charts && history_hours) history_chart(retained, HistoryMetric::search_time, view.connected, activity_height);
        else if (!minimal_charts) graph(recent, 2, view.connected, activity_height);
        ImGui::EndTable();
    }
}

void render_readiness(const ViewState& view, const Json& projects) {
    using namespace shin::ui;
    const auto availability = project_availability(projects, view.connected);
    const Json* attention = nullptr;
    int priority = 0;
    for (const auto& row : projects) {
        const int rank = project_presentation(row, view.connected).attention_priority();
        if (rank > priority) { attention = &row; priority = rank; }
    }
    ImGui::Separator();
    action_button(tr("Projects"), open_project_list);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (view.connected) {
        ImGui::TextColored(muted, tr("Search available · %zu of %zu"), availability.ready, projects.size());
        if (availability.notices) ImGui::TextColored(muted, tr("Projects with notices · %zu"), availability.notices);
    } else ImGui::TextWrapped("%s", tr("Current project readiness is unknown"));
    if (view.connected && project_connection_problem(view.data))
        ImGui::TextWrapped("%s", tr("Some project connections could not be loaded. Check the service report."));
    if (!view.connected || !attention) return;
    const auto target = attention->value("id", "");
    const auto explanation = diagnostic_overview(*attention, view.connected);
    action_button(text_field(*attention, "name"), 121, 0, !target.empty(), target);
    if (ImGui::GetContentRegionAvail().x > 250) ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x > 45) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(priority == 3 ? red : priority == 2 ? blue : amber, "%s",
            clipped(explanation.title, ImGui::GetContentRegionAvail().x).c_str());
    }
    ImGui::PushFont(nullptr, 12);
    ImGui::TextWrapped("%s", explanation.summary.c_str());
    ImGui::PopFont();
}
