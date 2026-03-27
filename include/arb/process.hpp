#pragma once

#include <map>
#include <string>

namespace arb {

struct ProcessResult {
    int exit_code {0};
    std::string output;
};

[[nodiscard]] ProcessResult run_command(const std::string& command);
[[nodiscard]] std::map<std::string, std::string> parse_key_value_lines(const std::string& output);
[[nodiscard]] std::string shell_escape(const std::string& value);

}  // namespace arb
