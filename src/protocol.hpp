// protocol.hpp: turning text lines into commands and back.
//
// The wire format is one command per line, words separated by spaces:
//     SET greeting "hello world"
// Double quotes let a value contain spaces. Inside quotes, \" \\ \n \r \t
// are escapes. The same format is used for the append-only file, so
// quote() and tokenize() must round-trip exactly.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace minikv {

// Splits a line into arguments. Throws std::invalid_argument on bad quoting.
std::vector<std::string> tokenize(const std::string& line);

// Wraps a string in quotes and escapes it so tokenize() gets it back exactly.
std::string quote(const std::string& s);

std::string to_upper(std::string s);

// Strict integer parse: the whole string must be a number. Returns false
// instead of throwing.
bool parse_int(const std::string& s, int64_t& out);

}  // namespace minikv
