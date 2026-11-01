#include "shin/observer/windows_setup.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/local_service.h"
#include "local_scm_windows.h"
#include "context_internal.h"
#include <set>

namespace shin::observer {
namespace {
void check_ready(const fs::path& home, Json& result) {
    const auto projects=registered_local_projects(home);
    std::set<std::string> current,recorded;
    for(const auto& project:projects) current.insert(project.root.string());
    for(const auto& row:result.at("projects")) recorded.insert(row.at("root").get<std::string>());
    if(current!=recorded) throw Error("registry-binding-changed","Project enrollment changed during setup.");
    if(projects.empty()) return;
    LocalProjectRequest status;status.home=home;status.command="service-status";
    const auto live=request_local_service(status);
    std::set<std::string> qualified;
    for(const auto& row:live.at("qualified_projects")) qualified.insert(row.at("root").get<std::string>());
    for(auto& row:result["projects"]) if(!qualified.contains(row.at("root").get<std::string>()) ||
        live.at("enrollment_errors").contains(row.at("root").get<std::string>())) {
        row["ok"]=false;row["error_code"]="installation-project-not-ready";result["ok"]=false;
    }
}
}
Json prepare_windows_projects(const fs::path& input_home, const fs::path& executable, bool check_only) {
    scm::Token user; user.desktop();
    const auto home=fs::absolute(input_home).lexically_normal();
    if(check_only) {
        StateStore operation(home/"windows-setup",false);
        auto result=operation.load();
        if(!result || operation.writer_present() || result->value("phase","")!="completed" ||
            result->value("version","")!=SHIN_OBSERVER_VERSION || result->value("core_version","")!=SHIN_VERSION)
            throw Error("installation-preparation-unavailable","Project preparation has not completed for this installation.");
        check_ready(home,*result);
        if(!result->value("ok",false)) {
            (*result)["phase"]="review-required";
            (*result)["error"]={{"code","installation-project-not-ready"},{"message","Current project readiness no longer confirms setup completion."}};
        }
        return *result;
    }
    ensure_private_directory(home);
    ensure_local_registry(home);
    ensure_private_directory(home/"windows-setup");
    StateStore operation(home/"windows-setup",true);operation.recover_pending();
    const auto projects=registered_local_projects(home);
    Json result{{"schema_version","shin.ce-response.v1"},{"tool","shin-ce"},
        {"command","prepare-projects"},{"version",SHIN_OBSERVER_VERSION},{"core_version",SHIN_VERSION},
        {"ok",true},{"phase","preparing"},{"projects",Json::array()},{"readiness_assessed",false}};
    operation.save(result);
    for(auto project:projects) {
        Json row{{"root",project.root.string()},{"ok",false}};
        try {
            const auto workspace=home/"projects"/digest(project.root.string());
            const auto lifecycle=require_local_project_active(workspace);
            project.maintenance_lifecycle_revision=lifecycle.revision;
            project.command="init";
            // Existing native init retains root/workspace identities, route,
            // profile approval, marker approval and publication ownership.
            const auto prepared=initialize_local_service(project,executable);
            row["ok"]=prepared.value("ok",false) && prepared.value("service_enrolled",false);
            row["rebuilt"]=prepared.value("rebuilt",false);
            if(!row.at("ok").get<bool>()) {
                const auto service=prepared.value("service",Json::object());
                const auto error=prepared.value("error",service.value("error",Json::object()));
                row["error_code"]=error.value("code","installation-project-not-ready");
            }
        } catch(const Error& error) {row["error_code"]=error.code;}
          catch(const shin::context::CliError& error) {row["error_code"]=error.code();}
          catch(...) {row["error_code"]="installation-project-not-ready";}
        if(!row.at("ok").get<bool>()) result["ok"]=false;
        result["projects"].push_back(std::move(row));operation.save(result);
    }
    // A detach/enrollment during the operation invalidates completion, rather
    // than silently widening the list or re-enrolling the removed project.
    std::set<fs::path> original,current;
    for(const auto& project:projects) original.insert(project.root);
    for(const auto& project:registered_local_projects(home)) current.insert(project.root);
    if(original!=current) {
        result["ok"]=false;result["error"]={{"code","registry-binding-changed"},{"message","Project enrollment changed during setup. Review the current registry."}};
    }
    if(result.at("ok")==true) try {check_ready(home,result);} catch(...) {result["ok"]=false;}
    result["readiness_assessed"]=true;
    result["phase"]=result.at("ok")==true?"completed":"review-required";
    if(result.at("ok")==false && !result.contains("error"))
        result["error"]={{"code","installation-project-not-ready"},{"message","Some enrolled projects require review. Their policy and previous data are preserved."}};
    operation.save(result);return result;
}
}
