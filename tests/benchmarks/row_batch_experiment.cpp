#include "row_batch_experiment.h"

namespace shin::context::experiment {
namespace {
thread_local PreparedRowScope* active_scope = nullptr;
}

PreparedRowScope::PreparedRowScope() noexcept : previous_(active_scope) {
    active_scope = this;
}

PreparedRowScope::PreparedRowScope(const SectionRow& row)
    : section_(&row), fields_(section_match_fields(row)), previous_(active_scope) {
    active_scope = this;
}

PreparedRowScope::PreparedRowScope(const FactRow& row)
    : fact_(&row), fields_(fact_match_fields(row)), previous_(active_scope) {
    active_scope = this;
}

PreparedRowScope::~PreparedRowScope() noexcept {
    if (active_scope == this) active_scope = previous_;
}

std::vector<FieldMatchInput>* prepared_batch_fields(const SectionRow& row) noexcept {
    return active_scope && active_scope->section_ == &row ? &active_scope->fields_ : nullptr;
}

std::vector<FieldMatchInput>* prepared_batch_fields(const FactRow& row) noexcept {
    return active_scope && active_scope->fact_ == &row ? &active_scope->fields_ : nullptr;
}

} // namespace shin::context::experiment
