#pragma once
#include "model.h"
#include "schema_checks.h"

namespace shin::server {
void append_observer_schema_checks(SchemaChecks& checks);
void validate_enrollment(const Json& enrollment);
} // namespace shin::server
