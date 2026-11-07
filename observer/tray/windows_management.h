#pragma once
// Sand management pages reuse the native CLI's authority and revision contracts.
struct WindowsManagement {
    enum class Page { setup, storage, project, login } page = Page::setup;
    struct Agent { Json target; bool selected = false; Json review; };
    fs::path home;
    std::string root, operation, message;
    std::vector<Agent> agents;
    Json storage = Json::object();
    Json report = Json::object();
    std::future<Json> pending;
    bool busy() const { return pending.valid(); }
    fs::path cli() const { return current_executable_path().parent_path()/"shin-ce.exe"; }
    std::vector<std::string> arguments(const std::string& command) const { return {command,"--home",home.string()}; }
    void run(const std::string& name, std::vector<std::string> args) {
        if (busy() || test_mode) return;
        operation = name; message.clear(); project_management_busy=true;
        const auto executable = cli();
        pending = std::async(std::launch::async,[executable,args=std::move(args)] {
            try { return windows_command(executable,args); }
            catch (const Error& error) { return Json{{"ok",false},{"error",{{"code",error.code},{"message",error.what()}}}}; }
            catch (...) { return Json{{"ok",false},{"error",{{"code","monitor-command-failed"}}}}; }
        });
    }
    void discover() { run("targets",arguments("client-targets")); }
    void inventory() {
        storage = Json::object();
        auto args=arguments("storage-status");
        if (!root.empty()) { args.push_back("--root"); args.push_back(root); }
        run("storage",std::move(args));
    }
    void poll() {
        if (!busy() || pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
        const auto result=pending.get();
        report=result;
        const bool service_failed=result.contains("service") && !result.at("service").value("ok",false);
        if (!result.value("ok",false) || service_failed) {
            const auto& failure=service_failed ? result.at("service") : result;
            message=shin::ui::tr("Operation was not confirmed. Review the report before retrying.");
            if (failure.contains("error")) message+="\n"+failure.at("error").value("code","");
            if (failure.contains("error") && failure.at("error").value("code", "") == "snapshot-memory-budget-exceeded") {
                message=shin::ui::tr("The generated project index exceeds a load budget. Review the index size and project limits before retrying.");
                message+="\nsnapshot-memory-budget-exceeded\n"+failure.at("error").value("message", std::string{}).substr(0,1024);
            }
            if(operation=="connect" || operation=="preview") for(auto& agent:agents) agent.review=Json();
            storage=Json::object();
        } else if (operation=="targets") {
            agents.clear();
            for (const auto& connection:result.at("targets")) agents.push_back({connection,false,Json()});
        } else if (operation=="add") { message=shin::ui::tr("Project added. Select the agents that should have access."); discover(); }
        else if (operation=="storage") storage=result;
        else if (operation=="preview") {
            for (std::size_t i=0;i<agents.size();++i) agents[i].review=result.at("reviews").at(i);
            message=shin::ui::tr("Review the selected configuration paths, then confirm connection.");
        } else if (operation=="connect") {
            for (auto& agent:agents) agent.review=Json();
            message=shin::ui::tr("Agents connected. Restart their MCP sessions to load the settings.");
        } else if (operation=="detach") {
            message=shin::ui::tr("The project was removed from observation. Sources, indexes, history and Shin settings were preserved.");
            page=Page::storage; storage=Json::object();
        } else { message=shin::ui::tr("Operation completed."); storage=Json::object(); }
        project_management_busy=busy();
    }
    void connections(bool apply) {
        if (busy() || root.empty() || test_mode) return;
        const auto rows=agents; const auto executable=cli(); const auto data_home=home.string(); const auto project=root;
        operation=apply?"connect":"preview"; message.clear(); project_management_busy=true;
        pending=std::async(std::launch::async,[rows,executable,data_home,project,apply] {
            Json reviews=Json::array();
            try {
                for (const auto& agent:rows) {
                    Json review;
                    if (agent.selected) {
                        const auto config=agent.target.value("config","");
                        std::vector<std::string> args{"client-connect","--home",data_home,"--root",project,
                            "--client",agent.target.at("client"),"--config",config};
                        if (apply) { args.push_back("--revision"); args.push_back(agent.review.at("revision")); }
                        review=windows_command(executable,args);
                        if (!review.value("ok",false)) {
                            // Earlier successful writes remain recorded for review.
                            review["completed_connections"]=reviews; return review;
                        }
                    }
                    reviews.push_back(std::move(review));
                }
                return Json{{"ok",true},{"reviews",reviews}};
            } catch(const Error& error) { return Json{{"ok",false},{"error",{{"code",error.code}}}}; }
            catch(...) { return Json{{"ok",false},{"error",{{"code","client-config-invalid"}}}}; }
        });
    }
};
WindowsManagement management;

std::optional<fs::path> management_picker(bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    checked(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)),"Cannot open picker.");
    dialog->SetOptions(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|(folder?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST));
    modal=true; const auto result=dialog->Show(panel); modal=false;
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    checked(result,"Cannot select path."); ComPtr<IShellItem> item; checked(dialog->GetResult(&item),"Cannot read path.");
    PWSTR raw=nullptr; checked(item->GetDisplayName(SIGDN_FILESYSPATH,&raw),"Cannot read path.");
    const fs::path path(raw); CoTaskMemFree(raw); return path;
}
void open_management(int code) {
    if (management.busy()) { management_page=true; show_panel(); return; }
    management_page=true; updates_page=false; management.message.clear(); project_management_busy=false;
    if(code==83) { management.page=WindowsManagement::Page::storage; management.root.clear(); management.inventory(); }
    else if(code==84) { management.page=WindowsManagement::Page::project; management.root=diagnostic_root; }
    else if(code==112) management.page=WindowsManagement::Page::login;
    else {
        management.page=WindowsManagement::Page::setup;
        if(code==116) management.root=diagnostic_root;
        management.discover();
    }
    show_panel();
}
bool management_button(const char* label,int id,bool enabled=true) {
    ImGui::PushID(id); ImGui::BeginDisabled(!enabled);
    const bool pressed=ImGui::Button(label,{ImGui::GetContentRegionAvail().x,28});
    if(ImGui::IsItemVisible()) {
        const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax(),clip=ImGui::GetWindowDrawList()->GetClipRectMax();
        ui_actions.push_back({label,"",lo,{std::min(hi.x,clip.x)-lo.x,std::min(hi.y,clip.y)-lo.y},id,enabled,false,ImGui::GetItemID()});
    }
    ImGui::EndDisabled(); ImGui::PopID(); return pressed;
}
void render_management() {
    using shin::ui::tr;
    management.poll(); ui_actions.clear();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Shin management",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(nullptr,14);
    const auto page=management.page;
    const char* title=page==WindowsManagement::Page::setup?tr("Connect projects and agents…"):
        page==WindowsManagement::Page::storage?tr("Storage and cleanup…"):page==WindowsManagement::Page::project?tr("Project actions"):tr("Login settings…");
    ImGui::PushFont(nullptr,20); ImGui::TextWrapped("%s",title); ImGui::PopFont(); ImGui::Separator();
    const float footer=ImGui::GetWindowHeight()-42;
    ImGui::BeginChild("management-content",{0,std::max(1.0f,footer-ImGui::GetCursorPosY()-8)});
    if(!management.root.empty()) { ImGui::TextWrapped("%s",management.root.c_str()); ImGui::Spacing(); }
    const bool idle=!management.busy();
    ImGui::BeginDisabled(!idle);
    if(page==WindowsManagement::Page::setup) {
        if(management_button(tr("Choose project folder…"),301)) {
            if(const auto folder=management_picker(true)) {
                management.root=folder->string(); management.agents.clear();
                auto args=management.arguments("project-add"); args.insert(args.end(),{"--root",management.root});
                management.run("add",std::move(args));
            }
        }
        ImGui::Spacing(); ImGui::TextWrapped("%s",tr("Only checked agents are changed. Existing settings are preserved and backed up."));
        bool selected_agent=false, reviewed=true;
        for(std::size_t i=0;i<management.agents.size();++i) {
            auto& agent=management.agents[i]; ImGui::PushID(static_cast<int>(i));
            const auto label=agent.target.value("label",agent.target.value("client",""));
            if(ImGui::Checkbox(label.c_str(),&agent.selected)) for(auto& item:management.agents) item.review=Json();
            ImGui::TextColored(muted,"%s",agent.target.value("detected",false)?tr("Detected"):tr("Not detected"));
            ImGui::TextWrapped("%s",agent.target.value("config","").c_str());
            if(ImGui::SmallButton(tr("Choose settings file…"))) if(const auto path=management_picker(false)) {
                agent.target["config"]=path->string(); for(auto& item:management.agents) item.review=Json();
            }
            if(agent.selected && agent.review.is_object() && agent.review.value("ok",false))
                ImGui::TextColored(sand,"%s",agent.review.value("changed",false)?tr("Ready to connect"):tr("Already connected"));
            if(agent.selected) { selected_agent=true; reviewed=reviewed&&agent.review.is_object()&&agent.review.value("ok",false); }
            ImGui::Separator(); ImGui::PopID();
        }
        if(management_button(tr("Review connections"),302,idle&&selected_agent&&!management.root.empty())) management.connections(false);
        if(management_button(tr("Connect selected agents"),303,idle&&selected_agent&&reviewed&&!management.root.empty())) management.connections(true);
        if(management_button(tr("Check project"),304,idle&&!management.root.empty())) {
            auto args=management.arguments("status"); args.insert(args.end(),{"--root",management.root}); management.run("status",std::move(args));
        }
    } else if(page==WindowsManagement::Page::storage) {
        const auto& data=management.storage;
        if(data.value("ok",false)) {
            for(const auto& [key,label]:std::array<std::pair<const char*,const char*>,4>{{{"used_bytes",tr("Used")},{"quota_bytes",tr("Budget")},{"reclaimable_bytes",tr("Reclaimable")},{"reserve_free_bytes",tr("Free-space reserve")}}})
                ImGui::Text("%s: %.1f MiB",label,static_cast<double>(data.value(key,0ULL))/(1024*1024));
            bool automatic=data.value("auto_clean",false);
            if(ImGui::Checkbox(tr("Automatically clean disconnected project caches"),&automatic)) {
                auto args=management.arguments("storage-policy"); args.insert(args.end(),{"--auto-clean",automatic?"on":"off"}); management.run("policy",std::move(args));
            }
            for(const auto& row:data.value("projects",Json::array()))
                ImGui::TextWrapped("%s · %.1f MiB",row.value("name",row.value("root","")).c_str(),static_cast<double>(row.value("used_bytes",0ULL))/(1024*1024));
            if(management_button(tr("Clean caches"),305,idle&&data.value("reclaimable_bytes",0ULL)>0&&data.contains("revision"))) {
                modal=true; const auto answer=MessageBoxW(panel,wide(tr("Sources, indexes, history and settings are preserved. Clean the reviewed disconnected caches?")).c_str(),L"Shin",MB_YESNO|MB_ICONQUESTION); modal=false;
                if(answer==IDYES) {
                    auto args=management.arguments("storage-clean"); args.insert(args.end(),{"--revision",data.at("revision")});
                    if(!management.root.empty()) args.insert(args.end(),{"--root",management.root}); management.run("clean",std::move(args));
                }
            }
        }
        if(management_button(tr("Refresh"),306,idle)) management.inventory();
    } else if(page==WindowsManagement::Page::project) {
        if(management_button(tr("Connect agent…"),307,idle&&!management.root.empty())) open_management(116);
        if(management_button(tr("Disconnect"),308,idle&&!management.root.empty())) {
            modal=true; const auto answer=MessageBoxW(panel,wide(tr("Disconnect this project? Sources, indexes, history and settings are preserved.")).c_str(),L"Shin",MB_YESNO|MB_ICONQUESTION); modal=false;
            if(answer==IDYES) { auto args=management.arguments("detach"); args.insert(args.end(),{"--root",management.root}); management.run("detach",std::move(args)); }
        }
        if(management_button(tr("Storage and cleanup…"),309,idle)) { management.page=WindowsManagement::Page::storage; management.inventory(); }
    } else {
        ImGui::TextWrapped("%s",tr("Monitor and the background service have separate startup settings. The MSI manages the service; Windows Startup Apps manages Monitor."));
        if(management_button(tr("Open Windows Startup Apps"),310)) ShellExecuteW(panel,L"open",L"ms-settings:startupapps",nullptr,nullptr,SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();
    if(!idle) ImGui::TextColored(sand,"%s",tr("Operation in progress…"));
    if(!management.message.empty()) ImGui::TextWrapped("%s",management.message.c_str());
    if(!management.report.empty() && management_button(tr("Copy diagnostic report"),312)) platform_copy(management.report.dump(2));
    ImGui::EndChild(); ImGui::SetCursorPosY(footer);
    if(management_button(tr("Back to Monitor"),311)) { management_page=false; show_panel(); }
    ImGui::PopFont();
    ImGui::End();
}
