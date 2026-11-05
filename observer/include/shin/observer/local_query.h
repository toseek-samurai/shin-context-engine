#pragma once
#include "local_project.h"

namespace shin::context { struct Profile; struct VerifiedSnapshot; class GenerationAdmission; }

namespace shin::observer {
Json source_coverage_summary(const Json& manifest);
struct LocalReaderQualification {
    const shin::context::VerifiedSnapshot* snapshot = nullptr;
    const shin::context::GenerationAdmission* admission = nullptr;
};
// One serial, process-owned reader. Preparation is admitted by local_project;
// Full readers require qualification; retained readers require the explicit
// approved profile and selected-source verification with partial coverage.
class LocalQuerySession {
public:
    LocalQuerySession(const Config& config, const fs::path& workspace, const fs::path& cache, const Json& status,
                      std::function<void()> verify_authority, const shin::context::Profile* approved_profile = nullptr,
                      LocalReaderQualification qualification = {});
    ~LocalQuerySession();
    LocalQuerySession(const LocalQuerySession&) = delete;
    LocalQuerySession& operator=(const LocalQuerySession&) = delete;
    bool observed_unchanged();
    Json execute(const LocalProjectRequest& request);
    Json description() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
