// database.hpp: the command layer. Ties together the Store (data), the
// AOF (persistence), and a mutex (thread safety).
//
// Every client thread calls execute(). The mutex makes each command atomic,
// and because the AOF is written while the lock is held, the order of lines
// in the file always matches the order the commands were applied in memory.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aof.hpp"
#include "store.hpp"

namespace minikv {

TimeMs now_ms();  // wall-clock time in ms since the Unix epoch

class Database {
public:
    // aof_path empty = persistence disabled.
    Database(size_t max_keys, std::string aof_path);

    // Replays the AOF (if any) and opens it for appending. Call once at
    // startup, before serving clients.
    ReplayResult load();

    // Runs one command, e.g. {"SET", "name", "alice"}, and returns the reply
    // text. Thread-safe.
    std::string execute(const std::vector<std::string>& args);

    // Removes expired keys. Called by the background sweeper thread.
    size_t sweep_expired();

private:
    using Log = std::vector<std::vector<std::string>>;

    // The actual command implementations. Assumes mu_ is held. Writes that
    // must be persisted are pushed onto `log`.
    std::string dispatch(const std::vector<std::string>& args, TimeMs now, Log& log);

    std::mutex mu_;
    Store store_;
    size_t max_keys_;
    std::string aof_path_;
    std::unique_ptr<AofWriter> aof_;
};

}  // namespace minikv
