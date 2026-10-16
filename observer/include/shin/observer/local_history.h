#pragma once
#include "io.h"

namespace shin::observer {
// Single asynchronous owner. The query/scheduler thread only queues bounded rows
// and copies a cached summary; it never opens or waits for SQLite.
class LocalHistory {
public:
    explicit LocalHistory(fs::path directory);
    ~LocalHistory();
    LocalHistory(const LocalHistory&) = delete;
    LocalHistory& operator=(const LocalHistory&) = delete;
    void append(const std::string& kind, const Json& row);
    Json snapshot() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace shin::observer
