#include "validation_generation.h"
#include "context_internal.h"
#include "compact_terms.h"
#include "generated_io.h"
#include "snapshot_verification.h"
#include "source_read.h"
#include <array>

namespace shin::context {

static std::map<std::string, const SourceDoc*> docs_by_id(const std::vector<SourceDoc>& docs) {
    std::map<std::string, const SourceDoc*> result;
    for (const SourceDoc& doc : docs) {
        result[doc.id] = &doc;
    }
    return result;
}

static std::vector<std::string> merged_authority_topics(const SourceDoc& doc, const SectionRow& section) {
    std::vector<std::string> topics = section.topics;
    for (const std::string& topic : doc.topics) {
        append_unique_string(topics, topic);
    }
    return topics;
}

static std::string classify_markdown_authority_section(const SectionRow& section) {
    if (section.row_type != "section") {
        return "";
    }
    const std::string heading = to_lower(section.heading);
    if (contains_any(heading, {"decision", "verdict", "accepted decision", "adr", "resolution"})) {
        return "decision";
    }
    if (contains_any(heading,
                     {"contract", "api contract", "openapi", "asyncapi", "schema", "protocol",
                      "interface contract", "capability contract", "result envelope"})) {
        return "contract";
    }
    if (contains_any(heading,
                     {"runbook", "playbook", "procedure", "rollback", "deployment", "operations",
                      "maintenance", "smoke", "troubleshooting"})) {
        return "runbook";
    }
    return "";
}

static std::string section_authority_row_id(const SourceDoc& doc,
                                     const SectionRow& section,
                                     const std::string& record_type) {
    return doc.id + "-" + record_type + "-" + section.anchor + "-line-" + std::to_string(section.line);
}

static void append_markdown_authority_rows(const std::vector<SourceDoc>& docs,
                                    const std::vector<SectionRow>& sections,
                                    std::vector<DecisionRow>& decisions,
                                    std::vector<TypedReferenceRow>& contracts,
                                    std::vector<TypedReferenceRow>& runbooks) {
    const std::map<std::string, const SourceDoc*> by_id = docs_by_id(docs);
    for (const SectionRow& section : sections) {
        const std::string record_type = classify_markdown_authority_section(section);
        if (record_type.empty()) {
            continue;
        }
        auto doc_it = by_id.find(section.document_id);
        if (doc_it == by_id.end() || doc_it->second == nullptr || doc_it->second->kind != "markdown") {
            continue;
        }
        const SourceDoc& doc = *doc_it->second;
        const std::string title = redact_sensitive_text(section.heading);
        const std::string summary = redact_sensitive_text(section.preview.empty() ? doc.summary : section.preview);
        const std::string status = section.status;
        const std::string authority_level =
            infer_authority_level(doc.doc_type, record_type, doc.path, status);
        const std::string id = section_authority_row_id(doc, section, record_type);
        if (record_type == "decision") {
            DecisionRow row{id,
                            doc.path,
                            status,
                            doc.area,
                            title,
                            summary,
                            summary,
                            authority_level,
                            merged_authority_topics(doc, section),
                            section.line};
            const EvidenceProvenance provenance = infer_record_evidence_provenance(
                row.path, doc.doc_type, record_type, row.status, row.authority_level, false);
            row.evidence_kind = provenance.evidence_kind;
            row.verification_level = provenance.verification_level;
            decisions.push_back(std::move(row));
        } else {
            std::vector<TypedReferenceRow>& target = record_type == "contract" ? contracts : runbooks;
            TypedReferenceRow row{id,
                                  record_type,
                                  doc.path,
                                  status,
                                  doc.area,
                                  title,
                                  summary,
                                  authority_level,
                                  merged_authority_topics(doc, section),
                                  section.line};
            const EvidenceProvenance provenance = infer_record_evidence_provenance(
                row.path, doc.doc_type, record_type, row.status, row.authority_level, false);
            row.evidence_kind = provenance.evidence_kind;
            row.verification_level = provenance.verification_level;
            target.push_back(std::move(row));
        }
    }
}

static std::vector<DecisionRow> build_decision_rows(const std::vector<SourceDoc>& docs,
                                             const std::map<std::string, std::vector<FactRow>>& facts_by_doc) {
    std::vector<DecisionRow> rows;
    for (const SourceDoc& doc : docs) {
        if (doc.doc_type == "adr") {
            const std::string status = doc.status;
            DecisionRow row{doc.id,
                            doc.path,
                            status,
                            doc.area,
                            doc.title,
                            doc.summary,
                            doc.summary,
                            fact_authority_level_or_default(
                                doc, "decision", status, ""),
                            doc.topics,
                            first_heading_line(doc.lines)};
            const EvidenceProvenance provenance = infer_record_evidence_provenance(
                row.path, doc.doc_type, "decision", row.status, row.authority_level, false);
            row.evidence_kind = provenance.evidence_kind;
            row.verification_level = provenance.verification_level;
            rows.push_back(std::move(row));
        }
        auto it = facts_by_doc.find(doc.path);
        if (it == facts_by_doc.end()) {
            continue;
        }
        for (const FactRow& fact : it->second) {
            if (fact.record_type != "decision") {
                continue;
            }
            DecisionRow row{fact.id,
                            doc.path,
                            fact.status,
                            fact.area,
                            fact.title,
                            fact.summary,
                            fact.summary,
                            fact.authority_level,
                            fact.topics,
                            fact.line};
            row.evidence_kind = fact.evidence_kind;
            row.verification_level = fact.verification_level;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

static std::vector<TypedReferenceRow> build_typed_reference_rows(const std::vector<SourceDoc>& docs,
                                                          const std::set<std::string>& accepted_doc_types,
                                                          const std::string& output_type) {
    std::vector<TypedReferenceRow> rows;
    for (const SourceDoc& doc : docs) {
        if (accepted_doc_types.find(doc.doc_type) == accepted_doc_types.end()) {
            continue;
        }
        const std::string record_type = output_type.empty() ? doc.doc_type : output_type;
        TypedReferenceRow row{doc.id,
                              record_type,
                              doc.path,
                              doc.status,
                              doc.area,
                              doc.title,
                              doc.summary,
                              fact_authority_level_or_default(
                                  doc, record_type, doc.status, ""),
                              doc.topics,
                              first_heading_line(doc.lines)};
        const EvidenceProvenance provenance = infer_record_evidence_provenance(
            row.path, doc.doc_type, record_type, row.status, row.authority_level, false);
        row.evidence_kind = provenance.evidence_kind;
        row.verification_level = provenance.verification_level;
        rows.push_back(std::move(row));
    }
    return rows;
}

static std::string documents_jsonl(const std::vector<SourceDoc>& docs) {
    std::ostringstream out;
    for (const SourceDoc& doc : docs) {
        out << "{"
            << "\"id\":" << json_string(doc.id)
            << ",\"path\":" << json_string(doc.path)
            << ",\"kind\":" << json_string(doc.kind)
            << ",\"doc_type\":" << json_string(doc.doc_type)
            << ",\"area\":" << json_string(doc.area)
            << ",\"status\":" << json_string(doc.status)
            << ",\"evidence_kind\":" << json_string(doc.evidence_kind)
            << ",\"verification_level\":" << json_string(doc.verification_level)
            << ",\"title\":" << json_string(redact_sensitive_text(doc.title))
            << ",\"summary\":" << json_string(redact_sensitive_text(doc.summary))
            << ",\"content_hash\":" << json_string(doc.content_hash)
            << (doc.quarantine_reason.empty() ? "" : ",\"quarantine_reason\":" + json_string(doc.quarantine_reason))
            << ",\"bytes\":" << doc.byte_count
            << ",\"line_count\":" << doc.line_count
            << ",\"heading_count\":" << doc.heading_count
            << ",\"topics\":" << json_string_array(doc.topics)
            << "}\n";
    }
    return out.str();
}

static std::string decisions_jsonl(const std::vector<DecisionRow>& rows) {
    std::ostringstream out;
    for (const DecisionRow& row : rows) {
        out << "{"
            << "\"id\":" << json_string(row.id)
            << ",\"path\":" << json_string(row.path)
            << ",\"line\":" << row.line
            << ",\"start_line\":" << row.line
            << ",\"end_line\":" << row.line
            << ",\"status\":" << json_string(row.status)
            << ",\"area\":" << json_string(row.area)
            << ",\"title\":" << json_string(redact_sensitive_text(row.title))
            << ",\"summary\":" << json_string(redact_sensitive_text(row.summary))
            << ",\"decision\":" << json_string(redact_sensitive_text(row.decision))
            << ",\"authority_level\":" << json_string(row.authority_level)
            << ",\"evidence_kind\":" << json_string(row.evidence_kind)
            << ",\"verification_level\":" << json_string(row.verification_level)
            << ",\"identifier_terms\":[]"
            << ",\"topics\":" << json_string_array(row.topics)
            << "}\n";
    }
    return out.str();
}

static std::string typed_references_jsonl(const std::vector<TypedReferenceRow>& rows) {
    std::ostringstream out;
    for (const TypedReferenceRow& row : rows) {
        out << "{"
            << "\"id\":" << json_string(row.id)
            << ",\"type\":" << json_string(row.type)
            << ",\"path\":" << json_string(row.path)
            << ",\"line\":" << row.line
            << ",\"start_line\":" << row.line
            << ",\"end_line\":" << row.line
            << ",\"status\":" << json_string(row.status)
            << ",\"area\":" << json_string(row.area)
            << ",\"title\":" << json_string(redact_sensitive_text(row.title))
            << ",\"summary\":" << json_string(redact_sensitive_text(row.summary))
            << ",\"authority_level\":" << json_string(row.authority_level)
            << ",\"evidence_kind\":" << json_string(row.evidence_kind)
            << ",\"verification_level\":" << json_string(row.verification_level)
            << ",\"identifier_terms\":[]"
            << ",\"topics\":" << json_string_array(row.topics)
            << "}\n";
    }
    return out.str();
}

static std::string facts_jsonl(const std::vector<FactRow>& rows) {
    std::ostringstream out;
    for (const FactRow& row : rows) {
        out << "{"
            << "\"id\":" << json_string(row.id)
            << ",\"record_type\":" << json_string(row.record_type)
            << ",\"path\":" << json_string(row.path)
            << ",\"line\":" << row.line
            << ",\"start_line\":" << row.start_line
            << ",\"end_line\":" << row.end_line
            << ",\"status\":" << json_string(row.status)
            << ",\"area\":" << json_string(row.area)
            << ",\"title\":" << json_string(redact_sensitive_text(row.title))
            << ",\"summary\":" << json_string(redact_sensitive_text(row.summary))
            << ",\"source\":" << json_string(redact_sensitive_text(row.source))
            << ",\"authority_level\":" << json_string(row.authority_level)
            << ",\"evidence_kind\":" << json_string(row.evidence_kind)
            << ",\"verification_level\":" << json_string(row.verification_level)
            << ",\"identifier_terms\":" << json_string_array(row.identifier_terms);
        if (!row.symbol_name.empty()) out << ",\"symbol_name\":" << json_string(row.symbol_name);
        if (!row.span_hash.empty()) out << ",\"span_hash\":" << json_string(row.span_hash);
        if (!row.range_origin.empty()) out << ",\"range_origin\":" << json_string(row.range_origin);
        out << ",\"topics\":" << json_string_array(row.topics)
            << "}\n";
    }
    return out.str();
}

static std::string sections_jsonl(const std::vector<SectionRow>& rows) {
    std::ostringstream out;
    for (const SectionRow& row : rows) {
        out << "{"
            << "\"document_id\":" << json_string(row.document_id)
            << ",\"path\":" << json_string(row.path)
            << ",\"status\":" << json_string(row.status)
            << ",\"authority_level\":" << json_string(row.authority_level)
            << ",\"evidence_kind\":" << json_string(row.evidence_kind)
            << ",\"verification_level\":" << json_string(row.verification_level)
            << ",\"line\":" << row.line
            << ",\"start_line\":" << row.start_line
            << ",\"end_line\":" << row.end_line
            << ",\"level\":" << row.level
            << ",\"row_type\":" << json_string(row.row_type)
            << ",\"chunk_index\":" << row.chunk_index
            << ",\"heading\":" << json_string(redact_sensitive_text(row.heading))
            << ",\"anchor\":" << json_string(row.anchor)
            << ",\"preview\":" << json_string(redact_sensitive_text(row.preview))
            << ",\"indexed_text\":" << json_string(row.indexed_text)
            << ",\"indexed_text_omitted_bytes\":" << row.indexed_text_omitted_bytes
            << ",\"topics\":" << json_string_array(row.topics)
            << "}\n";
    }
    return out.str();
}

static std::string links_jsonl(const std::vector<LinkRow>& rows) {
    std::ostringstream out;
    for (const LinkRow& row : rows) {
        out << "{"
            << "\"path\":" << json_string(row.path)
            << ",\"line\":" << row.line
            << ",\"kind\":" << json_string(row.kind)
            << ",\"text\":" << json_string(row.text)
            << ",\"target\":" << json_string(row.target)
            << ",\"status\":" << json_string(row.status)
            << ",\"resolved_path\":" << json_string(row.resolved_path);
        if (row.target_start_line > 0) {
            out << ",\"target_start_line\":" << row.target_start_line
                << ",\"target_end_line\":" << row.target_end_line
                << ",\"target_range_origin\":" << json_string(row.target_range_origin);
        }
        out << "}\n";
    }
    return out.str();
}

static std::string topics_jsonl(const Profile& profile, const std::map<std::string, std::set<std::string>>& topic_docs) {
    std::map<std::string, std::string> descriptions;
    for (const TopicRule& rule : profile.topic_rules) {
        descriptions[rule.id] = rule.description;
    }

    std::ostringstream out;
    for (const auto& [topic, docs] : topic_docs) {
        std::vector<std::string> doc_ids(docs.begin(), docs.end());
        out << "{"
            << "\"id\":" << json_string(topic)
            << ",\"description\":" << json_string(descriptions[topic])
            << ",\"document_count\":" << doc_ids.size()
            << ",\"documents\":" << json_string_array(doc_ids)
            << "}\n";
    }
    return out.str();
}

using IndexTermSet = std::unordered_set<std::string>;

static void add_index_terms_from_text(IndexTermSet& terms, const std::string& text) {
    const auto extracted = lexical_match_terms(text, false);
    terms.insert(extracted.begin(), extracted.end());
}

struct IndexTermExtractionCache {
    static constexpr std::size_t maximum_entries = 1024;
    std::unordered_map<std::string, std::vector<std::string>> text_terms;
};

static void add_index_terms_from_text(IndexTermSet& terms,
                               const std::string& text,
                               IndexTermExtractionCache& cache) {
    if (text.size() > 160) {
        add_index_terms_from_text(terms, text);
        return;
    }
    auto it = cache.text_terms.find(text);
    if (it == cache.text_terms.end()) {
        if (cache.text_terms.size() >= IndexTermExtractionCache::maximum_entries) {
            add_index_terms_from_text(terms, text);
            return;
        }
        it = cache.text_terms.emplace(text, lexical_match_terms(text, false)).first;
    }
    terms.insert(it->second.begin(), it->second.end());
}

static void add_terms_postings(TermPostingMap& postings,
                        const IndexTermSet& terms,
                        std::vector<int> TermPosting::*slot,
                        int row_index) {
    for (const std::string& term : terms) {
        auto [posting, inserted] = postings.try_emplace(term);
        (void)inserted;
        (posting->second.*slot).push_back(row_index);
    }
}

static IndexTermSet section_index_terms(const SectionRow& row, IndexTermExtractionCache& cache) {
    IndexTermSet terms;
    terms.reserve(64);
    for (const auto& field : section_match_fields(row)) {
        if (field.primary) add_index_terms_from_text(terms, std::string(field.text), cache);
    }
    return terms;
}

static IndexTermSet fact_index_terms(const FactRow& row, IndexTermExtractionCache& cache) {
    IndexTermSet terms;
    terms.reserve(48);
    for (const auto& field : fact_match_fields(row)) {
        if (field.primary) add_index_terms_from_text(terms, std::string(field.text), cache);
    }
    return terms;
}

static IndexTermSet decision_index_terms(const DecisionRow& row, IndexTermExtractionCache& cache) {
    return fact_index_terms(query_fact_row(row), cache);
}

static IndexTermSet typed_reference_index_terms(const TypedReferenceRow& row, IndexTermExtractionCache& cache) {
    return fact_index_terms(query_fact_row(row), cache);
}

static std::size_t estimate_term_posting_capacity(const GeneratedRows& rows) {
    return std::max<std::size_t>(
        1024,
        rows.sections.size() * 6 + rows.facts.size() * 4 + rows.decisions.size() * 4 +
            rows.contracts.size() * 4 + rows.runbooks.size() * 4);
}

struct TermPostingWorkerState {
    TermPostingMap postings;
    IndexTermExtractionCache cache;
};

struct TermPostingBuildResult {
    TermPostingMap postings;
    int worker_count = 1;
};

static void append_term_posting(TermPosting& target, const TermPosting& source) {
    target.sections.insert(target.sections.end(), source.sections.begin(), source.sections.end());
    target.facts.insert(target.facts.end(), source.facts.begin(), source.facts.end());
    target.decisions.insert(target.decisions.end(), source.decisions.begin(), source.decisions.end());
    target.contracts.insert(target.contracts.end(), source.contracts.begin(), source.contracts.end());
    target.runbooks.insert(target.runbooks.end(), source.runbooks.begin(), source.runbooks.end());
}

static void merge_term_posting_map(TermPostingMap& target, TermPostingMap& source) {
    for (auto it = source.begin(); it != source.end();) {
        const auto existing = target.find(it->first);
        if (existing == target.end()) {
            ++it;
            continue;
        }
        append_term_posting(existing->second, it->second);
        it = source.erase(it);
    }
    target.merge(source);
}

static TermPostingBuildResult build_term_postings(const GeneratedRows& rows, BuildPhaseTimings* timings) {
    constexpr std::size_t kTargetRowsPerWorker = 2048;
    const std::size_t sections_end = rows.sections.size();
    const std::size_t facts_end = sections_end + rows.facts.size();
    const std::size_t decisions_end = facts_end + rows.decisions.size();
    const std::size_t contracts_end = decisions_end + rows.contracts.size();
    const std::size_t total_rows = contracts_end + rows.runbooks.size();
    const unsigned int workers = worker_count_for_items(total_rows, kTargetRowsPerWorker);
    const std::size_t estimated_capacity = estimate_term_posting_capacity(rows);

    std::vector<TermPostingWorkerState> worker_states(workers);
    for (TermPostingWorkerState& state : worker_states) {
        state.postings.reserve(estimated_capacity / workers + 1024);
        state.cache.text_terms.reserve(IndexTermExtractionCache::maximum_entries);
    }

    // Static row ranges keep posting IDs ordered across the deterministic worker merge.
    parallel_for_worker_indices(total_rows, workers, [&](unsigned int worker, std::size_t index) {
        TermPostingWorkerState& state = worker_states[worker];
        if (index < sections_end) {
            add_terms_postings(state.postings, section_index_terms(rows.sections[index], state.cache),
                               &TermPosting::sections, static_cast<int>(index));
        } else if (index < facts_end) {
            const std::size_t row = index - sections_end;
            add_terms_postings(state.postings, fact_index_terms(rows.facts[row], state.cache), &TermPosting::facts,
                               static_cast<int>(row));
        } else if (index < decisions_end) {
            const std::size_t row = index - facts_end;
            add_terms_postings(state.postings, decision_index_terms(rows.decisions[row], state.cache),
                               &TermPosting::decisions, static_cast<int>(row));
        } else if (index < contracts_end) {
            const std::size_t row = index - decisions_end;
            add_terms_postings(state.postings, typed_reference_index_terms(rows.contracts[row], state.cache),
                               &TermPosting::contracts, static_cast<int>(row));
        } else {
            const std::size_t row = index - contracts_end;
            add_terms_postings(state.postings, typed_reference_index_terms(rows.runbooks[row], state.cache),
                               &TermPosting::runbooks, static_cast<int>(row));
        }
    });

    if (timings) timings->peak_rss_bytes["term_worker_maps"] = process_peak_rss_bytes();
    for (auto& state : worker_states) {
        decltype(state.cache.text_terms){}.swap(state.cache.text_terms);
    }
    TermPostingMap postings = std::move(worker_states.front().postings);
    if (workers > 1) {
        postings.reserve(estimated_capacity);
        for (std::size_t worker = 1; worker < worker_states.size(); ++worker) {
            merge_term_posting_map(postings, worker_states[worker].postings);
        }
    }
    if (timings) timings->peak_rss_bytes["term_merged"] = process_peak_rss_bytes();
    return {std::move(postings), static_cast<int>(workers)};
}

static RenderedTermIndex render_term_index(const TermPostingMap& postings) {
    RenderedTermIndex rendered;
    std::ostringstream postings_out;
    std::ostringstream offsets_out;
    std::vector<std::reference_wrapper<const TermPostingMap::value_type>> ordered_postings;
    ordered_postings.reserve(postings.size());
    for (const auto& posting : postings) {
        ordered_postings.emplace_back(posting);
    }
    std::sort(ordered_postings.begin(), ordered_postings.end(),
              [](const auto& a, const auto& b) {
                  return a.get().first < b.get().first;
              });
    long long offset = 0;
    for (const auto& entry_ref : ordered_postings) {
        const auto& entry = entry_ref.get();
        const std::string& term = entry.first;
        const TermPosting& posting = entry.second;
        postings_out << "{"
            << "\"term\":" << json_string(term)
            << ",\"sections\":" << json_int_array(posting.sections)
            << ",\"facts\":" << json_int_array(posting.facts)
            << ",\"decisions\":" << json_int_array(posting.decisions)
            << ",\"contracts\":" << json_int_array(posting.contracts)
            << ",\"runbooks\":" << json_int_array(posting.runbooks)
            << "}\n";
        const auto end = static_cast<long long>(postings_out.tellp());
        offsets_out << term << "\t" << offset << "\t" << end - offset << "\n";
        offset = end;
        ++rendered.term_count;
    }
    rendered.postings_jsonl = std::move(postings_out).str();
    rendered.offsets_tsv = std::move(offsets_out).str();
    return rendered;
}

static std::string row_offsets_tsv(const std::string& jsonl) {
    std::ostringstream offsets_out;
    long long offset = 0;
    int row = 0;
    std::size_t pos = 0;
    while (pos < jsonl.size()) {
        const std::size_t newline = jsonl.find('\n', pos);
        const std::size_t end = newline == std::string::npos ? jsonl.size() : newline + 1;
        const long long bytes = static_cast<long long>(end - pos);
        if (bytes > 0) {
            offsets_out << row << "\t" << offset << "\t" << bytes << "\n";
            offset += bytes;
            ++row;
        }
        pos = end;
    }
    return offsets_out.str();
}

static std::map<std::string, int> count_docs_by(const std::vector<SourceDoc>& docs, const std::string& field) {
    std::map<std::string, int> counts;
    for (const SourceDoc& doc : docs) {
        if (field == "kind") {
            ++counts[doc.kind];
        } else if (field == "doc_type") {
            ++counts[doc.doc_type];
        } else if (field == "area") {
            ++counts[doc.area];
        } else if (field == "status") {
            ++counts[doc.status];
        }
    }
    return counts;
}

std::string manifest_profile_config_identity(const Profile& profile) {
    const fs::path config_path(profile.config_source);
    if (config_path.empty()) {
        return "";
    }
    const fs::path filename = config_path.filename();
    const fs::path directory = config_path.parent_path().filename();
    return directory.empty() ? filename.generic_string() : (directory / filename).generic_string();
}

static std::string manifest_json(const Profile& profile,
                          const std::vector<SourceDoc>& docs,
                          int section_count,
                          int link_count,
                          int topic_count,
                          int term_count,
                          int decision_count,
                          int contract_count,
                          int runbook_count,
                          int fact_count,
                          const SourceFactBuildResult& source_facts,
                          const StructuredDataCounts& structured_data,
                          const std::map<std::string, std::string>& generated_files,
                          int broken_local_link_count, bool compact) {
    const auto quarantined_count = std::count_if(source_facts.sources.begin(), source_facts.sources.end(),
        [](const auto& source) { return !source.quarantine_reason.empty(); });
    std::ostringstream out;
    out << "{\n"
        << "  \"schema_version\": \"shin.context.kb-enterprise.v1\",\n"
        << "  \"tool\": \"shin-context\",\n"
        << "  \"generator_version\": " << json_string(compact ? kGeneratorVersion : kLegacyGeneratorVersion) << ",\n"
        << (compact ? "  \"term_index_format\": \"shin.lexical.v1\",\n" : "")
        << "  \"generated_at_utc\": \"stable\",\n"
        << "  \"profile\": " << json_string(profile.id) << ",\n"
        << "  \"source\": " << json_string(profile.source_label) << ",\n"
        << "  \"profile_config\": " << json_string(manifest_profile_config_identity(profile)) << ",\n"
        << "  \"profile_config_hash\": " << json_string(profile_config_hash(profile)) << ",\n"
        << "  \"bootstrap_only\": " << (profile.bootstrap_only ? "true" : "false") << ",\n"
        << "  \"required_root_files\": " << json_string_array(profile.root_files) << ",\n"
        << "  \"optional_root_files\": " << json_string_array(profile.optional_root_files) << ",\n"
        << "  \"source_count\": " << docs.size() << ",\n"
        << "  \"source_fact_roots\": " << json_string_array(profile.source_fact_roots) << ",\n"
        << "  \"source_fact_extensions\": " << json_string_array(profile.source_fact_extensions) << ",\n"
        << "  \"source_fact_extractors\": " << json_string_array(profile.source_fact_extractors) << ",\n"
        << "  \"source_fact_attribute_keys\": " << json_string_array(profile.source_fact_attribute_keys) << ",\n"
        << "  \"document_path_policy\": {\"include_prefixes\":"
        << json_string_array(profile.document_include_prefixes)
        << ",\"exclude_prefixes\":" << json_string_array(profile.document_exclude_prefixes) << "},\n"
        << "  \"markdown_chunking\": {\"chunk_lines\":" << profile.markdown_chunk_lines
        << ",\"chunk_min_lines\":" << profile.markdown_chunk_min_lines
        << ",\"chunk_chars\":" << profile.markdown_chunk_chars << "},\n"
        << "  \"source_fact_limits\": {\"max_files\":" << profile.source_fact_max_files
        << ",\"max_file_bytes\":" << profile.source_fact_max_file_bytes << "},\n"
        << "  \"structured_data_limits\": {\"json_key_include_prefixes\":"
        << json_string_array(profile.json_key_include_prefixes)
        << ",\"json_key_max_depth\":" << profile.json_key_max_depth
        << ",\"json_key_max_sections_per_file\":" << profile.json_key_max_sections_per_file
        << ",\"jsonl_max_rows_per_file\":" << profile.jsonl_max_rows_per_file << "},\n"
        << "  \"structured_data_counts\": {\"json_key_candidate_sections\":"
        << structured_data.json_key_candidate_sections
        << ",\"json_key_indexed_sections\":" << structured_data.json_key_indexed_sections
        << ",\"json_key_capped_sections\":" << structured_data.json_key_capped_sections
        << ",\"jsonl_candidate_rows\":" << structured_data.jsonl_candidate_rows
        << ",\"jsonl_indexed_rows\":" << structured_data.jsonl_indexed_rows
        << ",\"jsonl_capped_rows\":" << structured_data.jsonl_capped_rows << "},\n"
        << "  \"source_fact_counts\": {\"candidate_files\":" << source_facts.candidate_file_count
        << ",\"scanned_files\":" << source_facts.scanned_file_count
        << ",\"skipped_large_files\":" << source_facts.skipped_large_file_count
        << (quarantined_count ? ",\"quarantined_files\":" + std::to_string(quarantined_count) : "")
        << ",\"capped_files\":" << source_facts.capped_file_count << "},\n"
        << "  \"source_fact_sources\": [\n";
    for (std::size_t i = 0; i < source_facts.sources.size(); ++i) {
        const SourceFactSource& source = source_facts.sources[i];
        out << "    {\"path\":" << json_string(source.path)
            << ",\"status\":" << json_string(source.status)
            << ",\"content_hash\":" << json_string(source.content_hash)
            << (source.quarantine_reason.empty() ? "" : ",\"quarantine_reason\":" + json_string(source.quarantine_reason))
            << ",\"bytes\":" << source.byte_count << "}";
        if (i + 1 < source_facts.sources.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n"
        << "  \"row_counts\": {\n"
        << "    \"documents\": " << docs.size() << ",\n"
        << "    \"sections\": " << section_count << ",\n"
        << "    \"links\": " << link_count << ",\n"
        << "    \"topics\": " << topic_count << ",\n"
        << "    \"terms\": " << term_count << ",\n"
        << "    \"decisions\": " << decision_count << ",\n"
        << "    \"contracts\": " << contract_count << ",\n"
        << "    \"runbooks\": " << runbook_count << ",\n"
        << "    \"facts\": " << fact_count << "\n"
        << "  },\n"
        << "  \"counts_by_kind\": " << json_int_map(count_docs_by(docs, "kind")) << ",\n"
        << "  \"counts_by_doc_type\": " << json_int_map(count_docs_by(docs, "doc_type")) << ",\n"
        << "  \"counts_by_area\": " << json_int_map(count_docs_by(docs, "area")) << ",\n"
        << "  \"counts_by_status\": " << json_int_map(count_docs_by(docs, "status")) << ",\n"
        << "  \"agent_start_paths\": " << json_string_array(profile.agent_start_paths) << ",\n"
        << "  \"outputs\": [\n"
        << "    {\"name\":\"documents\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/documents.jsonl\",\"rows\":" << docs.size() << "},\n"
        << "    {\"name\":\"document-rows\",\"type\":\"json\",\"path\":\"docs/kb/generated/document-rows.json\",\"rows\":1},\n"
        << "    {\"name\":\"sections\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/sections.jsonl\",\"rows\":" << section_count << "},\n"
        << "    {\"name\":\"sections-index\",\"type\":\"tsv\",\"path\":\"docs/kb/generated/sections.index.tsv\",\"rows\":" << section_count << "},\n"
        << "    {\"name\":\"links\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/links.jsonl\",\"rows\":" << link_count << "},\n"
        << "    {\"name\":\"topics\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/topics.jsonl\",\"rows\":" << topic_count << "},\n"
        << (compact
            ? "    {\"name\":\"terms\",\"type\":\"shin.lexical.v1\",\"path\":\"docs/kb/generated/terms.lex\",\"rows\":" + std::to_string(term_count) + "},\n"
            : "    {\"name\":\"terms\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/terms.jsonl\",\"rows\":" + std::to_string(term_count) + "},\n"
              "    {\"name\":\"terms-index\",\"type\":\"tsv\",\"path\":\"docs/kb/generated/terms.index.tsv\",\"rows\":" + std::to_string(term_count) + "},\n")
        << "    {\"name\":\"decisions\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/decisions.jsonl\",\"rows\":" << decision_count << "},\n"
        << "    {\"name\":\"decisions-index\",\"type\":\"tsv\",\"path\":\"docs/kb/generated/decisions.index.tsv\",\"rows\":" << decision_count << "},\n"
        << "    {\"name\":\"contracts\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/contracts.jsonl\",\"rows\":" << contract_count << "},\n"
        << "    {\"name\":\"contracts-index\",\"type\":\"tsv\",\"path\":\"docs/kb/generated/contracts.index.tsv\",\"rows\":" << contract_count << "},\n"
        << "    {\"name\":\"runbooks\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/runbooks.jsonl\",\"rows\":" << runbook_count << "},\n"
        << "    {\"name\":\"runbooks-index\",\"type\":\"tsv\",\"path\":\"docs/kb/generated/runbooks.index.tsv\",\"rows\":" << runbook_count << "},\n"
        << "    {\"name\":\"facts\",\"type\":\"jsonl\",\"path\":\"docs/kb/generated/facts.jsonl\",\"rows\":" << fact_count << "},\n"
        << "    {\"name\":\"facts-index\",\"type\":\"tsv\",\"path\":\"docs/kb/generated/facts.index.tsv\",\"rows\":" << fact_count << "},\n"
        << "    {\"name\":\"manifest\",\"type\":\"json\",\"path\":\"docs/kb/generated/manifest.json\",\"rows\":1}\n"
        << "  ],\n"
        << "  \"generated_file_integrity\": {\"algorithm\":\"fnv1a64\",\"files\":[\n";
    std::size_t integrity_index = 0;
    for (const auto& [path, content] : generated_files) {
        out << "    {\"path\":" << json_string(path)
            << ",\"bytes\":" << content.size()
            << ",\"content_hash\":" << json_string(fnv1a64(content)) << "}";
        if (++integrity_index < generated_files.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]},\n"
        << "  \"sources\": [\n";
    for (std::size_t i = 0; i < docs.size(); ++i) {
        const SourceDoc& doc = docs[i];
        out << "    {\"path\":" << json_string(doc.path)
            << ",\"kind\":" << json_string(doc.kind)
            << ",\"doc_type\":" << json_string(doc.doc_type)
            << ",\"area\":" << json_string(doc.area)
            << ",\"status\":" << json_string(doc.status)
            << ",\"content_hash\":" << json_string(doc.content_hash)
            << (doc.quarantine_reason.empty() ? "" : ",\"quarantine_reason\":" + json_string(doc.quarantine_reason))
            << ",\"bytes\":" << doc.byte_count
            << ",\"line_count\":" << doc.line_count
            << "}";
        if (i + 1 < docs.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n"
        << "  \"broken_local_link_count\": " << broken_local_link_count << ",\n"
        << "  \"warnings\": [],\n"
        << "  \"errors\": []\n"
        << "}\n";
    return out.str();
}

static void append_generated_doc_rows(const Profile& profile,
                               const fs::path& root,
                               GeneratedRows& rows,
                               const SourceDoc& doc) {
    std::vector<SectionRow> doc_sections = build_sections(profile, doc, &rows.structured_data);
    rows.sections.insert(rows.sections.end(), doc_sections.begin(), doc_sections.end());
    std::vector<LinkRow> doc_links = build_links(profile, root, doc);
    rows.links.insert(rows.links.end(), doc_links.begin(), doc_links.end());
    DocumentFactBuildResult doc_facts = build_fact_rows(profile, doc);
    rows.structured_data.jsonl_candidate_rows += doc_facts.candidate_rows;
    rows.structured_data.jsonl_indexed_rows += doc_facts.indexed_rows;
    rows.structured_data.jsonl_capped_rows += doc_facts.capped_rows;
    if (!doc_facts.rows.empty()) {
        rows.facts_by_doc[doc.path] = doc_facts.rows;
        rows.facts.insert(rows.facts.end(), doc_facts.rows.begin(), doc_facts.rows.end());
    }
    for (const std::string& topic : doc.topics) {
        rows.topic_docs[topic].insert(doc.id);
    }
}

static void append_generated_source_fact_rows(const Profile& profile, const fs::path& root, GeneratedRows& rows) {
    rows.source_facts = build_source_fact_rows(profile, root);
    rows.facts.insert(rows.facts.end(), rows.source_facts.rows.begin(), rows.source_facts.rows.end());
    rows.links.insert(rows.links.end(), rows.source_facts.relations.begin(), rows.source_facts.relations.end());
}

static int count_broken_local_links(const std::vector<LinkRow>& links) {
    int count = 0;
    for (const LinkRow& link : links) {
        if (link.status == "broken") {
            ++count;
        }
    }
    return count;
}

static void validate_generated_evidence_provenance(const GeneratedRows& rows) {
    auto require_valid = [](const std::string& path,
                            const std::string& evidence_kind,
                            const std::string& verification_level) {
        if (!evidence_kind_valid(evidence_kind) ||
            !verification_level_valid(verification_level)) {
            throw std::logic_error("Generated row has invalid evidence provenance: " + path);
        }
    };
    for (const SourceDoc& doc : rows.docs) {
        require_valid(doc.path, doc.evidence_kind, doc.verification_level);
    }
    for (const SectionRow& section : rows.sections) {
        require_valid(section.path, section.evidence_kind, section.verification_level);
    }
    for (const FactRow& fact : rows.facts) {
        require_valid(fact.path, fact.evidence_kind, fact.verification_level);
    }
    for (const DecisionRow& decision : rows.decisions) {
        require_valid(decision.path, decision.evidence_kind, decision.verification_level);
    }
    for (const TypedReferenceRow& reference : rows.contracts) {
        require_valid(reference.path, reference.evidence_kind, reference.verification_level);
    }
    for (const TypedReferenceRow& reference : rows.runbooks) {
        require_valid(reference.path, reference.evidence_kind, reference.verification_level);
    }
}

static void sort_generated_rows(GeneratedRows& rows) {
    std::sort(rows.sections.begin(), rows.sections.end(), [](const SectionRow& a, const SectionRow& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        if (a.start_line != b.start_line) {
            return a.start_line < b.start_line;
        }
        if (a.end_line != b.end_line) {
            return a.end_line < b.end_line;
        }
        if (a.row_type != b.row_type) {
            return a.row_type < b.row_type;
        }
        if (a.chunk_index != b.chunk_index) {
            return a.chunk_index < b.chunk_index;
        }
        if (a.anchor != b.anchor) {
            return a.anchor < b.anchor;
        }
        if (a.level != b.level) {
            return a.level < b.level;
        }
        if (a.heading != b.heading) {
            return a.heading < b.heading;
        }
        if (a.preview != b.preview) {
            return a.preview < b.preview;
        }
        if (a.indexed_text != b.indexed_text) {
            return a.indexed_text < b.indexed_text;
        }
        if (a.indexed_text_omitted_bytes != b.indexed_text_omitted_bytes) {
            return a.indexed_text_omitted_bytes < b.indexed_text_omitted_bytes;
        }
        if (a.evidence_kind != b.evidence_kind) {
            return a.evidence_kind < b.evidence_kind;
        }
        if (a.verification_level != b.verification_level) {
            return a.verification_level < b.verification_level;
        }
        return a.topics < b.topics;
    });
    std::sort(rows.links.begin(), rows.links.end(), [](const LinkRow& a, const LinkRow& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        if (a.target != b.target) {
            return a.target < b.target;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        if (a.text != b.text) {
            return a.text < b.text;
        }
        return a.status < b.status;
    });
    std::sort(rows.facts.begin(), rows.facts.end(), [](const FactRow& a, const FactRow& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        if (a.record_type != b.record_type) {
            return a.record_type < b.record_type;
        }
        if (a.status != b.status) {
            return a.status < b.status;
        }
        if (a.area != b.area) {
            return a.area < b.area;
        }
        if (a.authority_level != b.authority_level) {
            return a.authority_level < b.authority_level;
        }
        if (a.evidence_kind != b.evidence_kind) {
            return a.evidence_kind < b.evidence_kind;
        }
        if (a.verification_level != b.verification_level) {
            return a.verification_level < b.verification_level;
        }
        if (a.title != b.title) {
            return a.title < b.title;
        }
        if (a.summary != b.summary) {
            return a.summary < b.summary;
        }
        if (a.source != b.source) {
            return a.source < b.source;
        }
        return a.topics < b.topics;
    });
    std::sort(rows.decisions.begin(), rows.decisions.end(), [](const DecisionRow& a, const DecisionRow& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        if (a.status != b.status) {
            return a.status < b.status;
        }
        if (a.authority_level != b.authority_level) {
            return a.authority_level < b.authority_level;
        }
        if (a.evidence_kind != b.evidence_kind) {
            return a.evidence_kind < b.evidence_kind;
        }
        if (a.verification_level != b.verification_level) {
            return a.verification_level < b.verification_level;
        }
        if (a.title != b.title) {
            return a.title < b.title;
        }
        return a.summary < b.summary;
    });
    std::sort(rows.contracts.begin(), rows.contracts.end(), [](const TypedReferenceRow& a, const TypedReferenceRow& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        if (a.type != b.type) {
            return a.type < b.type;
        }
        if (a.status != b.status) {
            return a.status < b.status;
        }
        if (a.area != b.area) {
            return a.area < b.area;
        }
        if (a.authority_level != b.authority_level) {
            return a.authority_level < b.authority_level;
        }
        if (a.evidence_kind != b.evidence_kind) {
            return a.evidence_kind < b.evidence_kind;
        }
        if (a.verification_level != b.verification_level) {
            return a.verification_level < b.verification_level;
        }
        if (a.title != b.title) {
            return a.title < b.title;
        }
        return a.summary < b.summary;
    });
    std::sort(rows.runbooks.begin(), rows.runbooks.end(), [](const TypedReferenceRow& a, const TypedReferenceRow& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.line != b.line) {
            return a.line < b.line;
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        if (a.type != b.type) {
            return a.type < b.type;
        }
        if (a.status != b.status) {
            return a.status < b.status;
        }
        if (a.area != b.area) {
            return a.area < b.area;
        }
        if (a.authority_level != b.authority_level) {
            return a.authority_level < b.authority_level;
        }
        if (a.evidence_kind != b.evidence_kind) {
            return a.evidence_kind < b.evidence_kind;
        }
        if (a.verification_level != b.verification_level) {
            return a.verification_level < b.verification_level;
        }
        if (a.title != b.title) {
            return a.title < b.title;
        }
        return a.summary < b.summary;
    });
}

static GeneratedRows build_generated_rows(const Profile& profile, const fs::path& root, BuildPhaseTimings* timings = nullptr) {
    GeneratedRows rows;
    auto phase_start = SteadyClock::now();
    rows.docs = read_sources(profile, root);
    for (auto& doc : rows.docs) {
        const auto secret = source_range_secret(doc.content);
        if (!secret.found) continue;
        doc.quarantine_reason = secret.kind;
        doc.title = "Quarantined source";
        doc.summary = "Source content withheld after secret detection.";
        doc.doc_type = "quarantined";
        doc.status = "quarantined";
        doc.topics = {profile.default_topic.empty() ? "quarantine" : profile.default_topic};
        doc.content.clear();
        doc.lines.clear();
    }
    if (timings != nullptr) {
        timings->read_sources_ms += elapsed_ms_since(phase_start);
        timings->peak_rss_bytes["read_sources"] = process_peak_rss_bytes();
    }

    phase_start = SteadyClock::now();
    for (const SourceDoc& doc : rows.docs) {
        if (!doc.quarantine_reason.empty()) continue;
        append_generated_doc_rows(profile, root, rows, doc);
    }
    if (timings != nullptr) {
        timings->document_rows_ms += elapsed_ms_since(phase_start);
        timings->peak_rss_bytes["document_rows"] = process_peak_rss_bytes();
    }

    phase_start = SteadyClock::now();
    append_generated_source_fact_rows(profile, root, rows);
    if (timings != nullptr) {
        timings->source_facts_ms += elapsed_ms_since(phase_start);
        timings->peak_rss_bytes["source_facts"] = process_peak_rss_bytes();
    }

    phase_start = SteadyClock::now();
    rows.decisions = build_decision_rows(rows.docs, rows.facts_by_doc);
    rows.contracts =
        build_typed_reference_rows(rows.docs,
                                   {"contract", "openapi-contract", "asyncapi-contract", "schema", "manifest"},
                                   "contract");
    rows.runbooks = build_typed_reference_rows(rows.docs, {"runbook", "deployment", "operations"}, "runbook");
    append_markdown_authority_rows(rows.docs, rows.sections, rows.decisions, rows.contracts, rows.runbooks);
    validate_generated_evidence_provenance(rows);
    if (timings != nullptr) {
        timings->authority_rows_ms += elapsed_ms_since(phase_start);
        timings->peak_rss_bytes["authority_rows"] = process_peak_rss_bytes();
    }

    phase_start = SteadyClock::now();
    sort_generated_rows(rows);
    if (timings != nullptr) {
        timings->sort_rows_ms += elapsed_ms_since(phase_start);
        timings->peak_rss_bytes["sort_rows"] = process_peak_rss_bytes();
    }

    phase_start = SteadyClock::now();
    rows.broken_local_link_count = count_broken_local_links(rows.links);
    if (timings != nullptr) {
        timings->final_counts_ms += elapsed_ms_since(phase_start);
    }
    return rows;
}

SourceScanCorpus build_source_scan_corpus(const Profile& profile, const fs::path& root) {
    GeneratedRows rows = build_generated_rows(profile, root);
    SourceScanCorpus corpus;
    corpus.documents = std::move(rows.docs);
    corpus.source_facts = std::move(rows.source_facts);
    corpus.sections = std::move(rows.sections);
    corpus.facts = std::move(rows.facts);
    corpus.facts.reserve(corpus.facts.size() + rows.decisions.size() +
                         rows.contracts.size() + rows.runbooks.size());
    for (const auto& row : rows.decisions) corpus.facts.push_back(query_fact_row(row));
    for (const auto& row : rows.contracts) corpus.facts.push_back(query_fact_row(row));
    for (const auto& row : rows.runbooks) corpus.facts.push_back(query_fact_row(row));
    return corpus;
}

static GeneratedOutput render_generated_output(const Profile& profile,
                                        const GeneratedRows& rows,
                                        BuildPhaseTimings* timings, bool compact) {
    GeneratedOutput generated;
    generated.document_count = static_cast<int>(rows.docs.size());
    generated.section_count = static_cast<int>(rows.sections.size());
    generated.link_count = static_cast<int>(rows.links.size());
    generated.topic_count = static_cast<int>(rows.topic_docs.size());
    auto phase_start = SteadyClock::now();
    // Release the posting map before rendering the other full-corpus files.
    auto term_index = [&] {
        auto term_posting_build = build_term_postings(rows, timings);
        generated.term_posting_workers = term_posting_build.worker_count;
        if (timings != nullptr) timings->term_postings_ms += elapsed_ms_since(phase_start);
        phase_start = SteadyClock::now();
        RenderedTermIndex rendered;
        if (compact) {
            rendered.postings_jsonl = encode_compact_terms(term_posting_build.postings,
                {static_cast<std::uint32_t>(rows.sections.size()), static_cast<std::uint32_t>(rows.facts.size()),
                 static_cast<std::uint32_t>(rows.decisions.size()), static_cast<std::uint32_t>(rows.contracts.size()),
                 static_cast<std::uint32_t>(rows.runbooks.size())});
            rendered.term_count = static_cast<int>(term_posting_build.postings.size());
        } else rendered = render_term_index(term_posting_build.postings);
        if (timings != nullptr) {
            timings->term_render_ms += elapsed_ms_since(phase_start);
            timings->peak_rss_bytes["term_render"] = process_peak_rss_bytes();
        }
        return rendered;
    }();

    phase_start = SteadyClock::now();
    generated.term_count = term_index.term_count;
    generated.decision_count = static_cast<int>(rows.decisions.size());
    generated.contract_count = static_cast<int>(rows.contracts.size());
    generated.runbook_count = static_cast<int>(rows.runbooks.size());
    generated.fact_count = static_cast<int>(rows.facts.size());
    generated.source_fact_candidate_file_count = rows.source_facts.candidate_file_count;
    generated.source_fact_scanned_file_count = rows.source_facts.scanned_file_count;
    generated.source_fact_skipped_large_file_count = rows.source_facts.skipped_large_file_count;
    generated.source_fact_capped_file_count = rows.source_facts.capped_file_count;
    generated.structured_data = rows.structured_data;
    generated.broken_local_link_count = rows.broken_local_link_count;
    generated.files[(kGeneratedDir / "documents.jsonl").generic_string()] = documents_jsonl(rows.docs);
    generated.files[(kGeneratedDir / "document-rows.json").generic_string()] = render_document_row_index(rows);
    generated.files[(kGeneratedDir / "sections.jsonl").generic_string()] = sections_jsonl(rows.sections);
    generated.files[(kGeneratedDir / "links.jsonl").generic_string()] = links_jsonl(rows.links);
    generated.files[(kGeneratedDir / "topics.jsonl").generic_string()] = topics_jsonl(profile, rows.topic_docs);
    if (compact) generated.files[(kGeneratedDir / "terms.lex").generic_string()] = std::move(term_index.postings_jsonl);
    else {
        generated.files[(kGeneratedDir / "terms.jsonl").generic_string()] = std::move(term_index.postings_jsonl);
        generated.files[(kGeneratedDir / "terms.index.tsv").generic_string()] = std::move(term_index.offsets_tsv);
    }
    generated.files[(kGeneratedDir / "decisions.jsonl").generic_string()] = decisions_jsonl(rows.decisions);
    generated.files[(kGeneratedDir / "contracts.jsonl").generic_string()] = typed_references_jsonl(rows.contracts);
    generated.files[(kGeneratedDir / "runbooks.jsonl").generic_string()] = typed_references_jsonl(rows.runbooks);
    generated.files[(kGeneratedDir / "facts.jsonl").generic_string()] = facts_jsonl(rows.facts);
    if (timings != nullptr) {
        timings->json_render_ms += elapsed_ms_since(phase_start);
    }

    phase_start = SteadyClock::now();
    for (const auto* name : {"sections", "decisions", "contracts", "runbooks", "facts"}) {
        const auto& content = generated.files.at((kGeneratedDir / (std::string(name) + ".jsonl")).generic_string());
        generated.files[(kGeneratedDir / (std::string(name) + ".index.tsv")).generic_string()] = row_offsets_tsv(content);
    }
    if (timings != nullptr) {
        timings->row_index_render_ms += elapsed_ms_since(phase_start);
    }

    phase_start = SteadyClock::now();
    generated.files[(kGeneratedDir / "manifest.json").generic_string()] =
        manifest_json(profile, rows.docs, generated.section_count, generated.link_count, generated.topic_count,
                      generated.term_count, generated.decision_count, generated.contract_count,
                      generated.runbook_count, generated.fact_count, rows.source_facts, rows.structured_data,
                      generated.files,
                      generated.broken_local_link_count, compact);
    if (timings != nullptr) {
        timings->json_render_ms += elapsed_ms_since(phase_start);
    }
    for (const auto& [path, content] : generated.files) {
        (void)path;
        generated.generated_bytes += static_cast<long long>(content.size());
    }
    if (timings) timings->peak_rss_bytes["render_complete"] = process_peak_rss_bytes();
    return generated;
}

static thread_local std::uint64_t generation_executions = 0;

std::uint64_t generation_execution_count() { return generation_executions; }

GeneratedOutput generate_outputs(const Profile& profile, const fs::path& root, BuildPhaseTimings* timings, bool compact_terms) {
    ++generation_executions;
    const auto total_start = SteadyClock::now();
    auto rows = build_generated_rows(profile, root, timings);
    // Aggregated rows already own these payloads. Rendering needs only the
    // source inventory/counters; source-scan callers keep their original copies.
    std::vector<FactRow>{}.swap(rows.source_facts.rows);
    std::vector<LinkRow>{}.swap(rows.source_facts.relations);
    rows.facts_by_doc.clear();
    GeneratedOutput output = render_generated_output(profile, rows, timings, compact_terms);
    if (timings != nullptr) {
        timings->total_generate_ms = elapsed_ms_since(total_start);
    }
    return output;
}

static void write_generated_output_files(const RuntimeOptions& options, const GeneratedOutput& generated) {
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
    prepare_freshness_cache(options);
}

static Json generated_counts_json(const GeneratedOutput& generated) {
    return {
        {"documents", generated.document_count},
        {"sections", generated.section_count},
        {"links", generated.link_count},
        {"topics", generated.topic_count},
        {"terms", generated.term_count},
        {"decisions", generated.decision_count},
        {"contracts", generated.contract_count},
        {"runbooks", generated.runbook_count},
        {"facts", generated.fact_count},
        {"json_key_candidate_sections", generated.structured_data.json_key_candidate_sections},
        {"json_key_indexed_sections", generated.structured_data.json_key_indexed_sections},
        {"json_key_capped_sections", generated.structured_data.json_key_capped_sections},
        {"jsonl_candidate_rows", generated.structured_data.jsonl_candidate_rows},
        {"jsonl_indexed_rows", generated.structured_data.jsonl_indexed_rows},
        {"jsonl_capped_rows", generated.structured_data.jsonl_capped_rows},
        {"generated_bytes", generated.generated_bytes}
    };
}

struct GeneratedDrift {
    std::vector<std::string> missing_files;
    std::vector<std::string> stale_files;
    int broken_local_links = 0;

    bool current(bool diagnostic_broken_links) const noexcept {
        return missing_files.empty() && stale_files.empty() &&
               (diagnostic_broken_links || broken_local_links == 0);
    }
};

struct GeneratedFileComparison {
    bool equal = true;
    std::uintmax_t bytes = 0;
};

static GeneratedFileComparison compare_generated_file(const fs::path& path, std::string_view expected) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    std::array<char, 65536> buffer{};
    GeneratedFileComparison result;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (result.equal && (result.bytes > expected.size() || count > expected.size() - result.bytes ||
            std::string_view(buffer.data(), count) != expected.substr(static_cast<std::size_t>(result.bytes), count)))
            result.equal = false;
        result.bytes += count;
    }
    if (!input.eof() || input.bad()) throw std::runtime_error("Cannot read " + path.string());
    result.equal = result.equal && result.bytes == expected.size();
    return result;
}

static GeneratedDrift inspect_generated_output_drift(const RuntimeOptions& options, const GeneratedOutput& generated) {
    std::set<std::string> expected_files;
    for (const auto& [path, content] : generated.files) {
        (void)content;
        expected_files.insert(path);
    }
    verify_snapshot_filesystem(options.out_root, expected_files);
    GeneratedDrift drift;
    drift.broken_local_links = generated.broken_local_link_count;
    for (const auto& [path, expected] : generated.files) {
        fs::path full_path = options.out_root / path;
        if (!fs::exists(full_path)) {
            drift.missing_files.push_back(path);
            continue;
        }
        if (fs::file_size(full_path) != expected.size()) {
            drift.stale_files.push_back(path);
            continue;
        }
        if (!compare_generated_file(full_path, expected).equal) {
            drift.stale_files.push_back(path);
        }
    }
    return drift;
}

static void emit_build_result(const RuntimeOptions& options, const GeneratedOutput& generated, bool published) {
    const bool diagnostic_broken_links = options.profile.bootstrap_only;
    const bool broken_links_block = generated.broken_local_link_count > 0 && !diagnostic_broken_links;
    const bool ok = published && !broken_links_block;
    Json result = make_result_document("shin.build.v1", "build", ok);
    result["profile"] = options.profile.id;
    result["root"] = options.root.generic_string();
    result["out_root"] = options.out_root.generic_string();
    result["published"] = published;
    result["local_link_policy"] = diagnostic_broken_links ? "diagnostic" : "reject";
    result["counts"] = generated_counts_json(generated);
    result["issues"] = Json::array();
    if (generated.broken_local_link_count > 0) {
        result["issues"].push_back({
            {"code", "broken-local-links"},
            {"count", generated.broken_local_link_count},
            {"severity", diagnostic_broken_links ? "warning" : "error"},
            {"summary", diagnostic_broken_links
                            ? "Bootstrap output was published with broken local links recorded as diagnostics."
                            : "Generated output was rejected; the previous healthy snapshot remains active."}
        });
    }
    emit_json_document(std::cout, result);
}

static void emit_check_result(const RuntimeOptions& options,
                       const GeneratedOutput& generated,
                       const GeneratedDrift& drift) {
    const bool diagnostic_broken_links = options.profile.bootstrap_only;
    const bool current = drift.current(diagnostic_broken_links);
    Json result = make_result_document("shin.check.v1", "check", current);
    result["profile"] = options.profile.id;
    result["root"] = options.root.generic_string();
    result["out_root"] = options.out_root.generic_string();
    result["current"] = current;
    result["local_link_policy"] = diagnostic_broken_links ? "diagnostic" : "reject";
    result["counts"] = generated_counts_json(generated);
    result["drift"] = {
        {"missing_files", drift.missing_files},
        {"stale_files", drift.stale_files},
        {"broken_local_links", drift.broken_local_links}
    };
    emit_json_document(std::cout, result);
}

int run_build(const RuntimeOptions& options) {
    shin::generated_io::IndexLock writer_lock(options.out_root,
                                               shin::generated_io::LockMode::exclusive,
                                               shin::generated_io::LockTarget::writer);
    shin::generated_io::recover_snapshot(options.out_root, kGeneratedDir);
    GeneratedOutput generated = generate_outputs(options.profile, options.root);
    if (generated.broken_local_link_count > 0 && !options.profile.bootstrap_only) {
        emit_build_result(options, generated, false);
        return 1;
    }
    write_generated_output_files(options, generated);
    emit_build_result(options, generated, true);
    return 0;
}

int run_check(const RuntimeOptions& options) {
    if (shin::generated_io::publication_in_progress(options.out_root)) {
        throw_index_error("publication-incomplete",
                          "Generated snapshot publication is incomplete and requires writer recovery.");
    }
    const auto expected_generation = validation_generated_output(options, compact_lexical_index(options));
    const auto& generated = *expected_generation;
    const GeneratedDrift drift = inspect_generated_output_drift(options, generated);
    emit_check_result(options, generated, drift);
    return drift.current(options.profile.bootstrap_only) ? 0 : 1;
}

long long file_size_or_zero(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<long long>(size);
}

static void emit_vacuum_summary_json(std::ostream& out, const VacuumSummary& summary) {
    out << "{"
        << "\"expected_files\":" << summary.expected_files
        << ",\"existing_files\":" << summary.existing_files
        << ",\"current_files\":" << summary.current_files
        << ",\"missing_files\":" << summary.missing_files
        << ",\"stale_files\":" << summary.stale_files
        << ",\"orphan_files\":" << summary.orphan_files
        << ",\"written_files\":" << summary.written_files
        << ",\"removed_files\":" << summary.removed_files
        << ",\"unchanged_files\":" << summary.unchanged_files
        << ",\"broken_local_links\":" << summary.broken_local_links
        << ",\"changes_needed\":" << (summary.changes_needed ? "true" : "false")
        << ",\"changes_remaining\":" << (summary.changes_remaining ? "true" : "false")
        << "}";
}

static void emit_vacuum_action_json(std::ostream& out, const VacuumAction& action) {
    out << "{"
        << "\"action\":" << json_string(action.action)
        << ",\"path\":" << json_string(action.path)
        << ",\"status\":" << json_string(action.status)
        << ",\"bytes_before\":" << action.bytes_before
        << ",\"bytes_after\":" << action.bytes_after
        << ",\"summary\":" << json_string(action.summary)
        << "}";
}

static std::vector<fs::directory_entry> sorted_generated_dir_entries(const fs::path& generated_root) {
    std::vector<fs::directory_entry> entries;
    std::error_code status_ec;
    const fs::file_status status = fs::symlink_status(generated_root, status_ec);
    if (status_ec || !fs::exists(status)) {
        return entries;
    }
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
        throw std::runtime_error("Generated KB path is not a directory: " + generated_root.string());
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(generated_root)) {
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().generic_string() < b.path().generic_string();
    });
    return entries;
}

static std::string generated_entry_key(const fs::directory_entry& entry) {
    return (kGeneratedDir / entry.path().filename()).generic_string();
}

static VacuumRequest parse_vacuum_request(const std::vector<std::string>& args) {
    VacuumRequest request;
    for (const std::string& arg : args) {
        if (arg == "--apply") {
            request.apply = true;
        } else if (arg == "--strict") {
            request.strict = true;
        } else {
            throw_usage_error("unsupported-option", "vacuum only accepts --apply and --strict.");
        }
    }
    return request;
}

static bool vacuum_can_publish(const VacuumResult& result) {
    return result.request.apply && result.summary.broken_local_links == 0;
}

static void write_missing_generated_file(VacuumResult& result,
                                  const std::string& path,
                                  long long expected_size) {
    ++result.summary.missing_files;
    result.summary.changes_needed = true;
    if (vacuum_can_publish(result)) {
        ++result.summary.written_files;
        result.actions.push_back({"write", path, "written", "Generated file was missing and has been written.",
                                  0, expected_size});
    } else {
        result.actions.push_back({"write", path, "planned", "Generated file is missing.", 0, expected_size});
    }
}

static void replace_symlink_generated_file(VacuumResult& result,
                                    const std::string& path,
                                    long long expected_size,
                                    const fs::path& full_path) {
    ++result.summary.stale_files;
    result.summary.changes_needed = true;
    const long long before_size = file_size_or_zero(full_path);
    if (vacuum_can_publish(result)) {
        ++result.summary.written_files;
        result.actions.push_back({"replace", path, "written",
                                  "Generated path was a symlink and has been replaced with a regular file.",
                                  before_size, expected_size});
    } else {
        result.actions.push_back({"replace", path, "planned",
                                  "Generated path is a symlink and must be replaced with a regular file.",
                                  before_size, expected_size});
    }
}

static void replace_non_regular_generated_entry(VacuumResult& result,
                                         const std::string& path,
                                         const fs::path& full_path,
                                         long long expected_size) {
    ++result.summary.stale_files;
    result.summary.changes_needed = true;
    const long long before_size = file_size_or_zero(full_path);
    if (vacuum_can_publish(result)) {
        ++result.summary.written_files;
        result.actions.push_back({"replace", path, "written",
                                  "Non-regular generated entry was replaced by the canonical snapshot.",
                                  before_size, expected_size});
    } else {
        result.actions.push_back({"replace", path, "planned",
                                  "Non-regular generated entry must be replaced by the canonical snapshot.",
                                  before_size, expected_size});
    }
}

static void rewrite_stale_generated_file(VacuumResult& result,
                                  const std::string& path,
                                  long long expected_size,
                                  long long actual_size) {
    ++result.summary.stale_files;
    result.summary.changes_needed = true;
    if (vacuum_can_publish(result)) {
        ++result.summary.written_files;
        result.actions.push_back({"rewrite", path, "written", "Generated file was stale and has been rewritten.",
                                  actual_size, expected_size});
    } else {
        result.actions.push_back({"rewrite", path, "planned", "Generated file is stale.",
                                  actual_size, expected_size});
    }
}

static void inspect_expected_generated_file(const RuntimeOptions& options,
                                     VacuumResult& result,
                                     const std::string& path,
                                     const std::string& expected) {
    const fs::path full_path = options.out_root / path;
    const long long expected_size = static_cast<long long>(expected.size());
    std::error_code status_ec;
    const fs::file_status link_status = fs::symlink_status(full_path, status_ec);
    const bool path_exists = !status_ec && fs::exists(link_status);
    if (!path_exists) {
        write_missing_generated_file(result, path, expected_size);
        return;
    }
    if (fs::is_symlink(link_status)) {
        replace_symlink_generated_file(result, path, expected_size, full_path);
        return;
    }
    if (!fs::is_regular_file(link_status)) {
        replace_non_regular_generated_entry(result, path, full_path, expected_size);
        return;
    }
    const auto comparison = compare_generated_file(full_path, expected);
    if (!comparison.equal) {
        rewrite_stale_generated_file(result, path, expected_size, static_cast<long long>(comparison.bytes));
        return;
    }
    ++result.summary.current_files;
    ++result.summary.unchanged_files;
    result.actions.push_back({"keep", path, "unchanged", "Generated file already matches canonical output.",
                              expected_size, expected_size});
}

static std::set<std::string> inspect_expected_generated_files(const RuntimeOptions& options,
                                                       VacuumResult& result,
                                                       const GeneratedOutput& generated) {
    std::set<std::string> expected_paths;
    for (const auto& [path, expected] : generated.files) {
        expected_paths.insert(path);
        inspect_expected_generated_file(options, result, path, expected);
    }
    return expected_paths;
}

static void inspect_orphan_generated_entry(VacuumResult& result,
                                    const fs::directory_entry& entry,
                                    const std::set<std::string>& expected_paths) {
    const std::string key = generated_entry_key(entry);
    if (expected_paths.find(key) != expected_paths.end()) {
        if (entry.is_regular_file()) {
            ++result.summary.existing_files;
        }
        return;
    }
    ++result.summary.existing_files;
    ++result.summary.orphan_files;
    result.summary.changes_needed = true;
    const long long size = file_size_or_zero(entry.path());
    if (vacuum_can_publish(result)) {
        ++result.summary.removed_files;
        result.actions.push_back({"remove", key, "removed",
                                  "Orphan generated entry was removed by the canonical snapshot.", size, 0});
    } else {
        result.actions.push_back({"remove", key, "planned",
                                  "Orphan generated entry is not part of the canonical snapshot.", size, 0});
    }
}

static void inspect_orphan_generated_entries(VacuumResult& result, const std::set<std::string>& expected_paths) {
    for (const fs::directory_entry& entry : sorted_generated_dir_entries(result.generated_root)) {
        inspect_orphan_generated_entry(result, entry, expected_paths);
    }
}

static VacuumResult inspect_generated_vacuum(const RuntimeOptions& options,
                                      const VacuumRequest& request,
                                      const GeneratedOutput& generated) {
    VacuumResult result;
    result.request = request;
    result.generated_root = options.out_root / kGeneratedDir;
    result.summary.expected_files = static_cast<int>(generated.files.size());
    result.summary.broken_local_links = generated.broken_local_link_count;
    const std::set<std::string> expected_paths = inspect_expected_generated_files(options, result, generated);
    inspect_orphan_generated_entries(result, expected_paths);
    result.summary.changes_remaining = result.summary.changes_needed && !vacuum_can_publish(result);
    return result;
}

static bool vacuum_result_ok(const VacuumResult& result) {
    return !result.summary.changes_remaining && result.summary.broken_local_links == 0;
}

VacuumAssessment inspect_generated_kb_vacuum(const RuntimeOptions& options) {
    if (shin::generated_io::publication_in_progress(options.out_root)) {
        throw_index_error("publication-incomplete",
                          "Generated snapshot publication is incomplete and requires writer recovery.");
    }
    const GeneratedOutput generated = generate_outputs(options.profile, options.root);
    const VacuumResult vacuum = inspect_generated_vacuum(options, {false, true}, generated);
    const auto manifest = generated.files.find((kGeneratedDir / "manifest.json").generic_string());
    if (manifest == generated.files.end()) {
        throw std::runtime_error("Canonical generated output is missing manifest.json.");
    }
    return {vacuum.summary, fnv1a64(manifest->second)};
}

static void emit_vacuum_result_json(const RuntimeOptions& options, const VacuumResult& result) {
    const bool ok = vacuum_result_ok(result);
    std::cout << "{\n"
              << "  \"schema_version\": \"shin.vacuum.v1\",\n"
              << "  \"tool\": \"shin-context\",\n"
              << "  \"command\": \"vacuum\",\n"
              << "  \"profile\": " << json_string(options.profile.id) << ",\n"
              << "  \"root\": " << json_string(options.root.generic_string()) << ",\n"
              << "  \"out_root\": " << json_string(options.out_root.generic_string()) << ",\n"
              << "  \"generated_root\": " << json_string(result.generated_root.generic_string()) << ",\n"
              << "  \"apply\": " << (result.request.apply ? "true" : "false") << ",\n"
              << "  \"strict\": " << (result.request.strict ? "true" : "false") << ",\n"
              << "  \"ok\": " << (ok ? "true" : "false") << ",\n"
              << "  \"summary\": ";
    emit_vacuum_summary_json(std::cout, result.summary);
    std::cout << ",\n"
              << "  \"actions\": [\n";
    for (std::size_t i = 0; i < result.actions.size(); ++i) {
        std::cout << "    ";
        emit_vacuum_action_json(std::cout, result.actions[i]);
        if (i + 1 < result.actions.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
}

int run_vacuum(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const VacuumRequest request = parse_vacuum_request(args);
    std::unique_ptr<shin::generated_io::IndexLock> writer_lock;
    if (request.apply) {
        writer_lock = std::make_unique<shin::generated_io::IndexLock>(
            options.out_root,
            shin::generated_io::LockMode::exclusive,
            shin::generated_io::LockTarget::writer);
        shin::generated_io::recover_snapshot(options.out_root, kGeneratedDir);
    }
    const auto expected_generation = request.apply
        ? std::make_shared<const GeneratedOutput>(generate_outputs(options.profile, options.root))
        : validation_generated_output(options);
    const auto& generated = *expected_generation;
    VacuumResult result = inspect_generated_vacuum(options, request, generated);
    if (vacuum_can_publish(result) && result.summary.changes_needed) {
        write_generated_output_files(options, generated);
    }
    emit_vacuum_result_json(options, result);
    const bool strict_failure = result.request.strict && !vacuum_result_ok(result);
    return strict_failure ? 1 : 0;
}

} // namespace shin::context
