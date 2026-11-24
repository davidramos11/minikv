// store.hpp: the in-memory data structure at the heart of minikv.
//
// Store is a hash map with two extra features:
//   1. LRU eviction: when there are more than max_keys entries, the
//      least-recently-used key is removed.
//   2. TTL expiry: a key can have an expiry time, after which it is treated
//      as if it does not exist.
//
// Store is NOT thread-safe on purpose. Database (database.hpp) owns a mutex
// and is the only thing that touches the Store. Keeping locking out of here
// makes this class simple to unit test.
//
// Every method that cares about time takes `now` as a parameter instead of
// reading the clock itself. That lets tests pass fake times ("pretend 10
// seconds have passed") without sleeping.
#pragma once

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace minikv {

using TimeMs = int64_t;  // milliseconds since the Unix epoch

class Store {
public:
    explicit Store(size_t max_keys);

    // Insert or overwrite a key. Returns the keys evicted to make room
    // (usually empty). The caller logs those evictions to the AOF.
    std::vector<std::string> set(const std::string& key, std::string value,
                                 std::optional<TimeMs> expires_at, TimeMs now);

    // Returns the value, or nullopt if the key is missing or expired.
    // A successful get marks the key as most recently used.
    std::optional<std::string> get(const std::string& key, TimeMs now);

    // Returns true if a live key was removed.
    bool del(const std::string& key, TimeMs now);

    bool exists(const std::string& key, TimeMs now);

    // Sets an absolute expiry time. If `when` is already in the past the key
    // is deleted immediately. Returns false if the key does not exist.
    bool expire_at(const std::string& key, TimeMs when, TimeMs now);

    // Milliseconds until expiry, or -1 (no expiry) / -2 (no such key).
    // Same convention as Redis.
    int64_t ttl_ms(const std::string& key, TimeMs now);

    // Removes every expired key. Called periodically by a background thread.
    size_t remove_expired(TimeMs now);

    // Changes capacity; evicts LRU keys if now over the limit.
    std::vector<std::string> set_max_keys(size_t max_keys);

    void clear();
    size_t size() const { return map_.size(); }
    size_t max_keys() const { return max_keys_; }
    uint64_t evictions() const { return evictions_; }

private:
    struct Entry {
        std::string value;
        std::optional<TimeMs> expires_at;
        // Where this key sits in lru_. Storing the iterator lets us move the
        // key to the front in O(1) without searching the list.
        std::list<std::string>::iterator lru_pos;
    };
    using Map = std::unordered_map<std::string, Entry>;

    static bool is_expired(const Entry& e, TimeMs now);
    Map::iterator find_live(const std::string& key, TimeMs now);
    void touch(Entry& e);
    void remove(Map::iterator it);
    std::vector<std::string> evict_if_needed();

    size_t max_keys_;
    std::list<std::string> lru_;  // front = most recently used, back = least
    Map map_;
    uint64_t evictions_ = 0;
};

}  // namespace minikv
