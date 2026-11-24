#include "protocol.hpp"

#include <cctype>
#include <charconv>
#include <stdexcept>

namespace minikv {

static bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> args;
    size_t i = 0;
    const size_t n = line.size();

    while (true) {
        while (i < n && is_space(line[i])) ++i;  // skip spaces between words
        if (i >= n) break;

        std::string token;
        if (line[i] == '"') {
            ++i;  // skip opening quote
            bool closed = false;
            while (i < n) {
                char c = line[i++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\' && i < n) {
                    char esc = line[i++];
                    switch (esc) {
                        case 'n': token += '\n'; break;
                        case 'r': token += '\r'; break;
                        case 't': token += '\t'; break;
                        default: token += esc;  // \" and \\ (and anything else) map to the char itself
                    }
                } else {
                    token += c;
                }
            }
            if (!closed) throw std::invalid_argument("unbalanced quotes");
            if (i < n && !is_space(line[i]))
                throw std::invalid_argument("closing quote must be followed by a space");
        } else {
            while (i < n && !is_space(line[i])) token += line[i++];
        }
        args.push_back(std::move(token));
    }
    return args;
}

std::string quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    out += '"';
    return out;
}

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool parse_int(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end;
}

}  // namespace minikv
