#pragma once

namespace pqxx { class transaction_base; }

namespace shin::server {
void verify_runtime_schema(pqxx::transaction_base& transaction);
}
