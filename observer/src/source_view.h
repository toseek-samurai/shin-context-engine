#pragma once
#include "shin/observer/observer.h"

namespace shin::observer {
// The structured receipt and the text describe the same bounded projection.
// Inspection has a JSON inventory instead of a text-body delivery receipt.
struct SourceViewProjection {
    std::string text;
    Json receipt;
};
SourceViewProjection project_source_view(const Json& result, std::size_t maximum_bytes = 8192);
} // namespace shin::observer
