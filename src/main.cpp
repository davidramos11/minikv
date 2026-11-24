// main.cpp: parses command-line flags, loads data, starts the server.
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "database.hpp"
#include "server.hpp"

namespace {

struct Options {
    uint16_t port = 6380;  // Redis uses 6379; one higher avoids clashing with a real Redis
    size_t max_keys = 100000;
    std::string aof_path = "minikv.aof";
};

void print_usage() {
    std::cout << "Usage: minikv [--port N] [--max-keys N] [--aof PATH] [--no-aof]\n"
                 "  --port N       TCP port to listen on (default 6380)\n"
                 "  --max-keys N   evict least-recently-used keys past this many (default 100000)\n"
                 "  --aof PATH     append-only file for persistence (default minikv.aof)\n"
                 "  --no-aof       keep data in memory only\n";
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--port") {
            int port = std::stoi(next());
            if (port < 1 || port > 65535) throw std::invalid_argument("port must be 1-65535");
            opts.port = static_cast<uint16_t>(port);
        } else if (arg == "--max-keys") {
            long long n = std::stoll(next());
            if (n < 1) throw std::invalid_argument("--max-keys must be at least 1");
            opts.max_keys = static_cast<size_t>(n);
        } else if (arg == "--aof") {
            opts.aof_path = next();
        } else if (arg == "--no-aof") {
            opts.aof_path.clear();
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opts = parse_args(argc, argv);

        // By default, writing to a socket whose client has disconnected
        // sends SIGPIPE, which kills the whole process. Ignoring it makes
        // send() return an error instead, which we handle per client.
        std::signal(SIGPIPE, SIG_IGN);

        minikv::Database db(opts.max_keys, opts.aof_path);
        auto replay = db.load();
        if (!opts.aof_path.empty()) {
            std::cout << "Replayed " << replay.applied << " commands from " << opts.aof_path;
            if (replay.skipped > 0) std::cout << " (skipped " << replay.skipped << " bad lines)";
            std::cout << std::endl;
        }

        // Background "active expiry": once a second, delete keys whose TTL
        // has passed, so memory is freed even if nobody reads them again.
        std::thread([&db] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                db.sweep_expired();
            }
        }).detach();

        minikv::Server(db, opts.port).run();
    } catch (const std::exception& e) {
        std::cerr << "minikv: " << e.what() << std::endl;
        print_usage();
        return 1;
    }
}
