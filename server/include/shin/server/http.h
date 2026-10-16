#pragma once

#include "config.h"

namespace shin::server {

Json response_envelope(Json result);
Json error_envelope(const ServiceError& error);
int serve(const Config& config);

} // namespace shin::server
