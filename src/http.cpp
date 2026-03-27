#include "arb/http.hpp"

#include <curl/curl.h>

#include <memory>
#include <stdexcept>

namespace arb {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* output = static_cast<std::string*>(userdata);
    output->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpResponse perform_request(
    const std::string& url,
    const std::string* payload,
    const std::map<std::string, std::string>& headers
) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl, curl_easy_cleanup);
    std::string body;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_guard(header_list, curl_slist_free_all);
    if (header_list != nullptr) {
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, header_list);
    }

    if (payload != nullptr) {
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload->c_str());
    }

    const CURLcode code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("curl request failed: ") + curl_easy_strerror(code));
    }

    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    return HttpResponse {.status_code = status, .body = body};
}

}  // namespace

HttpResponse http_get(const std::string& url, const std::map<std::string, std::string>& headers) {
    return perform_request(url, nullptr, headers);
}

HttpResponse http_post(const std::string& url, const std::string& payload, const std::map<std::string, std::string>& headers) {
    return perform_request(url, &payload, headers);
}

}  // namespace arb
