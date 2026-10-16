#pragma once

#include "model.h"

namespace shin::server {
struct BoundRoute;
Json knowledge_generation(const BoundRoute& binding, const Json& active);
bool knowledge_same_source_generation(const Json& observed, const Json& selected);
}
