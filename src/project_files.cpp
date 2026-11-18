#include "project_files.h"
#include "context_internal.h"
#include "execution_io.h"
#include <simdjson/simdjson.h>
#include "project_ignore.h"

namespace shin::context {
namespace {

bool text_bytes(std::string_view bytes) {
    return simdjson::validate_utf8(bytes.data(), bytes.size()) &&
        std::none_of(bytes.begin(), bytes.end(), [](unsigned char ch) {
            return ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f';
        });
}

bool safe_project_path(const std::string& relative) {
    return profile_relative_path_valid(relative) && simdjson::validate_utf8(relative.data(), relative.size()) &&
        std::none_of(relative.begin(), relative.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool skip_directory(const std::string& name) {
    static const std::set<std::string> configuration_dirs = {
        ".github", ".gitlab", ".circleci", ".config", ".changeset", ".storybook", ".devcontainer", ".mvn"
    };
    if (name.starts_with('.')) return !configuration_dirs.contains(name);
    // Many projects keep their actual scripts in bin/. Classify the files.
    return name != "bin" && (should_skip_source_fact_dir(name) || name == "__pycache__" ||
        name == "cmakefiles" || name == "vcpkg_installed" || name == "deriveddata" ||
        name.ends_with(".app") || name.ends_with(".framework") || name.ends_with(".dsym"));
}

bool retool_layout(const std::string& path) {
    return path.starts_with("apps/") && (path.ends_with("/.positions") || path.ends_with("/.positions.json"));
}

bool managed_control_path(const std::string& path) {
    return shin_internal_control_path(path) || path == "docs/kb/generated" || path == "docs/machine/shin-profiles" ||
           path == "docs/machine/shin-project-adoption-v1.json" || path == "docs/machine/shin-context-engine-adoption-v1.json" ||
           path == "docs/shin-context-engine-adoption.json" || path == "docs/architecture/shin-context-engine-adoption.json";
}

bool sensitive_filename(const std::string& lower) {
    return lower.starts_with('.') || lower.starts_with("id_rsa") || lower.starts_with("id_ed25519") ||
        lower.starts_with("id_ecdsa") || lower == "credentials" || lower == "credentials.json" ||
        lower == "kubeconfig" || lower == "ntuser.dat";
}

bool never_index_file(const std::string& lower) {
    if (sensitive_filename(lower)) return true;
    static const std::set<std::string> opaque_types = {
        ".7z", ".a", ".apk", ".avi", ".bin", ".bmp", ".bz2", ".class", ".db", ".dll",
        ".dmg", ".dylib", ".ear", ".exe", ".gif", ".gz", ".icns", ".ico", ".jar",
        ".jpeg", ".jpg", ".jks", ".key", ".keystore", ".mov", ".mp3", ".mp4", ".msi",
        ".o", ".obj", ".p12", ".pdf", ".pem", ".pfx", ".png", ".pyc", ".so", ".sqlite",
        ".sqlite3", ".tar", ".tgz", ".tif", ".ttf", ".war", ".wasm", ".webp", ".woff",
        ".woff2", ".xz", ".zip"
    };
    return opaque_types.contains(extension_of(lower));
}

bool regular_text(const fs::path& root, const std::string& path, const FileMetadata& metadata, std::size_t limit) {
    // Repeated freshness checks still enumerate the tree. Reuse only the format
    // classification of an unchanged file, never its contents or membership.
    struct Entry { FileMetadata file; bool text; };
    thread_local std::unordered_map<std::string, Entry> formats;
    const auto key = (root / path).generic_string();
    const auto found = formats.find(key);
    if (found != formats.end() && found->second.file == metadata) return found->second.text;
    const auto bytes = read_bounded_source_file(root, path, limit);
    if (source_file_metadata(root / path, "classify-project-file") != metadata)
        throw_source_changed_during_verification("classify-project-file");
    const bool text = text_bytes(bytes);
    if (formats.size() >= 65536) formats.clear();
    formats.insert_or_assign(key, Entry{metadata, text});
    return text;
}
} // namespace

std::string read_project_text(const fs::path& root, const std::string& path, std::size_t maximum_bytes) {
    auto content = read_bounded_source_file(root, path, maximum_bytes);
    if (!text_bytes(content)) throw_source_changed_during_verification("read-project-text");
    return content;
}

ProjectIgnoreBinding published_project_exclusions(const Profile& profile, const Json& manifest) {
    const auto* files = find_json_field(manifest, "project_files");
    if (!files && !profile.automatic_project_files) return {};
    if (!files || !profile.automatic_project_files)
        throw_index_error("project-exclusions-changed", "Publication is missing the active project selection binding.");
    return ProjectIgnoreBinding::from_summary(*files);
}

void require_project_exclusions(const fs::path& root, const ProjectIgnoreBinding& expected) {
    if (!expected.current(root))
        throw_index_error("project-exclusions-changed", "The selected project exclusions changed. Refresh the index.");
}

bool ProjectFiles::is_document(const std::string& path) {
    const auto extension = extension_of(path);
    if (extension == ".md" || extension == ".mdc" || extension == ".markdown" ||
        extension == ".adoc" || extension == ".rst") return true;
    return path.starts_with("docs/") && (extension == ".json" || extension == ".jsonl" ||
        extension == ".yaml" || extension == ".yml");
}

bool ProjectFiles::contains(const std::string& path) const {
    return std::binary_search(sources.paths.begin(), sources.paths.end(), path);
}

std::string ProjectFiles::link_status(const std::string& path) const {
    if (path == "." || contains(path)) return "ok";
    const auto relative = fs::path(path).lexically_normal();
    if (!safe_project_path(path) || relative.generic_string() != path) return "outside-scope";
    fs::path parent;
    ProjectExclusions::Decision decision;
    for (const auto& component : relative) {
        parent /= component;
        const auto candidate = parent.generic_string();
        std::error_code ec;
        const bool directory = parent != relative || fs::is_directory(fs::symlink_status(root_ / parent, ec));
        if (managed_control_path(candidate) ||
            (directory && !retool_layout(candidate) && skip_directory(to_lower(component.string())))) return "outside-scope";
        decision = exclusions_->decide(candidate, directory, decision);
        if (decision.excluded && (parent == relative || !exclusions_->may_include_below(candidate, decision))) return "outside-scope";
    }
    if (!retool_layout(path) && sensitive_filename(to_lower(relative.filename().string()))) return "outside-scope";
    // Documentation may refer to an image, binary, or empty directory. Its
    // existence is a dependency; do not read or index the asset's bytes.
    return observed_link_exists(true, root_, path) ? "ok" : "broken";
}

bool observed_link_exists(bool automatic, const fs::path& root, const std::string& relative) {
    if (!automatic) return fs::exists(root / relative);
    if (relative == ".") return true;
    if (!safe_project_path(relative)) return false;
    fs::path path = root;
    fs::file_status status;
    for (const auto& component : fs::path(relative)) {
        path /= component;
        std::error_code ec;
        status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) return false;
        if (ec) throw_source_observation_error("verify-source-metadata", ec);
        if (!fs::exists(status) || fs::is_symlink(status)) return false;
    }
    return fs::is_directory(status) || fs::is_regular_file(status);
}

Json ProjectFiles::summary() const {
    return {{"selection", "automatic-project-text"}, {"exclusion_revision", exclusions.revision},
            {"excluded_entries", omissions.excluded_entries}, {"oversized_files", omissions.oversized_files},
            {"binary_files", omissions.binary_files}};
}

bool automatic_project_event_relevant(const fs::path& root, const std::string& relative, bool directory) {
    if (to_lower(relative) == ".shinignore") return true;
    if (!safe_project_path(relative)) return false;
    const fs::path path(relative);
    const bool git_control = to_lower(path.filename().string()) == ".gitignore";
    fs::path parent;
    for (const auto& component : path) {
        parent /= component;
        const auto candidate = parent.generic_string();
        if (git_control && parent == path) break;
        if (managed_control_path(candidate) || (!retool_layout(candidate) &&
            ((directory || parent != path) ? skip_directory(to_lower(component.string()))
                                           : never_index_file(to_lower(component.string()))))) return false;
    }
    // Use the same decision as the scanner. A rule edit is itself relevant,
    // including when it makes a previously excluded file visible.
    try {
        ProjectExclusions rules(root);
        ProjectExclusions::Decision decision;
        parent.clear();
        for (const auto& component : path) {
            parent /= component;
            if (git_control && parent == path) return !decision.git_ignored;
            const auto candidate = parent.generic_string();
            const bool is_dir = directory || parent != path;
            decision = rules.decide(candidate, is_dir, decision);
            if (decision.excluded && !(is_dir && rules.may_include_below(candidate, decision))) return false;
            if (parent != path && !decision.git_ignored) rules.enter_directory(candidate);
        }
    } catch (const CliError&) { return true; }
    return true;
}

ProjectFiles ProjectFiles::scan(const Profile& profile, const fs::path& root) {
    if (profile.source_fact_roots != std::vector<std::string>{"."} ||
        !profile.document_include_prefixes.empty() || !profile.document_exclude_prefixes.empty() ||
        !profile.secret_scan_exclude_prefixes.empty())
        throw CliError("conflicting-project-selection", "configuration", "Automatic selection requires the project root and its exclusion rules.",
                       1, false, "Use the bundled policy and place any additional excluded paths in .shinignore.");
    if (profile.source_fact_max_file_bytes <= 0 || profile.source_fact_max_files <= 0)
        throw CliError("invalid-project-limits", "configuration", "Automatic indexing requires positive file and byte limits.",
                       1, false, "Check the installed engine policy.");
    ProjectFiles inventory;
    inventory.root_ = root;
    auto rules = std::make_shared<ProjectExclusions>(root);
    inventory.exclusions_ = rules;
    std::vector<ProjectExclusions::Decision> parents(1);
    const auto limit = static_cast<std::size_t>(profile.source_fact_max_file_bytes);
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec) throw_source_observation_error("enumerate-source-root", ec);
    while (it != end) {
        const auto relative = it->path().lexically_relative(root).generic_string();
        const auto lower = to_lower(it->path().filename().string());
        const auto status = it->symlink_status(ec);
        if (ec) throw_source_observation_error("enumerate-source-entry", ec);
        const bool directory = fs::is_directory(status);
        parents.resize(static_cast<std::size_t>(it.depth()) + 1);
        if (fs::is_symlink(status) || !safe_project_path(relative) || managed_control_path(relative) ||
            (!retool_layout(relative) && (directory ? skip_directory(lower) : never_index_file(lower)))) {
            it.disable_recursion_pending();
            ++inventory.omissions.excluded_entries;
        } else {
            const auto decision = rules->decide(relative, directory, parents.back());
            if (decision.excluded && !(directory && rules->may_include_below(relative, decision))) {
                it.disable_recursion_pending();
                ++inventory.omissions.excluded_entries;
            } else if (directory) {
                if (!decision.git_ignored) rules->enter_directory(relative);
                parents.push_back(decision);
            } else if (fs::is_regular_file(status)) {
                const auto metadata = source_file_metadata(it->path(), "classify-project-file");
                if (metadata.byte_count > limit) {
                    ++inventory.omissions.oversized_files;
                } else if (!regular_text(root, relative, metadata, limit)) {
                    ++inventory.omissions.binary_files;
                } else {
                    inventory.sources.paths.push_back(relative);
                    inventory.sources.text_bytes += metadata.byte_count;
                }
            }
        }
        it.increment(ec);
        if (ec) throw_source_observation_error("enumerate-source-entry", ec);
    }
    inventory.exclusions = rules->binding();
    if (!inventory.exclusions.current(root)) throw_source_changed_during_verification("read-project-exclusions");
    auto& sources = inventory.sources;
    std::sort(sources.paths.begin(), sources.paths.end());
    sources.candidate_file_count = static_cast<int>(sources.paths.size());
    if (profile.source_fact_max_files > 0 && sources.candidate_file_count > profile.source_fact_max_files) {
        sources.capped_file_count = sources.candidate_file_count - profile.source_fact_max_files;
        sources.paths.resize(static_cast<std::size_t>(profile.source_fact_max_files));
    }
    for (const auto& path : sources.paths)
        if (is_document(path)) inventory.documents.push_back(path);
    return inventory;
}
} // namespace shin::context
