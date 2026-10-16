#pragma once

#include "context_internal.h"

namespace shin::context::experiment {

// The row passed to a scope must outlive the scope. Prepared fields borrow its
// strings and are discarded with the scope.
class PreparedRowScope {
public:
    PreparedRowScope() noexcept;
    explicit PreparedRowScope(const SectionRow& row);
    explicit PreparedRowScope(const FactRow& row);
    PreparedRowScope(const PreparedRowScope&) = delete;
    PreparedRowScope& operator=(const PreparedRowScope&) = delete;
    PreparedRowScope(PreparedRowScope&&) = delete;
    PreparedRowScope& operator=(PreparedRowScope&&) = delete;
    ~PreparedRowScope() noexcept;

    const std::vector<FieldMatchInput>& fields() const noexcept { return fields_; }

private:
    friend std::vector<FieldMatchInput>* prepared_batch_fields(const SectionRow&) noexcept;
    friend std::vector<FieldMatchInput>* prepared_batch_fields(const FactRow&) noexcept;
    const SectionRow* section_ = nullptr;
    const FactRow* fact_ = nullptr;
    std::vector<FieldMatchInput> fields_;
    PreparedRowScope* previous_ = nullptr;
};

std::vector<FieldMatchInput>* prepared_batch_fields(const SectionRow& row) noexcept;
std::vector<FieldMatchInput>* prepared_batch_fields(const FactRow& row) noexcept;

} // namespace shin::context::experiment
