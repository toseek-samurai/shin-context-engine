#pragma once

#include "context_internal.h"
#include <memory>

namespace shin::context::experiment {

struct FieldPreparationEntry;
struct FieldPreparationState;

class PreparedFieldsLease {
public:
    PreparedFieldsLease() noexcept = default;
    PreparedFieldsLease(const PreparedFieldsLease&) = delete;
    PreparedFieldsLease& operator=(const PreparedFieldsLease&) = delete;
    PreparedFieldsLease(PreparedFieldsLease&& other) noexcept;
    PreparedFieldsLease& operator=(PreparedFieldsLease&& other) noexcept;
    ~PreparedFieldsLease() noexcept;

    std::vector<FieldMatchInput>* get() const noexcept { return fields_; }
    explicit operator bool() const noexcept { return fields_ != nullptr; }

    PreparedFieldsLease(std::shared_ptr<FieldPreparationEntry> entry,
                        std::shared_ptr<FieldPreparationState> state,
                        bool transient) noexcept;

private:
    friend PreparedFieldsLease prepared_section_fields(const SectionRow&);
    friend PreparedFieldsLease prepared_fact_fields(const FactRow&);
    void release() noexcept;

    std::shared_ptr<FieldPreparationEntry> entry_;
    std::shared_ptr<FieldPreparationState> state_;
    std::vector<FieldMatchInput>* fields_ = nullptr;
    bool transient_ = false;
};

PreparedFieldsLease prepared_section_fields(const SectionRow& row);
PreparedFieldsLease prepared_fact_fields(const FactRow& row);
void reset_fields(bool enabled, std::size_t payload_limit = 32U * 1024U * 1024U);
Json field_metrics();

} // namespace shin::context::experiment
