#include "store.hpp"

#include <stdexcept>

namespace minikv {

Store::Store(size_t max_keys) : max_keys_(max_keys) {
    if (max_keys_ == 0) throw std::invalid_argument("max_keys must be at least 1");
}

bool Store::is_expired(const Entry& e, TimeMs now) {
    return e.expires_at.has_value() && *e.expires_at <= now;
}

// Looks up a key and treats an expired entry as missing. Expired entries
// found here are deleted on the spot ("lazy expiry"). The background sweeper
// in remove_expired() catches keys that nobody reads again ("active expiry").
Store::Map::iterator Store::find_live(const std::string& key, TimeMs now) {
    auto it = map_.find(key);
    if (it != map_.end() && is_expired(it->second, now)) {
        remove(it);
        return map_.end();
    }
    return it;
}

// Move a key to the front of the LRU list. splice() relinks the existing
// node, so it is O(1) and does not invalidate any iterators.
void Store::touch(Entry& e) {
    lru_.splice(lru_.begin(), lru_, e.lru_pos);
}

void Store::remove(Map::iterator it) {
    lru_.erase(it->second.lru_pos);
    map_.erase(it);
}

std::vector<std::string> Store::evict_if_needed() {
    std::vector<std::string> evicted;
    while (map_.size() > max_keys_) {
        // Copy the key before erasing: erasing destroys the list node.
        std::string victim = lru_.back();
        remove(map_.find(victim));
        ++evictions_;
        evicted.push_back(std::move(victim));
    }
    return evicted;
}

std::vector<std::string> Store::set(const std::string& key, std::string value,
                                    std::optional<TimeMs> expires_at, TimeMs now) {
    auto it = find_live(key, now);
    if (it != map_.end()) {
        // Overwrite. Size doesn't change, so no eviction is needed.
        it->second.value = std::move(value);
        it->second.expires_at = expires_at;
        touch(it->second);
        return {};
    }
    lru_.push_front(key);
    map_.emplace(key, Entry{std::move(value), expires_at, lru_.begin()});
    return evict_if_needed();
}

std::optional<std::string> Store::get(const std::string& key, TimeMs now) {
    auto it = find_live(key, now);
    if (it == map_.end()) return std::nullopt;
    touch(it->second);
    return it->second.value;
}

bool Store::del(const std::string& key, TimeMs now) {
    auto it = find_live(key, now);
    if (it == map_.end()) return false;
    remove(it);
    return true;
}

bool Store::exists(const std::string& key, TimeMs now) {
    return find_live(key, now) != map_.end();
}

bool Store::expire_at(const std::string& key, TimeMs when, TimeMs now) {
    auto it = find_live(key, now);
    if (it == map_.end()) return false;
    if (when <= now) {
        remove(it);
    } else {
        it->second.expires_at = when;
    }
    return true;
}

int64_t Store::ttl_ms(const std::string& key, TimeMs now) {
    auto it = find_live(key, now);
    if (it == map_.end()) return -2;
    if (!it->second.expires_at) return -1;
    return *it->second.expires_at - now;
}

// O(n) scan of every key. Fine for a project this size. Real Redis instead
// samples ~20 random keys that have a TTL, many times per second, so the
// cost stays small even with millions of keys.
size_t Store::remove_expired(TimeMs now) {
    size_t removed = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (is_expired(it->second, now)) {
            lru_.erase(it->second.lru_pos);
            it = map_.erase(it);  // erase returns the next valid iterator
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<std::string> Store::set_max_keys(size_t max_keys) {
    if (max_keys == 0) throw std::invalid_argument("max_keys must be at least 1");
    max_keys_ = max_keys;
    return evict_if_needed();
}

void Store::clear() {
    map_.clear();
    lru_.clear();
}

}  // namespace minikv
