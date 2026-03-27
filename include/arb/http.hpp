#pragma once

#include <map>
#include <string>

namespace arb {

struct HttpResponse {
    long status_code {0};
    std::string body;
};

[[nodiscard]] HttpResponse http_get(
    const std::string& url,
    const std::map<std::string, std::string>& headers = {}
);

[[nodiscard]] HttpResponse http_post(
    const std::string& url,
    const std::string& payload,
    const std::map<std::string, std::string>& headers = {}
);

}  // namespace arb
