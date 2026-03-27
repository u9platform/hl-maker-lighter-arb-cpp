#include "arb/process.hpp"

#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>

namespace arb {

ProcessResult run_command(const std::string& command) {
    std::array<char, 256> buffer {};
    std::string output;

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("popen failed for command: " + command);
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    const int status = pclose(pipe);
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    return ProcessResult {.exit_code = exit_code, .output = output};
}

std::map<std::string, std::string> parse_key_value_lines(const std::string& output) {
    std::map<std::string, std::string> values;
    std::istringstream input(output);
    std::string line;
    while (std::getline(input, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        values.emplace(line.substr(0, pos), line.substr(pos + 1));
    }
    return values;
}

std::string shell_escape(const std::string& value) {
    std::string escaped {"'"};
    for (const char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped += ch;
        }
    }
    escaped += "'";
    return escaped;
}

}  // namespace arb
