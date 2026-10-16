#include "context_internal.h"
#include "execution_io.h"
#include "source_read.h"

namespace shin::context {

std::string read_bound_source(const fs::path& root, const std::string& path,
                              const std::string& expected_hash, std::size_t maximum_bytes) {
    if (!profile_relative_path_valid(path) || fs::path(path).lexically_normal().generic_string() != path ||
        expected_hash.empty()) {
        throw_index_error("source-read-invalid-binding", "Source read requires a canonical published binding.", false);
    }
    // The common descriptor reader rejects symlinks, special files and concurrent
    // replacement before the captured bytes can be checked against the snapshot.
    std::string content = read_execution_file(root, path, maximum_bytes);
    if (fnv1a64(content) != expected_hash)
        throw_index_error("source-read-hash-mismatch", "Source bytes no longer match their published binding.");
    return content;
}

std::vector<std::size_t> source_line_offsets(const std::string& content) {
    std::vector<std::size_t> offsets{0};
    for (std::size_t i = 0; i < content.size(); ++i)
        if (content[i] == '\n') offsets.push_back(i + 1);
    return offsets;
}

std::string source_physical_range(const std::string& content,
                                  const std::vector<std::size_t>& offsets, int start, int end) {
    if (start < 1 || end < start || static_cast<std::size_t>(end) > offsets.size())
        throw_index_error("source-read-invalid-range", "Published range is outside its source bytes.", false);
    const auto begin = offsets[static_cast<std::size_t>(start - 1)];
    const auto finish = static_cast<std::size_t>(end) < offsets.size()
        ? offsets[static_cast<std::size_t>(end)] : content.size();
    if (begin > finish || finish > content.size())
        throw_index_error("source-read-invalid-range", "Published range is outside its source bytes.", false);
    return content.substr(begin, finish - begin);
}

SecretScanHit source_range_secret(const std::string& text) {
    SecretScanHit hit = detect_secret_like_text(text, false);
    if (!hit.found) {
        for (const auto& line : split_lines(text)) {
            hit = detect_secret_like_text(line, false);
            if (hit.found) break;
        }
    }
    if (!hit.found) hit = detect_secret_like_text(compact_whitespace(text), false);
    return hit;
}

}  // namespace shin::context
