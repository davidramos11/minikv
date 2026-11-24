// Unit tests. Deliberately no test framework (no GoogleTest/Catch2), so the
// project builds with nothing but a compiler and CMake.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "database.hpp"
#include "protocol.hpp"
#include "store.hpp"

using namespace minikv;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

static void test_store_basics() {
    Store s(10);
    CHECK(!s.get("a", 0).has_value());
    s.set("a", "1", std::nullopt, 0);
    CHECK(s.get("a", 0) == "1");
    s.set("a", "2", std::nullopt, 0);  // overwrite
    CHECK(s.get("a", 0) == "2");
    CHECK(s.size() == 1);
    CHECK(s.del("a", 0));
    CHECK(!s.del("a", 0));
    CHECK(s.size() == 0);
}

static void test_lru_eviction() {
    Store s(2);
    s.set("a", "1", std::nullopt, 0);
    s.set("b", "2", std::nullopt, 0);
    s.get("a", 0);  // "a" is now most recently used, so "b" is the LRU
    auto evicted = s.set("c", "3", std::nullopt, 0);
    CHECK(evicted.size() == 1 && evicted[0] == "b");
    CHECK(s.exists("a", 0));
    CHECK(!s.exists("b", 0));
    CHECK(s.exists("c", 0));
    CHECK(s.evictions() == 1);

    // Overwriting an existing key never evicts.
    CHECK(s.set("a", "x", std::nullopt, 0).empty());
    // Shrinking capacity evicts the LRU ("c", since "a" was just touched).
    auto shrunk = s.set_max_keys(1);
    CHECK(shrunk.size() == 1 && shrunk[0] == "c");
}

static void test_ttl() {
    Store s(10);
    s.set("k", "v", TimeMs{1000}, 0);  // expires at t=1000ms
    CHECK(s.ttl_ms("k", 0) == 1000);
    CHECK(s.ttl_ms("k", 400) == 600);
    CHECK(s.get("k", 999) == "v");
    CHECK(!s.get("k", 1000).has_value());  // expired exactly at 1000
    CHECK(s.ttl_ms("k", 1000) == -2);
    CHECK(s.size() == 0);  // lazy expiry removed it

    s.set("p", "v", std::nullopt, 0);
    CHECK(s.ttl_ms("p", 0) == -1);
    CHECK(s.expire_at("p", 500, 0));
    CHECK(s.ttl_ms("p", 100) == 400);
    CHECK(s.expire_at("p", 50, 100));  // time in the past: deletes
    CHECK(!s.exists("p", 100));
    CHECK(!s.expire_at("missing", 500, 0));

    // Setting a key again clears its old TTL.
    s.set("q", "v", TimeMs{100}, 0);
    s.set("q", "v2", std::nullopt, 0);
    CHECK(s.ttl_ms("q", 0) == -1);
}

static void test_active_expiry() {
    Store s(10);
    s.set("a", "1", TimeMs{100}, 0);
    s.set("b", "2", TimeMs{200}, 0);
    s.set("c", "3", std::nullopt, 0);
    CHECK(s.remove_expired(150) == 1);
    CHECK(s.size() == 2);
    CHECK(s.remove_expired(1000) == 1);
    CHECK(s.size() == 1);
}

static void test_tokenize_and_quote() {
    CHECK((tokenize("SET a b") == std::vector<std::string>{"SET", "a", "b"}));
    CHECK((tokenize("  GET   a  ") == std::vector<std::string>{"GET", "a"}));
    CHECK(tokenize("").empty());
    CHECK((tokenize(R"(SET k "hello world")") == std::vector<std::string>{"SET", "k", "hello world"}));
    CHECK((tokenize(R"(SET k "")") == std::vector<std::string>{"SET", "k", ""}));

    bool threw = false;
    try { tokenize(R"(SET k "oops)"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    // Round trip: anything quote()-ed must tokenize back to the same string.
    std::vector<std::string> nasty = {"plain", "with space", "quote\"inside", "back\\slash",
                                      "new\nline", "tab\there", "", "\r\n"};
    for (const auto& s : nasty) {
        auto args = tokenize("X " + quote(s));
        CHECK(args.size() == 2 && args[1] == s);
    }

    int64_t n = 0;
    CHECK(parse_int("42", n) && n == 42);
    CHECK(parse_int("-7", n) && n == -7);
    CHECK(!parse_int("4x", n));
    CHECK(!parse_int("", n));
}

static void test_database_commands() {
    Database db(100, "");  // no persistence
    db.load();
    CHECK(db.execute({"PING"}) == "PONG");
    CHECK(db.execute({"set", "name", "alice"}) == "OK");  // case-insensitive commands
    CHECK(db.execute({"GET", "name"}) == "alice");
    CHECK(db.execute({"GET", "nope"}) == "(nil)");
    CHECK(db.execute({"EXISTS", "name", "nope"}) == "(integer) 1");
    CHECK(db.execute({"TTL", "name"}) == "(integer) -1");
    CHECK(db.execute({"EXPIRE", "name", "10"}) == "(integer) 1");
    CHECK(db.execute({"TTL", "name"}) == "(integer) 10");
    CHECK(db.execute({"SET", "t", "v", "EX", "5"}) == "OK");
    CHECK(db.execute({"TTL", "t"}) == "(integer) 5");
    CHECK(db.execute({"DBSIZE"}) == "(integer) 2");
    CHECK(db.execute({"DEL", "name", "t", "ghost"}) == "(integer) 2");
    CHECK(db.execute({"SET", "x"}).rfind("ERR wrong number", 0) == 0);
    CHECK(db.execute({"SET", "x", "y", "EX", "abc"}) == "ERR value is not an integer");
    CHECK(db.execute({"SET", "x", "y", "EX", "0"}) == "ERR invalid expire time");
    CHECK(db.execute({"SET", "x", "y", "NX", "1"}) == "ERR syntax error");
    CHECK(db.execute({"BOGUS"}).rfind("ERR unknown command", 0) == 0);
}

static void test_persistence() {
    const std::string path =
        (std::filesystem::temp_directory_path() / "minikv_test.aof").string();
    std::remove(path.c_str());

    {
        Database db(2, path);
        db.load();
        db.execute({"SET", "a", "1"});
        db.execute({"SET", "b", "two words"});
        db.execute({"GET", "a"});           // touch "a", so "b" is LRU
        db.execute({"SET", "c", "3"});      // evicts "b"
        db.execute({"SET", "gone", "x", "PX", "1"});  // evicts "a" (LRU), expires almost immediately
        db.execute({"DEL", "c"});
        db.execute({"SET", "ttl", "y", "EX", "100"});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    {
        Database db(2, path);
        auto r = db.load();
        CHECK(r.skipped == 0);
        CHECK(db.execute({"GET", "a"}) == "(nil)");      // was evicted
        CHECK(db.execute({"GET", "b"}) == "(nil)");      // was evicted
        CHECK(db.execute({"GET", "c"}) == "(nil)");      // was deleted
        CHECK(db.execute({"GET", "gone"}) == "(nil)");   // expired while "down"
        CHECK(db.execute({"GET", "ttl"}) == "y");
        // TTL is restored from the absolute time, not reset to 100.
        auto ttl = db.execute({"TTL", "ttl"});
        CHECK(ttl == "(integer) 100" || ttl == "(integer) 99");
        CHECK(db.execute({"DBSIZE"}) == "(integer) 1");
    }

    // A crash mid-write can leave a half-written last line. Replay should
    // skip it and keep everything before it.
    {
        std::FILE* f = std::fopen(path.c_str(), "a");
        std::fputs("\"SET\" \"half", f);
        std::fclose(f);
        Database db(10, path);
        auto r = db.load();
        CHECK(r.skipped == 1);
        CHECK(db.execute({"GET", "ttl"}) == "y");
    }
    std::remove(path.c_str());
}

static void test_concurrency() {
    // 8 threads x 1000 increments of separate keys plus shared reads.
    // The mutex in Database must keep the store consistent.
    Database db(1000000, "");
    db.load();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&db, t] {
            for (int i = 0; i < 1000; ++i) {
                std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
                db.execute({"SET", key, "v"});
                db.execute({"GET", key});
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(db.execute({"DBSIZE"}) == "(integer) 8000");
}

int main() {
    test_store_basics();
    test_lru_eviction();
    test_ttl();
    test_active_expiry();
    test_tokenize_and_quote();
    test_database_commands();
    test_persistence();
    test_concurrency();

    if (g_failures == 0) {
        std::cout << "All " << g_checks << " checks passed\n";
        return 0;
    }
    std::cout << g_failures << " of " << g_checks << " checks FAILED\n";
    return 1;
}
