#pragma once
#include "context_types.h"

namespace shin::context {
Json prepare_document_lifecycle(GeneratedRows& rows);
BriefResult supplement_document_lifecycle(const RuntimeOptions& options, const QueryRequest& request,
                                         const Json& manifest, BriefResult result);
} // namespace shin::context
