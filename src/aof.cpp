#include "aof.hpp"

#include <stdexcept>

#include "protocol.hpp"

namespace minikv {

AofWriter::AofWriter(const std::string& path) : out_(path, std::ios::app | std::ios::binary) {
    if (!out_) throw std::runtime_error("cannot open AOF file: " + path);
}

void AofWriter::append(const std::vector<std::string>& args) {
    std::string line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) line += ' ';
        line += quote(args[i]);  // always quote so values with spaces/newlines are safe
    }
    line += '\n';
    out_ << line;
    // flush() hands the bytes to the OS. That survives the minikv process
    // crashing, but not a power cut. For that you'd also call fsync(), which
    // is much slower. Redis's default ("appendfsync everysec") fsyncs once a
    // second as a middle ground.
    out_.flush();
    if (!out_) throw std::runtime_error("failed to write to AOF");
}

ReplayResult replay_aof(const std::string& path,
                        const std::function<bool(const std::vector<std::string>&)>& apply) {
    ReplayResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) return result;  // no file yet: first run

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto args = tokenize(line);
            if (!args.empty() && apply(args)) {
                ++result.applied;
            } else {
                ++result.skipped;
            }
        } catch (const std::exception&) {
            ++result.skipped;  // e.g. truncated line with an unclosed quote
        }
    }
    return result;
}

}  // namespace minikv
