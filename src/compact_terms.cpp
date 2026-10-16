#include "compact_terms.h"
#include "context_internal.h"

namespace shin::context {
namespace {
constexpr std::string_view magic = "SHINLEX1";
constexpr std::uint64_t header_bytes = 32;
constexpr std::uint64_t maximum_record_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::array members{&TermPosting::sections, &TermPosting::facts,
    &TermPosting::decisions, &TermPosting::contracts, &TermPosting::runbooks};
[[noreturn]] void corrupt() {
    throw_index_error("index-corrupt", "Compact lexical index is malformed or inconsistent.");
}
void fixed(std::string& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8U * i)) & 255U));
}
std::uint64_t fixed_value(std::string_view bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8U * i);
    return value;
}
std::size_t decimal_bytes(std::uint32_t value) {
    std::size_t bytes = 1;
    while (value >= 10) { value /= 10; ++bytes; }
    return bytes;
}
std::size_t logical_record_base(const std::string& term) {
    return json_string(term).size() + std::string_view(
        "{\"term\":,\"sections\":[],\"facts\":[],\"decisions\":[],\"contracts\":[],\"runbooks\":[]}\n").size();
}
// Sorted unique IDs have at least the decimal size of [0, ..., count-1].
// Check before allocation, retaining the legacy 64 MiB logical row budget.
std::uint64_t minimum_list_bytes(std::uint32_t count) {
    if (!count) return 0;
    std::uint64_t bytes = count - 1;
    std::uint64_t first = 0, next = 10, digits = 1;
    while (first < count) {
        bytes += (std::min<std::uint64_t>(count, next) - first) * digits;
        first = next; next *= 10; ++digits;
    }
    return bytes;
}
void varint(std::string& out, std::uint32_t value) {
    while (value >= 128) { out.push_back(static_cast<char>((value & 127U) | 128U)); value >>= 7U; }
    out.push_back(static_cast<char>(value));
}
std::uint32_t take_varint(std::string_view& bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (bytes.empty()) corrupt();
        const auto byte = static_cast<unsigned char>(bytes.front()); bytes.remove_prefix(1);
        if (i == 4 && byte > 7) corrupt(); // row/count IDs fit signed 32 bits
        value |= static_cast<std::uint32_t>(byte & 127U) << (7U * i);
        if (!(byte & 128U)) {
            if (i && byte == 0) corrupt(); // reject overlong encodings
            return value;
        }
    }
    corrupt();
}
std::string take_term(std::string_view& bytes) {
    const auto count = take_varint(bytes);
    if (!count || count > 128 || count > bytes.size()) corrupt();
    std::string term(bytes.substr(0, count)); bytes.remove_prefix(count);
    // The lexical producer does not emit control characters.
    if (std::any_of(term.begin(), term.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        !Json::accept(json_string(term))) corrupt();
    return term;
}
TermPosting decode_posting(std::string_view view, const TermRowCounts& counts, std::string* term = nullptr) {
    auto key = take_term(view);
    auto logical_bytes = logical_record_base(key);
    TermPosting result;
    for (std::size_t kind = 0; kind < members.size(); ++kind) {
        const auto count = take_varint(view);
        if (count > counts[kind] || count > view.size() ||
            minimum_list_bytes(count) > maximum_record_bytes - logical_bytes) corrupt();
        if (count) logical_bytes += count - 1;
        auto& rows = result.*members[kind]; rows.reserve(count);
        std::int64_t previous = -1;
        for (std::uint32_t row = 0; row < count; ++row) {
            const auto delta = take_varint(view);
            if (!delta || previous + delta >= counts[kind]) corrupt();
            previous += delta;
            logical_bytes += decimal_bytes(static_cast<std::uint32_t>(previous));
            if (logical_bytes > maximum_record_bytes) corrupt();
            rows.push_back(static_cast<int>(previous));
        }
    }
    if (!view.empty()) corrupt();
    if (term) *term = std::move(key);
    return result;
}
} // namespace

std::uint32_t compact_term_count(const Json& manifest) {
    if (!manifest.contains("row_counts") || !manifest.at("row_counts").is_object() ||
        !manifest.at("row_counts").contains("terms")) corrupt();
    const auto& value = manifest.at("row_counts").at("terms");
    if (!value.is_number_integer()) corrupt();
    const auto count = value.get<std::int64_t>();
    if (count < 0 || count > std::numeric_limits<int>::max()) corrupt();
    return static_cast<std::uint32_t>(count);
}

TermRowCounts term_row_counts(const Json& manifest) {
    TermRowCounts counts{};
    constexpr std::array names{"sections", "facts", "decisions", "contracts", "runbooks"};
    if (!manifest.contains("row_counts") || !manifest.at("row_counts").is_object()) corrupt();
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const auto& rows = manifest.at("row_counts");
        if (!rows.contains(names[i]) || !rows.at(names[i]).is_number_integer()) corrupt();
        const auto value = rows.at(names[i]).get<std::int64_t>();
        if (value < 0 || value > std::numeric_limits<int>::max()) corrupt();
        counts[i] = static_cast<std::uint32_t>(value);
    }
    return counts;
}

std::string encode_compact_terms(const TermPostingMap& postings, const TermRowCounts& counts) {
    if (postings.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) corrupt();
    std::vector<const TermPostingMap::value_type*> ordered;
    ordered.reserve(postings.size());
    for (const auto& entry : postings) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->first < b->first; });
    std::string out(magic);
    fixed(out, postings.size(), 4);
    for (auto count : counts) {
        if (count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) corrupt();
        fixed(out, count, 4);
    }
    const auto table_start = out.size();
    out.resize(out.size() + (ordered.size() + 1) * 8);
    const auto set_offset = [&](std::size_t id) {
        const auto offset = out.size();
        for (unsigned byte = 0; byte < 8; ++byte)
            out[table_start + id * 8 + byte] = static_cast<char>((static_cast<std::uint64_t>(offset) >> (8U * byte)) & 255U);
    };
    std::size_t id = 0;
    for (const auto* entry : ordered) {
        set_offset(id++);
        const auto start = out.size();
        if (entry->first.empty() || entry->first.size() > 128) corrupt();
        varint(out, static_cast<std::uint32_t>(entry->first.size()));
        out += entry->first;
        auto logical_bytes = logical_record_base(entry->first);
        for (std::size_t kind = 0; kind < members.size(); ++kind) {
            const auto& rows = entry->second.*members[kind];
            if (rows.size() > counts[kind]) corrupt();
            varint(out, static_cast<std::uint32_t>(rows.size()));
            if (!rows.empty()) logical_bytes += rows.size() - 1;
            int previous = -1;
            for (const int row : rows) {
                if (row <= previous || row < 0 || static_cast<std::uint32_t>(row) >= counts[kind]) corrupt();
                varint(out, static_cast<std::uint32_t>(static_cast<std::int64_t>(row) - previous));
                previous = row;
                logical_bytes += decimal_bytes(static_cast<std::uint32_t>(row));
                if (logical_bytes > maximum_record_bytes) corrupt();
            }
        }
        if (out.size() - start > maximum_record_bytes) corrupt();
    }
    set_offset(id);
    return out;
}

std::string CompactTerms::read(std::uint64_t offset, std::size_t bytes) {
    if (offset > bytes_ || bytes > bytes_ - offset) corrupt();
    input_.clear(); input_.seekg(static_cast<std::streamoff>(offset));
    std::string result(bytes, '\0');
    input_.read(result.data(), static_cast<std::streamsize>(bytes));
    if (input_.gcount() != static_cast<std::streamsize>(bytes)) corrupt();
    return result;
}
CompactTerms::CompactTerms(std::istream& input, const TermRowCounts& expected) : input_(input), counts_(expected) {
    input_.clear(); input_.seekg(0, std::ios::end);
    const auto end = input_.tellg();
    if (end < 0) corrupt();
    bytes_ = static_cast<std::uint64_t>(end);
    const auto header = read(0, header_bytes);
    if (std::string_view(header).substr(0, 8) != magic) corrupt();
    count_ = static_cast<std::uint32_t>(fixed_value(std::string_view(header).substr(8, 4)));
    if (count_ > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) corrupt();
    for (std::size_t i = 0; i < counts_.size(); ++i)
        if (fixed_value(std::string_view(header).substr(12 + i * 4, 4)) != counts_[i]) corrupt();
    data_start_ = header_bytes + (static_cast<std::uint64_t>(count_) + 1) * 8;
    if (data_start_ > bytes_ || fixed_value(read(header_bytes, 8)) != data_start_ ||
        fixed_value(read(data_start_ - 8, 8)) != bytes_) corrupt();
}
std::pair<std::uint64_t, std::uint64_t> CompactTerms::range(std::uint32_t id) {
    if (id >= count_) corrupt();
    const auto offsets = read(header_bytes + static_cast<std::uint64_t>(id) * 8, 16);
    const auto start = fixed_value(std::string_view(offsets).substr(0, 8));
    const auto end = fixed_value(std::string_view(offsets).substr(8, 8));
    if (start < data_start_ || end <= start || end > bytes_ || end - start > maximum_record_bytes) corrupt();
    return {start, end};
}
std::string CompactTerms::term(std::uint32_t id) {
    const auto [start, end] = range(id);
    const auto prefix = read(start, static_cast<std::size_t>(std::min<std::uint64_t>(end - start, 130)));
    std::string_view view(prefix);
    return take_term(view);
}
TermPosting CompactTerms::posting(std::uint32_t id) {
    const auto [start, end] = range(id);
    const auto record = read(start, static_cast<std::size_t>(end - start));
    std::string_view view(record);
    return decode_posting(view, counts_);
}
std::uint32_t CompactTerms::lower_bound(std::string_view key) {
    std::uint32_t first = 0, end = count_;
    while (first < end) {
        const auto middle = first + (end - first) / 2;
        if (term(middle) < key) first = middle + 1; else end = middle;
    }
    return first;
}
void CompactTerms::validate_all() {
    // Full validation walks two monotonic streams. Bound each read-ahead window
    // instead of seeking into the offset table and record for every term. Keep
    // these buffers local: later random reads and repeated validation read anew.
    constexpr std::size_t block_bytes = 64U * 1024U;
    struct Window { std::uint64_t start = 0; std::string bytes; } offsets, records;
    const auto window = [&](Window& cache, std::uint64_t offset, std::size_t length,
                            std::uint64_t limit) -> std::string_view {
        if (offset > limit || length > limit - offset) corrupt();
        if (offset < cache.start || offset - cache.start > cache.bytes.size() ||
            length > cache.bytes.size() - static_cast<std::size_t>(offset - cache.start)) {
            cache.bytes = read(offset, static_cast<std::size_t>(
                std::min<std::uint64_t>(block_bytes, limit - offset)));
            cache.start = offset;
        }
        return std::string_view(cache.bytes).substr(static_cast<std::size_t>(offset - cache.start), length);
    };
    std::string previous;
    for (std::uint32_t id = 0; id < count_; ++id) {
        const auto pair = window(offsets, header_bytes + static_cast<std::uint64_t>(id) * 8, 16, data_start_);
        const auto start = fixed_value(pair.substr(0, 8));
        const auto end = fixed_value(pair.substr(8, 8));
        if (start < data_start_ || end <= start || end > bytes_ || end - start > maximum_record_bytes) corrupt();
        std::string key;
        const auto length = static_cast<std::size_t>(end - start);
        // Exceptional large postings retain the original per-record budget.
        // They never make either read-ahead window grow with the whole corpus.
        if (length > block_bytes) (void)decode_posting(read(start, length), counts_, &key);
        else (void)decode_posting(window(records, start, length, bytes_), counts_, &key);
        if (id && key <= previous) corrupt();
        previous = std::move(key);
    }
}
} // namespace shin::context
