// server.hpp: the TCP networking layer.
//
// Model: one thread per connected client ("thread-per-connection"). Simple
// to read and fine for dozens or hundreds of clients. At thousands of
// clients, threads cost too much memory and context switching, and real
// servers (Redis, nginx) switch to an event loop with epoll (Linux) or
// kqueue (macOS) that serves every client from one thread.
#pragma once

#include <cstdint>
#include <string>

#include "database.hpp"

namespace minikv {

class Server {
public:
    Server(Database& db, uint16_t port);

    // Binds, listens, and accepts clients forever. Throws on setup errors.
    void run();

private:
    void handle_client(int client_fd);
    std::string handle_line(const std::string& line, bool& quit);

    Database& db_;
    uint16_t port_;
};

}  // namespace minikv
