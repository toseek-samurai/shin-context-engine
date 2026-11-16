#pragma once
#include "shin/observer/observer.h"

namespace shin::observer {
// Presentation only. Callers retain admission, publication locks and final
// source/authority verification before returning the response.
class LocalContextDelivery {
public:
    LocalContextDelivery(Json& response, const Config& config) : response_(response), config_(config) {}
    void query(Json documents, const Json& source, const Json& scope);
    void followup(const Json& source);
    void summarize(double measured_retrieval_ms = 0) const;
private:
    Json wrap(const Json& source, const char* schema, const char* key) const;
    Json& response_;
    const Config& config_;
    std::optional<std::size_t> shown_;
};
} // namespace shin::observer
