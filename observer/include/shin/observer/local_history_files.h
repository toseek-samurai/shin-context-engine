#pragma once
#include "io.h"
namespace shin::observer {
// Filesystem safety only; SQLite schema, retention and transactions stay shared.
class LocalHistoryFiles {
public:
    explicit LocalHistoryFiles(const fs::path& directory);
    ~LocalHistoryFiles();
    bool created() const;
    std::string path() const;
    const char* vfs() const;
    void check() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
