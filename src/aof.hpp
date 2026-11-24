// aof.hpp: the append-only file (AOF), which is how minikv survives restarts.
//
// Every command that changes data is appended to a log file as one line.
// On startup the log is replayed from the top, which rebuilds the exact
// same data. This is the same idea as Redis's AOF or a database's
// write-ahead log.
#pragma once

#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace minikv {

class AofWriter {
public:
    explicit AofWriter(const std::string& path);  // throws if the file can't be opened

    // Writes one command as a line and flushes it to the operating system.
    void append(const std::vector<std::string>& args);

private:
    std::ofstream out_;
};

struct ReplayResult {
    size_t applied = 0;
    size_t skipped = 0;  // malformed or rejected lines (e.g. a half-written last line after a crash)
};

// Reads the file line by line and calls `apply` for each command.
// `apply` returns false if the command was rejected. A missing file
// simply means there is nothing to replay.
ReplayResult replay_aof(const std::string& path,
                        const std::function<bool(const std::vector<std::string>&)>& apply);

}  // namespace minikv
