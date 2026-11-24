# minikv

A Redis-style in-memory key-value store written in C++17, built from scratch on raw POSIX sockets with no external dependencies.

- **TCP server**: multiple clients connect at once and send text commands (`SET`, `GET`, `DEL`, `EXPIRE`, …)
- **LRU eviction**: when memory hits `--max-keys`, the least-recently-used key is evicted in O(1)
- **TTL expiry**: keys can expire, cleaned up both lazily (on read) and actively (background sweep)
- **Persistence**: an append-only file (AOF) logs every write, and data is rebuilt on restart
- **Thread-safe**: one thread per client, with a mutex making each command atomic
- **Tested**: 77 unit-test checks, clean under AddressSanitizer, UBSan and ThreadSanitizer

## Quick start (macOS / Linux)

```bash
# needs a C++17 compiler and CMake (macOS: xcode-select --install && brew install cmake)
cmake -S . -B build
cmake --build build
./build/minikv_tests          # run unit tests
./build/minikv                # start server on 127.0.0.1:6380
```

In a second terminal:

```
$ nc localhost 6380
SET greeting "hello world"
OK
GET greeting
hello world
SET session abc123 EX 30
OK
TTL session
(integer) 30
INFO
keys:2 max_keys:100000 evictions:0 persistence:aof
```

Stop the server (Ctrl+C), start it again, and `GET greeting` still works.

### Options

| Flag | Default | Meaning |
|------|---------|---------|
| `--port N` | 6380 | TCP port (localhost only) |
| `--max-keys N` | 100000 | evict LRU keys beyond this count |
| `--aof PATH` | `minikv.aof` | persistence file |
| `--no-aof` | | in-memory only |

### Commands

| Command | Reply |
|---------|-------|
| `PING [msg]` | `PONG` or `msg` |
| `SET key value [EX sec \| PX ms]` | `OK` |
| `GET key` | value or `(nil)` |
| `DEL key [key ...]` | `(integer)` number deleted |
| `EXISTS key [key ...]` | `(integer)` number that exist |
| `EXPIRE key sec` | `(integer) 1` if set, `0` if no such key |
| `TTL key` / `PTTL key` | seconds / ms left, `-1` no expiry, `-2` no key |
| `DBSIZE` | `(integer)` key count |
| `INFO` | stats line |
| `FLUSHALL` | `OK` (deletes everything) |
| `QUIT` | `OK`, closes connection |

Values with spaces go in double quotes. `\"`, `\\` and `\n` escapes are supported.

## Architecture

```
 client (nc / bench.py)
        │  TCP, one command per line
        ▼
 ┌──────────────┐  server.cpp: accept loop, one thread per client,
 │    Server    │  buffers bytes into complete lines
 └──────┬───────┘
        │ tokenize()  (protocol.cpp)
        ▼
 ┌──────────────┐  database.cpp: command dispatch, holds the mutex,
 │   Database   │  decides what gets written to the AOF
 └──┬────────┬──┘
    │        │
    ▼        ▼
 ┌───────┐ ┌───────────┐
 │ Store │ │ AofWriter │  aof.cpp: append + flush each write;
 └───────┘ └───────────┘  replayed line by line on startup
 store.cpp: unordered_map + doubly linked list (LRU) + optional expiry time
```

| File | Responsibility |
|------|----------------|
| `src/store.*` | Data structure: hash map + LRU list + TTL. Single-threaded, clock injected for testing |
| `src/protocol.*` | Parsing lines into arguments, quoting/escaping |
| `src/database.*` | Commands, locking, deciding what to persist |
| `src/aof.*` | Writing and replaying the append-only file |
| `src/server.*` | Sockets and client threads |
| `src/main.cpp` | Flags, startup, background expiry thread |
| `tests/test_main.cpp` | Unit tests |
| `bench/bench.py` | Load generator |

## Design decisions

**O(1) LRU with a hash map plus a linked list.** Each map entry stores an iterator to its node in a `std::list`. On access, `splice()` moves that node to the front in O(1), and eviction pops from the back in O(1).

**Store isn't thread-safe, Database is.** Keeping the lock out of the data structure keeps `Store` simple and testable. A single mutex in `Database` makes each command atomic. The AOF is written while the lock is held, so the file's order always matches the order of changes in memory.

**Absolute expiry times in the AOF.** `SET k v EX 10` is logged as `SET k v PXAT <unix-ms>`. Logging "10 seconds" would give the key a fresh 10 seconds after every restart.

**Evictions are logged as explicit `DEL`s.** LRU order depends on reads, which aren't logged. If replay let the store evict on its own, it could pick different victims. So replay runs with unlimited capacity and reapplies the exact `DEL`s instead.

**Two kinds of expiry.** Lazy: an expired key is deleted when someone touches it. Active: a background thread sweeps once per second, so keys nobody reads again still free their memory.

**Crash tolerance.** A crash mid-write can leave a half-written last line. Replay skips malformed lines instead of refusing to start.

## Benchmark

`bench/bench.py` runs N concurrent clients doing SET/GET round trips:

```bash
./build/minikv --no-aof &
python3 bench/bench.py --clients 8 --requests 40000
```

On a Linux cloud VM: **~26,000 req/s, p50 latency ~0.23 ms, p99 ~1.2 ms**, 0 errors (with AOF on). The Python client is likely the bottleneck. Run it on your own machine for your numbers.

## Limitations and next steps

- **Thread-per-client** doesn't scale to thousands of connections. Next step: a single-threaded event loop with `kqueue` (macOS) or `epoll` (Linux), which is the model Redis uses.
- **One global lock**: commands run one at a time. Sharding the map into N buckets with N locks would let them run in parallel.
- **AOF grows forever**: add compaction (rewrite the file from current state, like Redis's `BGREWRITEAOF`).
- **flush, not fsync**: survives a process crash, not a power loss. Could add an "fsync every second" mode.
- **Text protocol**: values can't contain raw newlines in replies. Redis's RESP protocol uses length prefixes to solve this.
- **No auth**: listens on localhost only for that reason.
