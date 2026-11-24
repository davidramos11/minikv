#include "database.hpp"

#include <chrono>
#include <limits>

#include "protocol.hpp"

namespace minikv {

// We use wall-clock time (not steady_clock) because expiry times are written
// to the AOF and must still mean the same thing after a restart. Trade-off:
// if the system clock jumps, TTLs jump with it.
TimeMs now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

constexpr int64_t kMaxTtlSeconds = 100LL * 365 * 24 * 3600;  // guards against overflow

std::string integer(int64_t n) { return "(integer) " + std::to_string(n); }

std::string wrong_args(const std::string& cmd) {
    return "ERR wrong number of arguments for '" + cmd + "'";
}

}  // namespace

Database::Database(size_t max_keys, std::string aof_path)
    : store_(max_keys), max_keys_(max_keys), aof_path_(std::move(aof_path)) {}

ReplayResult Database::load() {
    std::lock_guard<std::mutex> lock(mu_);
    ReplayResult result;
    if (aof_path_.empty()) return result;

    // During replay, capacity is unlimited. Evictions were logged as explicit
    // DEL lines when they happened, so replay reproduces them exactly. If we
    // let the Store evict on its own here, it could pick different victims,
    // because GETs (which change LRU order) are never logged.
    store_.set_max_keys(std::numeric_limits<size_t>::max());
    const TimeMs now = now_ms();
    result = replay_aof(aof_path_, [&](const std::vector<std::string>& args) {
        Log ignored;  // don't re-log commands we're reading from the log
        return dispatch(args, now, ignored).rfind("ERR", 0) != 0;
    });
    auto evicted = store_.set_max_keys(max_keys_);  // in case --max-keys shrank

    aof_ = std::make_unique<AofWriter>(aof_path_);
    for (const auto& key : evicted) aof_->append({"DEL", key});
    return result;
}

std::string Database::execute(const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> lock(mu_);
    Log log;
    std::string reply = dispatch(args, now_ms(), log);
    if (aof_) {
        for (const auto& record : log) aof_->append(record);
    }
    return reply;
}

size_t Database::sweep_expired() {
    std::lock_guard<std::mutex> lock(mu_);
    // Not logged: the AOF stores absolute expiry times, so replay drops
    // these keys on its own.
    return store_.remove_expired(now_ms());
}

std::string Database::dispatch(const std::vector<std::string>& args, TimeMs now, Log& log) {
    if (args.empty()) return "ERR empty command";
    const std::string cmd = to_upper(args[0]);
    const size_t argc = args.size();

    if (cmd == "PING") {
        if (argc > 2) return wrong_args(cmd);
        return argc == 2 ? args[1] : "PONG";
    }

    // SET key value [EX seconds | PX milliseconds | PXAT unix-ms]
    if (cmd == "SET") {
        if (argc != 3 && argc != 5) return wrong_args(cmd);
        const std::string& key = args[1];
        std::optional<TimeMs> expires_at;
        if (argc == 5) {
            const std::string opt = to_upper(args[3]);
            int64_t n = 0;
            if (!parse_int(args[4], n)) return "ERR value is not an integer";
            if (opt == "EX") {
                if (n <= 0 || n > kMaxTtlSeconds) return "ERR invalid expire time";
                expires_at = now + n * 1000;
            } else if (opt == "PX") {
                if (n <= 0 || n > kMaxTtlSeconds * 1000) return "ERR invalid expire time";
                expires_at = now + n;
            } else if (opt == "PXAT") {  // absolute time; this is what the AOF stores
                if (n <= 0) return "ERR invalid expire time";
                expires_at = n;
            } else {
                return "ERR syntax error";
            }
        }
        if (expires_at && *expires_at <= now) {
            // Already expired (happens when replaying an old AOF entry).
            if (store_.del(key, now)) log.push_back({"DEL", key});
            return "OK";
        }
        auto evicted = store_.set(key, args[2], expires_at, now);

        std::vector<std::string> record = {"SET", key, args[2]};
        if (expires_at) {
            // Log the absolute time, not "EX 10": otherwise a restart would
            // give the key a fresh 10 seconds.
            record.push_back("PXAT");
            record.push_back(std::to_string(*expires_at));
        }
        log.push_back(std::move(record));
        for (const auto& victim : evicted) log.push_back({"DEL", victim});
        return "OK";
    }

    if (cmd == "GET") {
        if (argc != 2) return wrong_args(cmd);
        auto value = store_.get(args[1], now);
        return value ? *value : "(nil)";
    }

    if (cmd == "DEL") {
        if (argc < 2) return wrong_args(cmd);
        int64_t removed = 0;
        for (size_t i = 1; i < argc; ++i) {
            if (store_.del(args[i], now)) {
                ++removed;
                log.push_back({"DEL", args[i]});
            }
        }
        return integer(removed);
    }

    if (cmd == "EXISTS") {
        if (argc < 2) return wrong_args(cmd);
        int64_t count = 0;
        for (size_t i = 1; i < argc; ++i) count += store_.exists(args[i], now) ? 1 : 0;
        return integer(count);
    }

    // EXPIRE key seconds  /  PEXPIREAT key unix-ms
    if (cmd == "EXPIRE" || cmd == "PEXPIREAT") {
        if (argc != 3) return wrong_args(cmd);
        int64_t n = 0;
        if (!parse_int(args[2], n)) return "ERR value is not an integer";
        TimeMs when;
        if (cmd == "EXPIRE") {
            if (n > kMaxTtlSeconds) return "ERR invalid expire time";
            when = now + n * 1000;  // n <= 0 means "delete now", same as Redis
        } else {
            when = n;
        }
        bool ok = store_.expire_at(args[1], when, now);
        if (ok) log.push_back({"PEXPIREAT", args[1], std::to_string(when)});
        return integer(ok ? 1 : 0);
    }

    if (cmd == "TTL" || cmd == "PTTL") {
        if (argc != 2) return wrong_args(cmd);
        int64_t ms = store_.ttl_ms(args[1], now);
        if (ms < 0) return integer(ms);  // -1 or -2
        return integer(cmd == "PTTL" ? ms : (ms + 999) / 1000);  // round seconds up
    }

    if (cmd == "DBSIZE") {
        if (argc != 1) return wrong_args(cmd);
        return integer(static_cast<int64_t>(store_.size()));
    }

    if (cmd == "FLUSHALL") {
        if (argc != 1) return wrong_args(cmd);
        store_.clear();
        log.push_back({"FLUSHALL"});
        return "OK";
    }

    if (cmd == "INFO") {
        if (argc != 1) return wrong_args(cmd);
        return "keys:" + std::to_string(store_.size()) +
               " max_keys:" + std::to_string(max_keys_) +
               " evictions:" + std::to_string(store_.evictions()) +
               " persistence:" + (aof_path_.empty() ? "off" : "aof");
    }

    return "ERR unknown command '" + args[0] + "'";
}

}  // namespace minikv
