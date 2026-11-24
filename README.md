# minikv

A small Redis-style key-value store written in C++. You connect to it over TCP, store and read values with commands like `SET` and `GET`, give keys an expiration time, and your data is still there after a restart.

I built this to understand data structures on a deeper level than solving problems in class. Using a hash map is easy. I wanted to see what it takes to turn one into something real: what happens when memory fills up, how keys expire on time, and how you avoid losing everything when the program crashes. Redis handles all of that, so I used it as a model and built a much smaller version.

The trickiest part was making sure the data comes back exactly the same after a restart, including which keys had been evicted.

## How it works

The core is a hash map combined with a doubly linked list. The map gives O(1) lookups, and the list keeps keys in order of when they were last used. Each map entry stores a pointer (iterator) to its spot in the list, so moving a key to the front on every access is O(1), and so is evicting the least recently used key from the back when the store is full.

Keys with a TTL get deleted in two ways: when someone tries to read them after they expire, and by a background thread that sweeps once a second.

Every command that changes data is appended to a log file (`minikv.aof`). On startup the server replays that file from the top to rebuild its state.

Each client connection gets its own thread, and a single mutex makes sure only one command runs at a time.

## Running it

You need a C++17 compiler and CMake (on a Mac: `xcode-select --install` and `brew install cmake`).

```bash
cmake -S . -B build
cmake --build build
./build/minikv_tests     # run the tests
./build/minikv           # start the server on 127.0.0.1:6380
```

Then in another terminal:

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
```

Options: `--port N` (default 6380), `--max-keys N` (default 100000), `--aof PATH` (default `minikv.aof`), `--no-aof` to keep everything in memory only.

## Commands

| Command | What it does |
|---------|--------------|
| `PING` | returns `PONG` |
| `SET key value [EX sec \| PX ms]` | stores a value, optionally with an expiration |
| `GET key` | returns the value or `(nil)` |
| `DEL key [key ...]` | deletes keys, returns how many were deleted |
| `EXISTS key [key ...]` | returns how many of the keys exist |
| `EXPIRE key sec` | sets an expiration on an existing key |
| `TTL key` / `PTTL key` | time left in seconds / ms (`-1` = no expiration, `-2` = no key) |
| `DBSIZE` | number of keys |
| `INFO` | key count, capacity, evictions |
| `FLUSHALL` | deletes everything |
| `QUIT` | closes the connection |

Values with spaces go in double quotes.

## Code layout

| File | What's in it |
|------|--------------|
| `src/store.*` | the hash map + LRU list + TTL logic |
| `src/protocol.*` | splitting a line of text into arguments |
| `src/database.*` | command handling, locking, deciding what to log |
| `src/aof.*` | writing and replaying the log file |
| `src/server.*` | sockets and client threads |
| `src/main.cpp` | command-line flags and startup |
| `tests/test_main.cpp` | unit tests |
| `bench/bench.py` | load-testing script |

## A couple of design notes

Expiration times are saved to the log as absolute timestamps, not as "10 seconds from now." Otherwise every restart would give the key a fresh 10 seconds.

Evictions are written to the log as explicit deletes. Which key gets evicted depends on which keys were read recently, and reads aren't logged. If the server re-ran evictions on its own during replay, it could throw out different keys than it did the first time.

## Benchmark

With 8 clients sending SET/GET requests (`python3 bench/bench.py --clients 8 --requests 40000`), on my MacBook it handled about 87,000 requests per second, with a median latency around 0.08 ms and zero errors.

## What I'd improve next

- Replace thread-per-client with an event loop (`kqueue`/`epoll`) so it can handle thousands of connections
- Split the store into shards with separate locks so commands can run in parallel
- Compact the log file so it doesn't grow forever
