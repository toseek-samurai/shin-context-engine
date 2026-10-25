#include "field_preparation_experiment.h"

#include <algorithm>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace shin::context::experiment {
struct FieldPreparationEntry {
    enum class Kind { section, fact } kind;
    std::string key;
    SectionRow section;
    FactRow fact;
    std::vector<FieldMatchInput> fields;
    std::size_t payload = 0;
    bool cached = false;

    FieldPreparationEntry(const SectionRow& source, std::string identity_key)
        : kind(Kind::section), key(std::move(identity_key)), section(source),
          fields(section_match_fields(section)) {}
    FieldPreparationEntry(const FactRow& source, std::string identity_key)
        : kind(Kind::fact), key(std::move(identity_key)), fact(source),
          fields(fact_match_fields(fact)) {}
    std::size_t estimate() const;
    bool same(const SectionRow& value) const { return kind == Kind::section && section == value; }
    bool same(const FactRow& value) const { return kind == Kind::fact && fact == value; }
};
struct FieldPreparationState {
    bool enabled = false;
    std::size_t limit = 32U * 1024U * 1024U;
    std::size_t bytes = 0;
    std::size_t hits = 0, misses = 0, invalidations = 0, evictions = 0;
    std::size_t lazy_growth = 0, transient_leases = 0, active_transient_leases = 0;
    std::list<std::shared_ptr<FieldPreparationEntry>> lru;
    std::unordered_map<std::string, std::list<std::shared_ptr<FieldPreparationEntry>>::iterator> lookup;
};

namespace {
constexpr std::size_t kEntryLimit = 16384;
// All accesses and lease mutations are serialized within one retrieval lane.
thread_local std::shared_ptr<FieldPreparationState> state = std::make_shared<FieldPreparationState>();

void append_key_part(std::string& key, std::string_view part) {
    key += std::to_string(part.size());
    key.push_back(':');
    key.append(part);
    key.push_back('|');
}
void append_key_part(std::string& key, int part) { append_key_part(key, std::to_string(part)); }
std::string identity(const SectionRow& row) {
    std::string key("section|");
    append_key_part(key, row.path);
    append_key_part(key, row.document_id);
    append_key_part(key, row.anchor);
    append_key_part(key, row.line);
    append_key_part(key, row.row_type);
    return key;
}
std::string identity(const FactRow& row) {
    std::string key("fact|");
    append_key_part(key, row.path);
    append_key_part(key, row.id);
    append_key_part(key, row.record_type);
    append_key_part(key, row.line);
    return key;
}
std::size_t string_payload(const std::string& value) { return value.capacity() + 1; }
std::size_t strings_payload(const std::vector<std::string>& values) {
    std::size_t bytes = values.capacity() * sizeof(std::string);
    for (const auto& value : values) bytes += string_payload(value);
    return bytes;
}
std::size_t row_payload(const SectionRow& row) {
    return string_payload(row.document_id) + string_payload(row.path) +
        string_payload(row.status) + string_payload(row.authority_level) +
        string_payload(row.heading) + string_payload(row.anchor) + string_payload(row.preview) +
        string_payload(row.indexed_text) + strings_payload(row.topics) +
        string_payload(row.row_type) + string_payload(row.evidence_kind) +
        string_payload(row.verification_level);
}
std::size_t row_payload(const FactRow& row) {
    return string_payload(row.id) + string_payload(row.record_type) +
        string_payload(row.path) + string_payload(row.status) + string_payload(row.area) +
        string_payload(row.title) + string_payload(row.summary) + string_payload(row.source) +
        string_payload(row.authority_level) + strings_payload(row.topics) +
        strings_payload(row.identifier_terms) + string_payload(row.evidence_kind) +
        string_payload(row.verification_level) + string_payload(row.symbol_name) +
        string_payload(row.span_hash) + string_payload(row.range_origin);
}
std::size_t fields_payload(const std::vector<FieldMatchInput>& fields) {
    std::size_t bytes = fields.capacity() * sizeof(FieldMatchInput);
    for (const auto& field : fields) {
        bytes += field.lexical_terms.capacity() * sizeof(std::string);
        for (const auto& term : field.lexical_terms) bytes += string_payload(term);
        bytes += string_payload(field.normalized_text);
    }
    return bytes;
}
void erase_entry(const std::shared_ptr<FieldPreparationState>& owner,
                 std::list<std::shared_ptr<FieldPreparationEntry>>::iterator it, bool eviction) noexcept {
    auto& entry = *it;
    owner->bytes -= entry->payload;
    owner->lookup.erase(entry->key);
    entry->cached = false;
    owner->lru.erase(it);
    if (eviction) ++owner->evictions;
}
void trim(const std::shared_ptr<FieldPreparationState>& owner) noexcept {
    while (!owner->lru.empty() &&
           (owner->bytes > owner->limit || owner->lru.size() > kEntryLimit)) {
        erase_entry(owner, std::prev(owner->lru.end()), true);
    }
}
void reconcile(const std::shared_ptr<FieldPreparationState>& owner,
               const std::shared_ptr<FieldPreparationEntry>& entry) noexcept {
    if (!entry->cached || !owner->enabled) return;
    const auto found = owner->lookup.find(entry->key);
    if (found == owner->lookup.end() || *found->second != entry) return;
    const std::size_t updated = entry->estimate();
    const std::size_t previous = entry->payload;
    if (updated > previous) ++owner->lazy_growth;
    if (updated >= previous) owner->bytes += updated - previous;
    else owner->bytes -= previous - updated;
    entry->payload = updated;
    trim(owner);
}
template<class Row>
PreparedFieldsLease prepared(const Row& row) {
    auto owner = state;
    if (!owner->enabled) return {};
    auto key = identity(row);
    if (const auto found = owner->lookup.find(key); found != owner->lookup.end()) {
        auto entry = *found->second;
        if (entry->same(row)) {
            ++owner->hits;
            owner->lru.splice(owner->lru.begin(), owner->lru, found->second);
            return PreparedFieldsLease(std::move(entry), std::move(owner), false);
        }
        ++owner->invalidations;
        erase_entry(owner, found->second, false);
    }
    ++owner->misses;
    auto entry = std::make_shared<FieldPreparationEntry>(row, std::move(key));
    entry->payload = entry->estimate();
    if (entry->payload > owner->limit || owner->limit == 0) {
        ++owner->transient_leases;
        ++owner->active_transient_leases;
        return PreparedFieldsLease(std::move(entry), std::move(owner), true);
    }
    owner->lru.push_front(entry);
    entry->cached = true;
    owner->lookup.emplace(entry->key, owner->lru.begin());
    owner->bytes += entry->payload;
    trim(owner);
    const bool transient = !entry->cached;
    if (transient) {
        ++owner->transient_leases;
        ++owner->active_transient_leases;
    }
    return PreparedFieldsLease(std::move(entry), std::move(owner), transient);
}
} // namespace

std::size_t FieldPreparationEntry::estimate() const {
    // Count the entry object, both owned key buffers, and all row/field buffers.
    // Allocator and list/map node overhead are intentionally outside this estimate;
    // string_views refer into the owned row.
    return sizeof(*this) + 2 * string_payload(key) +
        (kind == Kind::section ? row_payload(section) : row_payload(fact)) +
        fields_payload(fields);
}

PreparedFieldsLease::PreparedFieldsLease(std::shared_ptr<FieldPreparationEntry> entry,
                                         std::shared_ptr<FieldPreparationState> owner,
                                         bool transient) noexcept
    : entry_(std::move(entry)), state_(std::move(owner)),
      fields_(entry_ ? &entry_->fields : nullptr), transient_(transient) {}
PreparedFieldsLease::PreparedFieldsLease(PreparedFieldsLease&& other) noexcept
    : entry_(std::move(other.entry_)), state_(std::move(other.state_)),
      fields_(std::exchange(other.fields_, nullptr)), transient_(std::exchange(other.transient_, false)) {}
PreparedFieldsLease& PreparedFieldsLease::operator=(PreparedFieldsLease&& other) noexcept {
    if (this != &other) {
        release();
        entry_ = std::move(other.entry_);
        state_ = std::move(other.state_);
        fields_ = std::exchange(other.fields_, nullptr);
        transient_ = std::exchange(other.transient_, false);
    }
    return *this;
}
PreparedFieldsLease::~PreparedFieldsLease() noexcept { release(); }
void PreparedFieldsLease::release() noexcept {
    if (!entry_) return;
    if (transient_ && state_ && state_->active_transient_leases)
        --state_->active_transient_leases;
    if (state_) reconcile(state_, entry_);
    fields_ = nullptr;
    transient_ = false;
    entry_.reset();
    state_.reset();
}

PreparedFieldsLease prepared_section_fields(const SectionRow& row) { return prepared(row); }
PreparedFieldsLease prepared_fact_fields(const FactRow& row) { return prepared(row); }
void reset_fields(bool enabled, std::size_t payload_limit) {
    // Active leases hold their individual entries and this old state. Detach
    // the cache itself so the epoch cannot retain unrelated rows after reset.
    auto previous = state;
    for (const auto& entry : previous->lru) entry->cached = false;
    decltype(previous->lookup) empty_lookup;
    previous->lookup.swap(empty_lookup);
    previous->lru.clear();
    previous->bytes = 0;

    auto replacement = std::make_shared<FieldPreparationState>();
    replacement->enabled = enabled;
    replacement->limit = payload_limit;
    state = std::move(replacement);
}
Json field_metrics() {
    const auto& current = *state;
    return {{"enabled", current.enabled}, {"hits", current.hits}, {"misses", current.misses},
        {"invalidations", current.invalidations}, {"evictions", current.evictions},
        {"lazy_growth", current.lazy_growth}, {"transient_leases", current.transient_leases},
        {"active_transient_leases", current.active_transient_leases},
        {"entries", current.lru.size()}, {"retained_payload_bytes_estimate", current.bytes},
        {"payload_limit", current.limit}, {"entry_limit", kEntryLimit},
        {"payload_estimate_scope", "entry and owned row/field buffers plus both lookup-key buffers; excludes allocator and container-node overhead"},
        {"cache_budget_scope", "transient and evicted-but-leased entries may outlive the cache cap until lease release; single-thread retrieval lane"}};
}
} // namespace shin::context::experiment
