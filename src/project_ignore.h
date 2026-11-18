#pragma once

#include "context_types.h"

namespace shin::context {

class IgnoreRules;

class ProjectExclusions {
public:
    struct Decision {
        bool git_ignored = false;
        int shin_rule = -1;
        bool excluded = false;
    };
    explicit ProjectExclusions(const fs::path& root);
    void enter_directory(const std::string& path);
    Decision decide(const std::string& path, bool directory, Decision parent) const;
    bool may_include_below(const std::string& directory, Decision decision) const;
    ProjectIgnoreBinding binding() const;
private:
    fs::path root_;
    std::shared_ptr<const IgnoreRules> shin_;
    std::string shin_digest_;
    std::map<std::string, std::string> git_digests_;
    std::map<std::string, std::shared_ptr<const IgnoreRules>, std::less<>> git_rules_;
};

} // namespace shin::context
