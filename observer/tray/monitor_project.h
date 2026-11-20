#pragma once

void render_project_analytics(const ViewState& view, const Json& project, const Json& telemetry) {
    const auto scoped = shin::ui::project_request_telemetry(telemetry, project.at("id").get<std::string>());
    const auto history = scoped.find("history");
    const bool retained = history != scoped.end() && history->value("status", "") == "ready";
    const auto summary = shin::ui::request_timing(scoped, retained, 0);
    const bool known = summary.available && !summary.invalid && (summary.requests || !summary.truncated);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const auto log = shin::ui::tr("Project log");
    heading(shin::ui::tr("Project search"));
    if (ImGui::GetContentRegionAvail().x > 250)
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 4, right - ImGui::CalcTextSize(log).x - 20));
    action_button(log, project_journal);
    ImGui::PushFont(nullptr, 13);
    ImGui::TextColored(muted, "%s", shin::ui::tr(retained ? "Searches · retained history" : "Searches · this run"));
    if (!view.connected) ImGui::TextColored(amber, "%s", shin::ui::tr("No fresh service response"));
    if (summary.truncated || summary.invalid) ImGui::TextWrapped("%s", shin::ui::tr("Only part of the request history is available."));
    ImGui::PopFont();
    if (ImGui::BeginTable("project-metrics", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(raised));
        metric(shin::ui::tr("Searches"), known ? std::to_string(summary.requests) : "—", ink, 20);
        ImGui::TableNextColumn(); metric(shin::ui::tr("Search errors"), known ? std::to_string(summary.errors) : "—", summary.errors ? amber : ink, 20);
        ImGui::TableNextColumn(); metric(shin::ui::tr("Mean response"), summary.service.value() ? number(*summary.service.value(), shin::ui::tr("ms")) : "—", ink, 20);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    heading(shin::ui::tr("Recent search response time"));
    const auto recent = scoped.value("request_metrics", Json::object()).value("recent", Json::array());
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = layout_height() < 565 ? 49.0f : 68.0f;
    auto* draw = ImGui::GetWindowDrawList();
    double maximum = 1, end = 0, first = 0;
    std::size_t observations = 0;
    for (const auto& row : recent) {
        if (row.value("method", "") != "query" || !row.value("elapsed_ms", Json()).is_number()) continue;
        shin::ui::TimingMean sample; sample.add(row.value("service_duration_ms", Json()));
        if (!sample.value()) continue;
        const auto at = row.at("elapsed_ms").get<double>();
        if (!observations++) first = at;
        end = at; maximum = std::max(maximum, *sample.value() * 1.15);
    }
    for (int line = 0; line < 3; ++line)
        draw->AddLineH(origin.x, origin.x + width, origin.y + line * height / 2, ImGui::GetColorU32(rule));
    ImVec2 previous{};
    double previous_at = 0;
    bool connected_point = false;
    const double span = std::max(60000.0, end - first);
    for (const auto& row : recent) {
        if (row.value("method", "") != "query") continue;
        shin::ui::TimingMean sample; sample.add(row.value("service_duration_ms", Json()));
        if (!sample.value() || !row.value("elapsed_ms", Json()).is_number()) { connected_point = false; continue; }
        const double at = row.at("elapsed_ms").get<double>();
        const ImVec2 point{origin.x + 2 + static_cast<float>((at - end + span) / span) * std::max(1.0f, width - 4),
            origin.y + height - static_cast<float>(*sample.value() / maximum) * (height - 16)};
        const auto color = ImGui::GetColorU32(view.connected ? blue : muted);
        if (connected_point && at >= previous_at && at - previous_at <= 60000) {
            draw->AddQuadFilled(previous, point, {point.x, origin.y + height}, {previous.x, origin.y + height}, alpha_color(blue, .08f));
            draw->AddLine(previous, point, color, 1.8f);
        }
        draw->AddCircleFilled(point, 2.5f, row.value("outcome", "") == "response-error" ? ImGui::GetColorU32(amber) : color);
        previous = point; previous_at = at; connected_point = true;
    }
    if (observations) draw->AddText(origin, ImGui::GetColorU32(muted), number(maximum, shin::ui::tr("ms")).c_str());
    else draw->AddText({origin.x + 4, origin.y + 30}, ImGui::GetColorU32(muted), shin::ui::tr("No search measurements yet"));
    ImGui::Dummy({width, height + 4});
    ImGui::PushFont(nullptr, 13);
    if (observations) ImGui::TextColored(muted, shin::ui::tr("%.0f s · samples: %zu"), (end - first) / 1000, observations);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shin::ui::tr("Recent requests from this run. Gaps have no measurements."));
    ImGui::PopFont();
}

void render_project_settings(const ViewState& view, const std::string& name, const std::string& root) {
    using shin::ui::tr;
    const bool busy = project_management_busy.load() || project_disconnection.busy();
    const bool enabled = view.connected && !diagnostic_root.empty() && !busy;
    if (project_view == ProjectPage::disconnect) {
        const bool disconnecting = project_disconnection.pending_for(diagnostic_project, root);
        page_title(tr("Disconnect this project from Shin?"));
        ImGui::TextUnformatted(name.c_str());
        ImGui::TextWrapped("%s", root.c_str());
        ImGui::TextWrapped("%s", tr("Shin will remove the project from the list and stop indexing the folder. You can connect it again."));
        ImGui::TextWrapped("%s", tr("Cache cleanup is a separate action in Storage."));
        if (const auto* result = project_disconnection.result_for(diagnostic_project, root))
            ImGui::TextWrapped("%s", disconnect_result_text(*result, false).c_str());
        action_button(tr("Cancel"), back_to_project_settings, 0, !disconnecting);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, red);
        action_button(tr(disconnecting ? "Disconnecting project…" : "Disconnect project"), disconnect_project, 0, enabled);
        ImGui::PopStyleColor();
        return;
    }
    action_button(tr("Back"), project_view == ProjectPage::settings ? back_to_project : back_to_project_settings);
    if (project_view == ProjectPage::exclusions) {
        page_title(tr("Indexing exclusions"));
        ImGui::TextUnformatted(name.c_str());
        ImGui::TextWrapped("%s", tr("Shin uses .gitignore rules throughout the project. A root .shinignore can override them."));
        if (!root.empty()) ImGui::TextWrapped("%s", (fs::path(root) / ".shinignore").string().c_str());
        action_button(tr("Rule examples"), open_exclusion_guide);
        action_button(tr("Show project folder"), 38, 0, !diagnostic_root.empty());
        return;
    }

    page_title(tr("Project settings"));
    ImGui::TextUnformatted(name.c_str());
    ImGui::TextWrapped("%s", root.c_str());
    ImGui::Separator();
    std::string configured_agents;
    if (view.agent_access_root == root && view.agent_access) for (const auto& agent : *view.agent_access) {
        if (agent.value("access_state", "") != "configured") continue;
        if (!configured_agents.empty()) configured_agents += " · ";
        configured_agents += shin::ui::text_field(agent, "label", 96);
    }
    option_row(tr("Agent access"), 116, configured_agents, enabled);
    option_row(tr("Indexing exclusions"), project_exclusions, ".gitignore · .shinignore");
    option_row(tr("Storage"), 83, tr("Local data and caches"));
    const auto width = ImGui::GetContentRegionAvail().x;
    action_button(tr("Show project folder"), 38, 0, !diagnostic_root.empty());
    if (width >= 410) ImGui::SameLine();
    action_button(tr("Technical report"), 39);
    ImGui::Separator();
    if (!view.connected) ImGui::TextWrapped("%s", tr("Disconnection is available when the service is connected."));
    ImGui::PushStyleColor(ImGuiCol_Text, red);
    action_button(tr("Disconnect project…"), confirm_disconnect, 0, enabled);
    ImGui::PopStyleColor();
}

void render_agent_access(const ViewState& view, const std::string& root) {
    using namespace shin::ui;
    heading(tr("Agent access"));
    ImGui::PushFont(nullptr, 12);
    if (!view.connected || root.empty() || (view.agent_access_root == root && !view.agent_access)) {
        ImGui::TextColored(muted, "%s", tr("Agent access could not be checked"));
    } else if (view.agent_access_root != root) {
        ImGui::TextColored(muted, "%s", tr("Checking agent access…"));
    } else if (view.agent_access->empty()) {
        ImGui::TextColored(muted, "%s", tr("No supported agents detected"));
    } else if (ImGui::BeginTable("agent-access", 2, ImGuiTableFlags_SizingStretchSame)) {
        const auto& agents = *view.agent_access;
        for (std::size_t i = 0; i < std::min<std::size_t>(agents.size(), 5); ++i) {
            const auto& agent = agents.at(i);
            const auto state = agent.value("access_state", "review-required");
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", text_field(agent, "label", 96).c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, state == "configured" ? green : state == "not-configured" ? muted : amber);
            ImGui::TextWrapped("%s", tr(state == "configured" ? "Access configured" : client_access_label(state)));
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
        if (agents.size() > 5) ImGui::TextColored(muted, "%s", tr("More connections in project settings"));
    }
    ImGui::PopFont();
}

void render_project_diagnostics(const ViewState& view, const Json& project, const Json& telemetry, const Json& history) {
    using namespace shin::ui;
    diagnostic_project = text_field(project, "id", 96);
    const auto root = project.contains("root") && project.at("root").is_string() ? project.at("root").get<std::string>() : std::string{};
    active_profile_review = view.profile_review;
    if (!profile_review_open) profile_review_root = root;
    if (root.size() <= 4096 && !root.empty() && fs::path(root).is_absolute() &&
        std::none_of(root.begin(), root.end(), [](unsigned char c) { return c < 32 || c == 127; })) diagnostic_root = root;
    diagnostic_copy = diagnostic_summary(project, view.data, view.connected, view.error,
        platform_version());
    const auto presentation = project_presentation(project, view.connected);
    const auto state = presentation.state;
    const auto overview = diagnostic_overview(project, presentation);
    const auto recovery = recovery_options(project, view.connected);
    const bool partial_coverage = presentation.partial;
    const bool advisory = presentation.searchable() && presentation.advisory();
    const auto name = text_field(project, "name");
    diagnostic_name = name;
    const auto prior_code = historical_code(project);
    auto current_code = view.connected ? error_code(project, "error_code") : error_code(Json{{"code", view.error}}, "code");
    if (current_code.empty() && active_issue(project)) current_code = prior_code;
    const auto explanation = current_code.empty() || (view.connected && active_issue(project) &&
        (current_code == "refresh-validation-failed" || current_code == "project-check-failed"))
        ? Explanation{overview.summary, overview.next} : explain(current_code);
    if (profile_review_open) {
        render_profile_review(view, root, name);
        return;
    }
    diagnostic_report_text = name + "\n\n" + overview.title + "\n" + explanation.cause +
        shin::ui::tr("\n\nNext step\n") + (recovery.next.empty() ? explanation.next : recovery.next) + shin::ui::tr("\n\nHistory\n") +
        diagnostic_history_status(project, view.connected) +
        (prior_code.empty() ? "" : shin::ui::tr("\nLast code: ") + prior_code + "\n" + explain(prior_code).cause) +
        shin::ui::tr("\n\nTechnical details of the last observation\n") + diagnostic_copy;
    const auto* retained = retained_project(project, history);
    const double elapsed = view.received == Clock::time_point{} ? 0 :
        std::chrono::duration<double, std::milli>(Clock::now() - view.received).count();
    const auto age = age_text(age_ms(project, "last_success_age_ms", elapsed));
    const float width = ImGui::GetContentRegionAvail().x;
    if (project_view == ProjectPage::settings || project_view == ProjectPage::exclusions || project_view == ProjectPage::disconnect) {
        render_project_settings(view, name, root);
        return;
    }
    const auto text = [&](const std::string& value, ImVec4 color, int id) {
        if (value.empty()) return;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", value.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemVisible()) {
            const auto start = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
            const auto clip = ImGui::GetWindowDrawList()->GetClipRectMax();
            ui_actions.push_back({value, selected, start, {std::min(end.x,clip.x)-start.x, std::min(end.y,clip.y)-start.y}, id, true, true});
        }
    };

    if (diagnostic_details_open) {
        action_button(tr("Back"), 120);
        text(tr("Search check details"), amber, 124);
        const auto raw = presentation.search_warning ? project.at("quality_warning").value("diagnostic", Json()) : project.value("validation_diagnostic", Json());
        const auto diagnostic = shin::observer::bounded_validation_diagnostic(raw);
        if (!diagnostic.is_object()) text(tr("Details unavailable"), muted, 127);
        for (const auto& gate : diagnostic.is_object() ? diagnostic.value("gates", Json::array()) : Json::array()) {
            for (const auto& item : gate.value("cases", Json::array())) {
                const auto id = shin::observer::validation_identifier(item.value("id", Json()));
                if (!id.empty()) text(std::string(tr("Search check: ")) + id, ink, 125);
                for (const auto& path : item.value("expected_paths", Json::array()))
                    text(std::string(tr("Expected document: ")) + shin::observer::validation_expected_path(path), ink, 126);
            }
        }
        text(tr("Check the expected file directly. Report a reproducible miss with the original query."), muted, 127);
        return;
    }

    action_button(tr("Back"), diagnostic_history_first ? 35 : 32);
    const auto settings_label = tr("Project settings");
    const float settings_width = ImGui::CalcTextSize(settings_label).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 4, width - settings_width));
    action_button(settings_label, project_settings);
    const bool searchable = presentation.searchable();
    const auto state_label = searchable && !partial_coverage ? std::string(tr("Search available")) : overview.title;
    const auto tint = searchable ? green : project_color(state, project);
    page_title(name.c_str());
    const float state_width = ImGui::CalcTextSize(state_label.c_str()).x;
    if (width >= 360 && ImGui::CalcTextSize(name.c_str()).x * 1.55f + state_width + 24 < width)
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, width - state_width));
    text(state_label, tint, 54);
    const auto metadata = [&] {
        ImGui::PushFont(nullptr, 12);
        if (!root.empty()) text(root, muted, 58);
        if (view.connected) {
            const auto check = std::string(tr("Last source check: ")) + age;
            if (!root.empty() && ImGui::CalcTextSize(root.c_str()).x + ImGui::CalcTextSize(check.c_str()).x + 14 < width)
                ImGui::SameLine();
            text(check, muted, 59);
        }
        ImGui::PopFont();
    };
    const bool low = layout_height() <= 380;
    if (!low) metadata();

    if (diagnostic_history_first) {
        ImGui::Separator();
        text(tr("Project history"), ink, 54);
        text(diagnostic_history_status(project, view.connected), ink, 55);
        text(tr("Historical counters do not imply a current error."), muted, 56);
        if (retained) text(std::string(tr("Retained requests: ")) + std::to_string(count(*retained, "requests")), ink, 57);
        action_button(tr("Copy report"), 36);
        ImGui::SameLine();
        action_button(tr("Technical report"), 39);
        return;
    }

    if (!searchable || advisory || partial_coverage) {
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        ImGui::Indent(8);
        const auto repair = [&] {
            if (recovery.action == RecoveryAction::profile) primary_button(tr("Review changes"), 70);
            else if (recovery.action == RecoveryAction::search) action_button(tr("Review search checks"), 120);
            else if (recovery.action == RecoveryAction::storage) action_button(tr("Storage and cleanup…"), 83);
            else if (recovery.action == RecoveryAction::folder) action_button(tr("Show project folder"), 38, 0, !diagnostic_root.empty());
            else if (recovery.action == RecoveryAction::report) action_button(tr("Copy report"), 36, 0, !diagnostic_copy.empty());
        };
        if (layout_height() <= 280) repair();
        text(explanation.cause, state == ProjectState::blocked ? red : advisory || partial_coverage ? amber : ink, 55);
        if (layout_height() > 280) repair();
        int notice_id = 139;
        for (const auto& summary : additional_advisory_summaries(presentation, overview))
            text(summary, amber, notice_id++);
        const auto next = recovery.next.empty() ? explanation.next : recovery.next;
        if (!next.empty()) text(next, muted, 57);
        if (view.connected && partial_coverage) {
            const auto& coverage = project.at("source_coverage");
            const auto files = coverage.value("files", Json::array());
            if (files.is_array() && !files.empty()) {
                text(tr("Files with hidden lines"), muted, 128);
                for (std::size_t i = 0; i < std::min<std::size_t>(8, files.size()); ++i) {
                    const auto scanner_rule = shin::context::source_redaction_rule(text_field(files[i], "reason", 64));
                    text(text_field(files[i], "path", 512) + " · " + std::string(scanner_rule), ink, 129 + static_cast<int>(i));
                }
                if (count(coverage, "omitted_files"))
                    text(std::string(tr("Other files with hidden lines: ")) + std::to_string(count(coverage, "omitted_files")), muted, 138);
            }
        }
        ImGui::Unindent(8);
        ImGui::EndGroup();
        ImGui::GetWindowDrawList()->AddLineV(origin.x + 1, origin.y, ImGui::GetItemRectMax().y,
            ImGui::GetColorU32(state == ProjectState::blocked ? red : advisory ? amber : rule), 2);
    }
    if (low) metadata();
    ImGui::Separator();
    render_project_analytics(view, project, telemetry);
    ImGui::Separator();
    render_agent_access(view, root);
}
