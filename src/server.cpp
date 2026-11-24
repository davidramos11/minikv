#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <system_error>
#include <thread>

#include "protocol.hpp"

namespace minikv {

namespace {

constexpr size_t kMaxLineBytes = 1 << 20;  // 1 MB: stops a client from making us buffer forever

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// send() may write fewer bytes than asked, so loop until everything is sent.
bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal: retry
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

Server::Server(Database& db, uint16_t port) : db_(db), port_(port) {}

void Server::run() {
    // 1. socket(): create a TCP endpoint.
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) throw_errno("socket");

    // Allows restarting the server right away instead of waiting ~60s for
    // the old socket to leave the TIME_WAIT state.
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 2. bind(): attach it to 127.0.0.1:<port>. Localhost only, so nobody
    // else on your network can reach it (there is no password).
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);  // htons: host to network byte order (big-endian)
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) throw_errno("bind");

    // 3. listen(): start accepting. 128 = backlog of not-yet-accepted connections.
    if (::listen(listen_fd, 128) < 0) throw_errno("listen");
    std::cout << "minikv listening on 127.0.0.1:" << port_ << std::endl;

    // 4. accept() loop: each call blocks until a client connects and returns
    // a new socket just for that client.
    while (true) {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno != EINTR) std::perror("accept");
            continue;
        }
        // detach(): the thread cleans itself up when the client disconnects.
        std::thread(&Server::handle_client, this, client_fd).detach();
    }
}

std::string Server::handle_line(const std::string& line, bool& quit) {
    std::vector<std::string> args;
    try {
        args = tokenize(line);
    } catch (const std::exception& e) {
        return std::string("ERR ") + e.what();
    }
    if (args.empty()) return "";  // blank line: no reply
    if (to_upper(args[0]) == "QUIT") {
        quit = true;
        return "OK";
    }
    try {
        return db_.execute(args);
    } catch (const std::exception& e) {
        return std::string("ERR ") + e.what();
    }
}

void Server::handle_client(int fd) {
    // TCP is a byte stream, not a message stream: one recv() might return
    // half a command, or three commands at once. So we buffer bytes and only
    // process complete lines (ending in '\n').
    std::string buffer;
    char chunk[4096];
    bool quit = false;

    while (!quit) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) break;  // client closed the connection
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        buffer.append(chunk, static_cast<size_t>(n));

        size_t start = 0;
        size_t newline;
        while (!quit && (newline = buffer.find('\n', start)) != std::string::npos) {
            std::string line = buffer.substr(start, newline - start);
            start = newline + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();  // handle \r\n too

            std::string reply = handle_line(line, quit);
            if (reply.empty() && !quit) continue;
            if (!send_all(fd, reply + "\n")) quit = true;
        }
        buffer.erase(0, start);  // drop the lines we've handled, keep any partial line

        if (buffer.size() > kMaxLineBytes) {
            send_all(fd, "ERR line too long\n");
            break;
        }
    }
    ::close(fd);
}

}  // namespace minikv
