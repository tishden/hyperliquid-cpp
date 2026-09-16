#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hltest {

inline std::string fixture(const std::string& name) {
    std::ifstream in(std::string{HL_FIXTURES_DIR} + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

inline std::vector<std::string> fixtureLines(const std::string& name) {
    std::ifstream in(std::string{HL_FIXTURES_DIR} + "/" + name);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace hltest
