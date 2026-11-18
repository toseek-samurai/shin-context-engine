#pragma once

#include "context_types.h"

namespace shin::context {

std::string read_project_text(const fs::path& root, const std::string& path, std::size_t maximum_bytes);
ProjectIgnoreBinding published_project_exclusions(const Profile& profile, const Json& manifest);
void require_project_exclusions(const fs::path& root, const ProjectIgnoreBinding& expected);

bool observed_link_exists(bool automatic, const fs::path& root, const std::string& relative);

bool automatic_project_event_relevant(const fs::path& root, const std::string& relative, bool directory);

struct ProjectFileCounts {
    std::size_t excluded_entries = 0, binary_files = 0, oversized_files = 0;
};

class ProjectExclusions;

struct ProjectFiles {
    std::vector<std::string> documents;
    SourceFactPathResult sources;
    ProjectFileCounts omissions;
    ProjectIgnoreBinding exclusions;

    static ProjectFiles scan(const Profile& profile, const fs::path& root);
    static bool is_document(const std::string& path);
    bool contains(const std::string& path) const;
    std::string link_status(const std::string& path) const;
    Json summary() const;
private:
    std::shared_ptr<const ProjectExclusions> exclusions_;
    fs::path root_;
};

} // namespace shin::context
