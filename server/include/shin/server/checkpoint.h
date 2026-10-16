#pragma once

#include "model.h"
#include <cstdint>

namespace shin::server {

struct CheckpointMutation {
    std::string task_id;
    std::string thread_id;
    std::string request_id;
    std::string request_hash;
    std::uint64_t expected_revision = 0;
    std::int64_t request_deadline = 0;
};

inline constexpr std::size_t kCheckpointMaximumBytes = 262144;
inline constexpr std::size_t kCheckpointMaximumSlots = 1024;
inline constexpr std::size_t kCheckpointMaximumOwnerSlots = 128;
inline constexpr std::int64_t kCheckpointRetrySeconds = 300;

void validate_checkpoint_mutation(const CheckpointMutation& mutation);
bool checkpoint_receipt_origins_valid(const Json& checkpoint, const std::string& owner, const Json& origins);
Json checkpoint_public_record(const Json& record);

} // namespace shin::server
