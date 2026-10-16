#include "context_internal.h"
#include "generated_snapshot.h"

namespace shin::context {

namespace {

constexpr int kScopeCoverageBoostPerGroup = 1000;
constexpr int kPreferredStemBoost = 6000;
constexpr int kPreferredStemSuffixBoost = 5500;
constexpr int kScopeStemBoost = 5000;
constexpr int kDirectChildBoost = 2000;
constexpr int kGrandchildBoost = 1000;
constexpr int kDocumentOwnerBoost = 12000;
constexpr std::size_t kMinHumanizedOwnerSegments = 3;
constexpr std::size_t kMaxHumanizedOwnerSegments = 8;
constexpr std::size_t kMaxHumanizedQuerySegments = 24;
constexpr std::size_t kMaxHumanizedInterposedSegments = 1;

struct ScopedBriefPath {
    std::set<int> matched_groups;
    std::vector<ContextHit> sections;
    std::vector<FactHit> facts;
    std::set<FactRow> seen_fact_keys;
    int matched_sections = 0;
    int matched_facts = 0;
    int deduplicated_facts = 0;
    int matched_decisions = 0;
    int matched_contracts = 0;
    int matched_runbooks = 0;
    int matched_other_facts = 0;
};

int semantic_group_coverage(const ScopedBriefPath& path, int scope_group_index) {
    return static_cast<int>(std::count_if(path.matched_groups.begin(), path.matched_groups.end(),
                                          [scope_group_index](int group) {
                                              return group != scope_group_index;
                                          }));
}

std::set<int> topical_group_coverage(std::vector<FieldMatchInput> fields,
                                    const PreparedTermGroups& groups,
                                    const std::string& scope_path) {
    std::set<int> matched;
    for (auto& field : fields) {
        if (!field.primary ||
            (field.name != "title" && field.name != "summary" && field.name != "record_type" &&
             field.name != "identifier_references" && field.name != "heading" &&
             field.name != "preview" && field.name != "indexed_text")) continue;
        // Ownership metadata tokens are identity, including the key in key=path.
        std::string content(field.text);
        if (!scope_path.empty()) {
            for (auto offset = content.find(scope_path); offset != std::string::npos;
                 offset = content.find(scope_path, offset)) {
                const auto before = content.find_last_of(" \t\r\n", offset);
                const auto begin = before == std::string::npos ? 0 : before + 1;
                const auto after = content.find_first_of(" \t\r\n", offset + scope_path.size());
                content.replace(begin, after == std::string::npos ? std::string::npos : after - begin, " ");
                offset = begin + 1;
            }
        }
        field.text = content;
        for (std::size_t index = 1; index < groups.size(); ++index) {
            if (std::any_of(groups[index].begin(), groups[index].end(),
                            [&](const auto& term) {
                                return std::none_of(groups.front().begin(), groups.front().end(),
                                    [&](const auto& scope) { return term.compact == scope.compact; }) &&
                                    field_contains_term(field, term);
                            })) {
                matched.insert(static_cast<int>(index));
            }
        }
    }
    return matched;
}

std::string scope_anchor_compact(const QueryRequest& request) {
    if (request.terms.empty()) {
        return {};
    }
    std::string raw_anchor = request.terms.front();
    if (raw_anchor.find('/') != std::string::npos) {
        raw_anchor = fs::path(raw_anchor).filename().string();
    }
    std::string anchor = prepare_match_term(raw_anchor).compact;
    std::size_t digits = anchor.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(anchor[digits - 1]))) {
        --digits;
    }
    if (digits > 1 && digits < anchor.size() && anchor[digits - 1] == 'v') {
        anchor.resize(digits - 1);
    }
    return anchor;
}

int scope_path_affinity_boost(const Profile& profile,
                              const std::string& path,
                              const std::string& anchor) {
    const std::string stem = prepare_match_term(fs::path(path).stem().string()).compact;
    if (stem.size() < 3) {
        return 0;
    }
    for (const std::string& preferred : profile.compound_query_preferred_stems) {
        if (stem == prepare_match_term(preferred).compact) {
            return kPreferredStemBoost;
        }
    }
    for (const std::string& preferred_suffix : profile.compound_query_preferred_stem_suffixes) {
        const std::string suffix = prepare_match_term(preferred_suffix).compact;
        if (suffix.size() >= 3 && stem.size() > suffix.size() && ends_with(stem, suffix)) {
            return kPreferredStemSuffixBoost;
        }
    }
    if (anchor.size() < 3) {
        return 0;
    }
    std::vector<std::string> scope_stems = {anchor};
    if (anchor.size() > 3 && ends_with(anchor, "s")) {
        scope_stems.push_back(anchor.substr(0, anchor.size() - 1));
    }
    for (const std::string& scope_stem : scope_stems) {
        for (const std::string& suffix : profile.compound_query_scope_stem_suffixes) {
            if (stem == scope_stem + prepare_match_term(suffix).compact) {
                return kScopeStemBoost;
            }
        }
    }
    return 0;
}

int scope_path_proximity_boost(const std::string& path, const std::string& anchor) {
    if (anchor.size() < 3) {
        return 0;
    }
    std::vector<std::string> components;
    for (const fs::path& component : fs::path(path)) {
        components.push_back(prepare_match_term(component.string()).compact);
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (components[i] == anchor || starts_with(components[i], anchor) ||
            starts_with(anchor, components[i])) {
            const std::size_t remaining = components.size() - i - 1;
            if (remaining == 1) {
                return kDirectChildBoost;
            }
            if (remaining == 2) {
                return kGrandchildBoost;
            }
            break;
        }
    }
    return 0;
}

template <typename Hit>
void add_scope_boost(Hit& hit, const std::string& name, int boost) {
    if (boost <= 0) {
        return;
    }
    hit.score += boost;
    hit.score_components[name] = boost;
    append_unique_string(hit.matched_fields, name);
}

void count_scoped_fact_group(BriefResult& result, const FactRow& fact) {
    const std::string group = brief_fact_group(fact);
    if (group == "decisions") {
        ++result.matched_decisions;
    } else if (group == "contracts") {
        ++result.matched_contracts;
    } else if (group == "runbooks") {
        ++result.matched_runbooks;
    } else {
        ++result.matched_other_facts;
    }
}

void append_scoped_fact_hit(BriefResult& result, FactHit hit) {
    const std::string group = brief_fact_group(hit.fact);
    if (group == "decisions") {
        result.decisions.push_back(std::move(hit));
    } else if (group == "contracts") {
        result.contracts.push_back(std::move(hit));
    } else if (group == "runbooks") {
        result.runbooks.push_back(std::move(hit));
    } else {
        result.facts.push_back(std::move(hit));
    }
}

void sort_and_cap_fact_hits(std::vector<FactHit>& hits, int limit) {
    std::sort(hits.begin(), hits.end(), fact_hit_better);
    if (static_cast<int>(hits.size()) > limit) {
        hits.resize(static_cast<std::size_t>(limit));
    }
}

std::string normalized_owner_path(std::string raw) {
    std::replace(raw.begin(), raw.end(), '\\', '/');
    raw = to_lower(trim(raw));
    while (starts_with(raw, "./")) {
        raw.erase(0, 2);
    }
    return fs::path(raw).lexically_normal().generic_string();
}

bool explicit_document_anchor(const std::string& anchor) {
    return anchor.find('/') != std::string::npos || anchor.find('.') != std::string::npos ||
           anchor.find('-') != std::string::npos || anchor.find('_') != std::string::npos ||
           anchor.find(' ') != std::string::npos;
}

struct DocumentOwnerResolution {
    std::string anchor;
    std::string path;
    bool explicit_identity = false;
    std::size_t group_index = 0;
};

struct DirectoryOwnerResolution {
    std::string anchor;
    std::string path_prefix;
    bool ambiguous = false;
};

template <typename Callback>
void for_each_manifest_source_path(const Json& manifest, Callback callback,
                                  const char* only_field = nullptr) {
    for (const char* field : {"sources", "source_fact_sources"}) {
        if (only_field != nullptr && std::string_view(field) != only_field) continue;
        const Json* sources = find_json_field(manifest, field);
        if (sources == nullptr || !sources->is_array()) {
            continue;
        }
        for (const Json& source : *sources) {
            const std::string path = json_string_value(source, "path");
            if (!path.empty()) {
                callback(path);
            }
        }
    }
}

std::vector<std::string> identifier_segments(std::string_view value) {
    std::vector<std::string> segments;
    std::string segment;
    auto flush = [&]() {
        if (index_term_has_signal(segment)) {
            segments.push_back(std::move(segment));
        }
        segment.clear();
    };

    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(value[index]);
        if (!is_ascii_alnum(current)) {
            flush();
            continue;
        }
        const bool current_upper = is_ascii_upper(current);
        const bool previous_lower_or_digit = index > 0 &&
            (is_ascii_lower(static_cast<unsigned char>(value[index - 1])) ||
             is_ascii_digit(static_cast<unsigned char>(value[index - 1])));
        const bool acronym_boundary = current_upper && index > 0 && index + 1 < value.size() &&
            is_ascii_upper(static_cast<unsigned char>(value[index - 1])) &&
            is_ascii_lower(static_cast<unsigned char>(value[index + 1]));
        if (!segment.empty() && current_upper && (previous_lower_or_digit || acronym_boundary)) {
            flush();
        }
        segment.push_back(static_cast<char>(ascii_lower(current)));
    }
    flush();
    return segments;
}

std::vector<std::string> literal_query_segments(const QueryRequest& request) {
    std::vector<std::string> segments;
    for (const std::string& term : request.terms) {
        std::vector<std::string> term_segments = identifier_segments(term);
        for (std::string& segment : term_segments) {
            segments.push_back(std::move(segment));
        }
        if (segments.size() > kMaxHumanizedQuerySegments) {
            return {};
        }
    }
    return segments;
}

struct OwnerSegmentMatch {
    std::size_t completion_index;
    std::size_t interposed_segments;
};

std::optional<OwnerSegmentMatch> match_owner_segments(
    const std::vector<std::string>& owner,
    const std::vector<std::string>& query) {
    if (owner.size() < kMinHumanizedOwnerSegments ||
        owner.size() > kMaxHumanizedOwnerSegments || query.size() < owner.size() ||
        owner[0] != query[0] || owner[1] != query[1]) {
        return std::nullopt;
    }
    std::size_t owner_index = 2;
    for (std::size_t query_index = 2; query_index < query.size(); ++query_index) {
        const std::string& segment = query[query_index];
        if (segment != owner[owner_index]) {
            continue;
        }
        ++owner_index;
        if (owner_index == owner.size()) {
            return OwnerSegmentMatch{query_index, query_index + 1 - owner.size()};
        }
    }
    return std::nullopt;
}

bool trailing_version_segment(const std::string& segment) {
    return segment.size() > 1 && segment.front() == 'v' &&
           std::all_of(segment.begin() + 1, segment.end(), [](char value) {
               return is_ascii_digit(static_cast<unsigned char>(value));
           });
}

std::set<std::string> humanized_document_owner_paths(const Json& manifest,
                                                      const QueryRequest& request) {
    const std::vector<std::string> query_segments = literal_query_segments(request);
    if (query_segments.empty()) {
        return {};
    }
    std::size_t best_specificity = 0;
    std::size_t best_completion = std::numeric_limits<std::size_t>::max();
    std::size_t best_contiguous_specificity = 0;
    std::set<std::string> paths;
    for_each_manifest_source_path(manifest, [&](const std::string& path) {
        std::vector<std::string> owner_segments =
            identifier_segments(fs::path(path).stem().string());
        if (!owner_segments.empty() && trailing_version_segment(owner_segments.back())) {
            owner_segments.pop_back();
        }
        const std::optional<OwnerSegmentMatch> match =
            match_owner_segments(owner_segments, query_segments);
        if (!match.has_value()) {
            return;
        }
        if (match->interposed_segments == 0) {
            best_contiguous_specificity =
                std::max(best_contiguous_specificity, owner_segments.size());
            return;
        }
        if (match->interposed_segments > kMaxHumanizedInterposedSegments) {
            return;
        }
        if (owner_segments.size() < best_specificity ||
            (owner_segments.size() == best_specificity &&
             match->completion_index > best_completion)) {
            return;
        }
        if (owner_segments.size() > best_specificity ||
            match->completion_index < best_completion) {
            best_specificity = owner_segments.size();
            best_completion = match->completion_index;
            paths.clear();
        }
        if (owner_segments.size() == best_specificity &&
            match->completion_index == best_completion) {
            paths.insert(path);
        }
    });
    if (best_contiguous_specificity >= best_specificity) {
        return {};
    }
    return paths;
}

bool query_starts_with_owner_alias(std::string_view query, std::string_view alias) {
    return query == alias ||
           (query.size() > alias.size() && query.starts_with(alias) &&
            std::isspace(static_cast<unsigned char>(query[alias.size()])));
}

bool query_contains_spaced_owner_alias(std::string_view query, std::string_view alias) {
    if (alias.find(' ') == std::string_view::npos) return false;
    for (auto position = query.find(alias); position != std::string_view::npos;
         position = query.find(alias, position + 1)) {
        if ((position == 0 || std::isspace(static_cast<unsigned char>(query[position - 1]))) &&
            query_starts_with_owner_alias(query.substr(position), alias)) return true;
    }
    return false;
}

std::optional<DocumentOwnerResolution> resolve_document_owner(const Json& manifest,
                                                               const QueryRequest& request) {
    // A complete literal filename identifies the same owner wherever it occurs.
    // Use manifest identity, not an alias or a document's mention of that file.
    struct FileAnchor {
        std::size_t group;
        std::string normalized;
        bool directory;
        std::set<std::string> paths;
    };
    std::vector<FileAnchor> filenames;
    for (std::size_t index = 0; index < request.terms.size(); ++index) {
        const auto normalized = normalized_owner_path(request.terms[index]);
        const fs::path path(normalized);
        if (path.has_extension() && path.extension() != ".")
            filenames.push_back({index, normalized, normalized.find('/') != std::string::npos, {}});
    }
    std::set<std::string> first_term_paths;
    std::map<std::string, std::set<std::string>> prefix_paths;
    const std::string first_term = request.terms.empty() ? std::string{} : request.terms.front();
    const fs::path anchor_path(normalized_owner_path(first_term));
    const std::string normalized_anchor = anchor_path.generic_string();
    const bool anchor_has_directory = first_term.find('/') != std::string::npos ||
                                      first_term.find('\\') != std::string::npos;
    const bool anchor_has_extension = anchor_path.has_extension();
    std::string query = to_lower(trim(request.query));
    std::replace(query.begin(), query.end(), '\\', '/');
    while (starts_with(query, "./")) {
        query.erase(0, 2);
    }

    bool spaced_literal = false;
    for_each_manifest_source_path(manifest, [&](const std::string& path) {
        const std::string normalized = normalized_owner_path(path);
        const std::string_view whole(normalized);
        const std::size_t slash = whole.find_last_of('/');
        const std::string_view filename = whole.substr(slash == std::string_view::npos ? 0 : slash + 1);
        spaced_literal = spaced_literal || query_contains_spaced_owner_alias(query, whole) ||
            query_contains_spaced_owner_alias(query, filename);
        for (auto& anchor : filenames)
            if ((anchor.directory ? whole : filename) == anchor.normalized) anchor.paths.insert(path);
        const std::size_t dot = filename.find_last_of('.');
        const bool extension = dot != std::string_view::npos && dot != 0 && filename != "..";
        const std::string_view stem = extension ? filename.substr(0, dot) : filename;
        const std::string_view path_stem = whole.substr(0, whole.size() - filename.size() + stem.size());
        const std::string_view candidate = anchor_has_directory
            ? (anchor_has_extension ? whole : path_stem)
            : (anchor_has_extension ? filename : stem);
        if (!first_term.empty() && candidate == normalized_anchor) {
            first_term_paths.insert(path);
        }
        for (const std::string_view alias : {whole, path_stem, filename, stem}) {
            if (!alias.empty() && query_starts_with_owner_alias(query, alias)) {
                prefix_paths[std::string(alias)].insert(path);
            }
        }
    });
    // Preserve leading directory boundaries and complete literal aliases with spaces.
    const bool longer_literal_prefix = std::any_of(prefix_paths.begin(), prefix_paths.end(), [&](const auto& entry) {
        return entry.first.size() > normalized_anchor.size() && entry.first.find(' ') != std::string::npos;
    });
    // Never reinterpret the final token of a literal multiword filename as
    // another file. Existing leading aliases still resolve below.
    if (!(anchor_has_directory && !anchor_has_extension) && !longer_literal_prefix && !spaced_literal) {
        std::optional<DocumentOwnerResolution> explicit_owner;
        for (const auto& anchor : filenames) {
            // Duplicate basenames and multiple named owners remain multi-document queries.
            if (anchor.paths.size() > 1) return std::nullopt;
            if (anchor.paths.empty()) continue;
            const auto& path = *anchor.paths.begin();
            if (explicit_owner && explicit_owner->path != path) return std::nullopt;
            if (!explicit_owner)
                explicit_owner = DocumentOwnerResolution{request.terms[anchor.group], path, true, anchor.group};
        }
        if (explicit_owner) return explicit_owner;
    }
    // A bare first word is weaker than a complete humanized filename. Preserve
    // existing resolution for explicit paths, identifiers and longer literal aliases.
    if (!first_term_paths.empty() && !explicit_document_anchor(first_term) &&
        identifier_segments(first_term).size() == 1 &&
        std::none_of(prefix_paths.begin(), prefix_paths.end(), [&](const auto& entry) {
            return entry.first.size() > normalized_anchor.size();
        })) {
        const auto humanized_paths = humanized_document_owner_paths(manifest, request);
        if (humanized_paths.size() > 1) return std::nullopt;
        if (humanized_paths.size() == 1)
            return DocumentOwnerResolution{first_term, *humanized_paths.begin(), true};
    }
    if (first_term_paths.size() == 1) {
        return DocumentOwnerResolution{first_term, *first_term_paths.begin(), false};
    }
    if (first_term_paths.size() > 1) {
        return std::nullopt;
    }
    if (!prefix_paths.empty()) {
        const auto longest = std::max_element(
            prefix_paths.begin(), prefix_paths.end(), [](const auto& left, const auto& right) {
                return std::tuple(left.first.size(), left.first) <
                       std::tuple(right.first.size(), right.first);
            });
        if (longest->second.size() != 1) {
            return std::nullopt;
        }
        return DocumentOwnerResolution{longest->first, *longest->second.begin(), false};
    }

    const std::set<std::string> humanized_paths =
        humanized_document_owner_paths(manifest, request);
    if (humanized_paths.size() != 1) {
        return std::nullopt;
    }
    return DocumentOwnerResolution{first_term, *humanized_paths.begin(), true};
}

bool path_in_directory(const std::string& path, const std::string& directory) {
    const std::string normalized_path = normalized_owner_path(path);
    std::string normalized_directory = normalized_owner_path(directory);
    while (!normalized_directory.empty() && normalized_directory.back() == '/') {
        normalized_directory.pop_back();
    }
    return !normalized_directory.empty() &&
           starts_with(normalized_path, normalized_directory + "/");
}

std::vector<std::string> canonical_path_components(const std::string& path) {
    std::vector<std::string> components;
    for (const fs::path& component : fs::path(normalized_owner_path(path))) {
        const std::string compact = prepare_match_term(component.string()).compact;
        if (compact.empty()) {
            return {};
        }
        components.push_back(compact);
    }
    return components;
}

bool equivalent_directory_path(const std::string& candidate, const std::string& anchor) {
    const std::vector<std::string> candidate_components = canonical_path_components(candidate);
    const std::vector<std::string> anchor_components = canonical_path_components(anchor);
    return !candidate_components.empty() && candidate_components == anchor_components;
}

std::optional<DirectoryOwnerResolution> resolve_directory_owner(const Json& manifest,
                                                                 const QueryRequest& request) {
    if (request.terms.empty()) {
        return std::nullopt;
    }
    const std::string raw_anchor = request.terms.front();
    if (raw_anchor.find('/') != std::string::npos || raw_anchor.find('\\') != std::string::npos) {
        const std::string normalized_anchor = normalized_owner_path(raw_anchor);
        if (!fs::path(normalized_anchor).has_extension()) {
            std::set<std::string> matching_directories;
            for_each_manifest_source_path(manifest, [&](const std::string& path) {
                fs::path prefix;
                const fs::path parent = fs::path(normalized_owner_path(path)).parent_path();
                for (const fs::path& component : parent) {
                    prefix /= component;
                    if (equivalent_directory_path(prefix.generic_string(), normalized_anchor)) {
                        matching_directories.insert(prefix.generic_string());
                    }
                }
            });
            if (matching_directories.size() == 1) {
                return DirectoryOwnerResolution{raw_anchor, *matching_directories.begin()};
            }
            if (matching_directories.size() > 1) {
                return DirectoryOwnerResolution{raw_anchor, {}, true};
            }
            return DirectoryOwnerResolution{raw_anchor, normalized_anchor};
        }
        return std::nullopt;
    }

    const std::string compact_anchor = prepare_match_term(raw_anchor).compact;
    if (compact_anchor.size() < 3) {
        return std::nullopt;
    }
    std::set<std::string> matching_directories;
    const auto collect_directories = [&](const std::string& path) {
        fs::path prefix;
        const fs::path parent = fs::path(normalized_owner_path(path)).parent_path();
        for (const fs::path& component : parent) {
            prefix /= component;
            if (prepare_match_term(component.string()).compact == compact_anchor) {
                matching_directories.insert(prefix.generic_string());
            }
        }
    };
    // Interpretation documents may refer to an owner without becoming another source owner.
    for_each_manifest_source_path(manifest, collect_directories, "source_fact_sources");
    if (matching_directories.size() > 1) {
        return DirectoryOwnerResolution{raw_anchor, {}, true};
    }
    if (matching_directories.empty()) {
        for_each_manifest_source_path(manifest, collect_directories, "sources");
    }
    if (matching_directories.size() != 1) {
        return std::nullopt;
    }
    return DirectoryOwnerResolution{raw_anchor, *matching_directories.begin()};
}

}  // namespace

BriefResult collect_brief_from_index_document_owner_scope(const RuntimeOptions& options,
                                                          const QueryRequest& request,
                                                          const Json* manifest) {
    BriefResult result;
    if (request.prepared_term_groups.empty() || request.terms.empty()) {
        return result;
    }

    Json loaded_manifest;
    if (manifest == nullptr && options.generated_snapshot) manifest = &options.generated_snapshot->manifest();
    if (manifest == nullptr) {
        const fs::path manifest_path = options.out_root / kGeneratedDir / "manifest.json";
        loaded_manifest = parse_json_text(read_file(manifest_path), manifest_path.string());
        manifest = &loaded_manifest;
    }
    const std::optional<DocumentOwnerResolution> resolution =
        resolve_document_owner(*manifest, request);
    if (!resolution.has_value()) {
        return result;
    }

    const GeneratedPathProbe probe =
        probe_generated_path(options, resolution->path, kMaxIndexedCandidateRows);
    if (!probe.complete) {
        return result;
    }
    std::map<std::string, ScopedBriefPath> paths;
    auto owner_match_groups = request.prepared_term_groups;
    owner_match_groups.at(resolution->group_index).push_back(prepare_match_term(resolution->path));
    FieldMatchInput owner_field{"path", resolution->path, 1, true};
    std::set<int> qualifier_groups;
    for (std::size_t index = 0; index < request.prepared_term_groups.size(); ++index) {
        if (index == resolution->group_index) continue;
        const auto& group = request.prepared_term_groups[index];
        if (std::none_of(group.begin(), group.end(), [&](const auto& term) {
                return field_contains_term(owner_field, term);
            })) {
            qualifier_groups.insert(static_cast<int>(index));
        }
    }
    const auto matches_qualifier = [&](const MatchScore& match) {
        return qualifier_groups.empty() ||
            std::any_of(match.matched_group_indices.begin(), match.matched_group_indices.end(),
                        [&](int group) { return qualifier_groups.contains(group); });
    };
    const auto type_allowed = [&](const std::string& type) {
        return request.allowed_evidence_types.empty() ||
            std::find(request.allowed_evidence_types.begin(), request.allowed_evidence_types.end(), type) !=
                request.allowed_evidence_types.end();
    };
    ScopedBriefPath owner_entry;
    for (const SectionRow& section : probe.sections) {
        MatchScore match = score_section_match(section, owner_match_groups, 1);
        if (match.total <= 0) {
            continue;
        }
        match.total += kDocumentOwnerBoost;
        match.components["document_owner_boost"] += kDocumentOwnerBoost;
        append_unique_string(match.matched_fields, "document_owner_path");
        ScopedBriefPath& scoped = paths[section.path];
        ++scoped.matched_sections;
        scoped.matched_groups.insert(match.matched_group_indices.begin(),
                                     match.matched_group_indices.end());
        if (!type_allowed("section")) continue;
        ScopedBriefPath& retained = matches_qualifier(match) ? scoped : owner_entry;
        push_top_context_hit(retained.sections,
                             {section, match.total, match.components, match.matched_fields,
                              match.matched_group_indices},
                             request.per_doc_limit);
    }
    for (const FactRow& fact : probe.facts) {
        MatchScore match = score_fact_match(fact, owner_match_groups, 1);
        if (match.total <= 0) {
            continue;
        }
        match.total += kDocumentOwnerBoost;
        match.components["document_owner_boost"] += kDocumentOwnerBoost;
        append_unique_string(match.matched_fields, "document_owner_path");
        ScopedBriefPath& scoped = paths[fact.path];
        ++scoped.matched_facts;
        scoped.matched_groups.insert(match.matched_group_indices.begin(),
                                     match.matched_group_indices.end());
        const std::string group = brief_fact_group(fact);
        if (!scoped.seen_fact_keys.insert(fact).second) {
            ++scoped.deduplicated_facts;
            continue;
        }
        if (group == "decisions") {
            ++scoped.matched_decisions;
        } else if (group == "contracts") {
            ++scoped.matched_contracts;
        } else if (group == "runbooks") {
            ++scoped.matched_runbooks;
        } else {
            ++scoped.matched_other_facts;
        }
        if (!type_allowed(group.substr(0, group.size() - 1))) continue;
        ScopedBriefPath& retained = matches_qualifier(match) ? scoped : owner_entry;
        push_top_fact_hit(retained.facts,
                          {fact, match.total, match.components, match.matched_fields,
                           match.matched_group_indices},
                          request.per_doc_limit);
    }

    if (paths.size() != 1) {
        return result;
    }
    std::set<int> owner_groups;
    for (const auto& [path_name, path] : paths) {
        (void)path_name;
        owner_groups.insert(path.matched_groups.begin(), path.matched_groups.end());
    }
    const int total_groups = strict_required_group_matches(request);
    if (total_groups > 1 && !resolution->explicit_identity &&
        !explicit_document_anchor(resolution->anchor) &&
        static_cast<int>(owner_groups.size()) < total_groups) {
        return BriefResult{};
    }
    result.document_owner_scope_used = true;
    result.scope_group_index = static_cast<int>(resolution->group_index);
    result.required_group_matches = 1;
    result.total_term_groups = total_groups;
    result.candidate_rows = static_cast<std::size_t>(probe.rows_examined);
    result.candidate_index_used = true;
    for (auto& [path_name, path] : paths) {
        // Owner-only entries must not consume the quota of matching qualifiers.
        if (path.sections.empty() && path.facts.empty()) {
            path.sections = std::move(owner_entry.sections);
            path.facts = std::move(owner_entry.facts);
        }
        result.matched_sections = path.matched_sections;
        result.matched_facts = path.matched_facts;
        result.deduplicated_facts = path.deduplicated_facts;
        result.matched_decisions = path.matched_decisions;
        result.matched_contracts = path.matched_contracts;
        result.matched_runbooks = path.matched_runbooks;
        result.matched_other_facts = path.matched_other_facts;
        if (!path.sections.empty()) {
            std::sort(path.sections.begin(), path.sections.end(), context_hit_better);
            result.section_hits_by_doc[path_name] = std::move(path.sections);
        }
        for (FactHit& hit : path.facts) {
            append_scoped_fact_hit(result, std::move(hit));
        }
    }
    return result;
}

BriefResult collect_brief_from_index_first_term_scope(const RuntimeOptions& options,
                                                       const QueryRequest& request,
                                                       const Json* manifest) {
    BriefResult result;
    result.fallback_used = true;
    result.scope_fallback_used = true;
    result.scope_group_index = 0;
    result.required_group_matches = 1;
    result.total_term_groups = strict_required_group_matches(request);
    if (request.prepared_term_groups.empty()) {
        return result;
    }

    Json loaded_manifest;
    if (manifest == nullptr && options.generated_snapshot) manifest = &options.generated_snapshot->manifest();
    if (manifest == nullptr) {
        const fs::path manifest_path = options.out_root / kGeneratedDir / "manifest.json";
        loaded_manifest = parse_json_text(read_file(manifest_path), manifest_path.string());
        manifest = &loaded_manifest;
    }
    const std::optional<DirectoryOwnerResolution> directory =
        resolve_directory_owner(*manifest, request);
    result.directory_owner_scope_used = directory.has_value();
    if (directory.has_value() && directory->ambiguous) return result;

    const auto preferred_scope_entry = [&](const std::string& path) {
        return directory.has_value() &&
            fs::path(normalized_owner_path(path)).parent_path().generic_string() == directory->path_prefix &&
            scope_path_affinity_boost(options.profile, path, "") == kPreferredStemBoost;
    };
    std::set<std::string> entry_paths;
    if (directory.has_value()) {
        for_each_manifest_source_path(*manifest, [&](const std::string& path) {
            if (preferred_scope_entry(path)) entry_paths.insert(path);
        });
    }
    const auto type_allowed = [&](const std::string& type) {
        return request.allowed_evidence_types.empty() ||
            std::find(request.allowed_evidence_types.begin(), request.allowed_evidence_types.end(), type) !=
                request.allowed_evidence_types.end();
    };
    const auto fact_allowed = [&](const FactRow& fact) {
        const std::string group = brief_fact_group(fact);
        return type_allowed(group.substr(0, group.size() - 1));
    };

    PreparedTermGroups scope_groups = {request.prepared_term_groups.front()};
    if (directory.has_value()) {
        scope_groups = prepare_term_groups(
            {{fs::path(directory->path_prefix).filename().string()}});
    }
    auto match_groups = request.prepared_term_groups;
    if (directory.has_value()) match_groups.front() = scope_groups.front();
    const CandidateRows candidates = load_generated_query_candidates(options, scope_groups, 1);
    result.candidate_rows = candidates.discovered_row_count;
    result.candidate_index_used = candidates.from_terms_index;
    std::map<std::string, ScopedBriefPath> paths;
    std::map<std::string, ScopedBriefPath> entry_context;

    for_each_generated_section_matching_terms(options, candidates, scope_groups, 1,
                                               [&](const SectionRow& section) {
        if (directory.has_value() && !path_in_directory(section.path, directory->path_prefix)) {
            return;
        }
        if (score_section_match(section, scope_groups, 1).total <= 0) {
            return;
        }
        MatchScore match = score_section_match(section, match_groups, 1);
        const auto topical_groups = topical_group_coverage(section_match_fields(section), match_groups,
            directory ? section.path.substr(0, directory->path_prefix.size()) : "");
        const bool context_only = topical_groups.empty();
        if (context_only && (!entry_paths.contains(section.path) || !type_allowed("section"))) {
            return;
        }
        if (!context_only) ++result.matched_sections;
        ScopedBriefPath& path = context_only ? entry_context[section.path] : paths[section.path];
        path.matched_groups.insert(topical_groups.begin(), topical_groups.end());
        ContextHit hit{section, match.total, match.components, match.matched_fields,
                       match.matched_group_indices};
        push_top_context_hit(path.sections, hit, context_only ? 1 : request.per_doc_limit);
    });

    for_each_generated_fact_matching_terms(options, candidates, scope_groups, 1,
                                            [&](const FactRow& fact) {
        if (directory.has_value() && !path_in_directory(fact.path, directory->path_prefix)) {
            return;
        }
        if (score_fact_match(fact, scope_groups, 1).total <= 0) {
            return;
        }
        MatchScore match = score_fact_match(fact, match_groups, 1);
        const auto topical_groups = topical_group_coverage(fact_match_fields(fact), match_groups,
            directory ? fact.path.substr(0, directory->path_prefix.size()) : "");
        const bool context_only = topical_groups.empty();
        if (context_only && (!entry_paths.contains(fact.path) || !fact_allowed(fact))) {
            return;
        }
        if (!result.seen_fact_keys.insert(fact).second) {
            ++result.deduplicated_facts;
            return;
        }
        if (!context_only) {
            ++result.matched_facts;
            count_scoped_fact_group(result, fact);
        }
        ScopedBriefPath& path = context_only ? entry_context[fact.path] : paths[fact.path];
        path.matched_groups.insert(topical_groups.begin(), topical_groups.end());
        FactHit hit{fact, match.total, match.components, match.matched_fields,
                    match.matched_group_indices};
        push_top_fact_hit(path.facts, hit, context_only ? 1 : request.per_doc_limit);
    });

    // Entry context is one fallback record, never a replacement for topical evidence.
    for (auto& [path_name, entry] : entry_context) {
        const auto topical = paths.find(path_name);
        if (topical != paths.end() &&
            ((!topical->second.sections.empty() && type_allowed("section")) ||
             std::any_of(topical->second.facts.begin(), topical->second.facts.end(),
                         [&](const FactHit& hit) { return fact_allowed(hit.fact); }))) continue;
        if (!entry.sections.empty() && !entry.facts.empty()) {
            if (entry.facts.front().score >= entry.sections.front().score) entry.sections.clear();
            else entry.facts.clear();
        }
        result.matched_sections += static_cast<int>(entry.sections.size());
        result.matched_facts += static_cast<int>(entry.facts.size());
        for (const FactHit& hit : entry.facts) count_scoped_fact_group(result, hit.fact);
        paths.insert_or_assign(path_name, std::move(entry));
    }

    const std::string anchor = scope_anchor_compact(request);
    for (auto& [path_name, path] : paths) {
        const int coverage = semantic_group_coverage(path, result.scope_group_index);
        const int affinity_boost = scope_path_affinity_boost(options.profile, path_name, anchor);
        const int proximity_boost = scope_path_proximity_boost(path_name, anchor);
        for (ContextHit& hit : path.sections) {
            add_scope_boost(hit, "scope_coverage_boost", coverage * kScopeCoverageBoostPerGroup);
            add_scope_boost(hit, "scope_path_affinity_boost", affinity_boost);
            add_scope_boost(hit, "scope_path_proximity_boost", proximity_boost);
        }
        if (!path.sections.empty()) {
            std::sort(path.sections.begin(), path.sections.end(), context_hit_better);
            result.section_hits_by_doc[path_name] = std::move(path.sections);
        }
        for (FactHit& hit : path.facts) {
            add_scope_boost(hit, "scope_coverage_boost", coverage * kScopeCoverageBoostPerGroup);
            add_scope_boost(hit, "scope_path_affinity_boost", affinity_boost);
            add_scope_boost(hit, "scope_path_proximity_boost", proximity_boost);
            append_scoped_fact_hit(result, std::move(hit));
        }
    }

    const int retained_limit = request.limit * request.per_doc_limit;
    sort_and_cap_fact_hits(result.decisions, retained_limit);
    sort_and_cap_fact_hits(result.contracts, retained_limit);
    sort_and_cap_fact_hits(result.runbooks, retained_limit);
    sort_and_cap_fact_hits(result.facts, retained_limit);
    return result;
}

}  // namespace shin::context
