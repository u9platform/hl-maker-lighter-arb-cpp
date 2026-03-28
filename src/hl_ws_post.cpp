#include "arb/hl_ws_post.hpp"

#include <chrono>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace arb {

namespace {

std::optional<std::uint64_t> parse_post_id(const std::string& msg) {
    if (msg.find("\"channel\":\"post\"") == std::string::npos) {
        return std::nullopt;
    }
    const auto id_pos = msg.find("\"id\":");
    if (id_pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t start = id_pos + 5;
    while (start < msg.size() && msg[start] == ' ') {
        ++start;
    }
    std::size_t end = start;
    while (end < msg.size() && std::isdigit(static_cast<unsigned char>(msg[end]))) {
        ++end;
    }
    if (end == start) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::stoull(msg.substr(start, end - start)));
}

std::optional<std::string> extract_json_value(const std::string& msg, const std::string& key) {
    const auto key_pos = msg.find(key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t start = key_pos + key.size();
    while (start < msg.size() && (msg[start] == ' ' || msg[start] == ':')) {
        ++start;
    }
    if (start >= msg.size()) {
        return std::nullopt;
    }

    if (msg[start] == '"') {
        std::size_t pos = start + 1;
        bool escaped = false;
        while (pos < msg.size()) {
            const char ch = msg[pos];
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                return msg.substr(start, pos - start + 1);
            }
            ++pos;
        }
        return std::nullopt;
    }

    if (msg[start] == '{' || msg[start] == '[') {
        const char open = msg[start];
        const char close = open == '{' ? '}' : ']';
        std::size_t pos = start;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        while (pos < msg.size()) {
            const char ch = msg[pos];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
            } else {
                if (ch == '"') {
                    in_string = true;
                } else if (ch == open) {
                    ++depth;
                } else if (ch == close) {
                    --depth;
                    if (depth == 0) {
                        return msg.substr(start, pos - start + 1);
                    }
                }
            }
            ++pos;
        }
        return std::nullopt;
    }

    std::size_t end = start;
    while (end < msg.size() && msg[end] != ',' && msg[end] != '}' && msg[end] != ']') {
        ++end;
    }
    return msg.substr(start, end - start);
}

std::string make_error_body(const std::string& message) {
    return "{\"status\":\"error\",\"error\":\"" + message + "\"}";
}

}  // namespace

HlWsPostTransport::HlWsPostTransport(Config config)
    : config_(std::move(config)),
      ws_(WsClient::Config {
          .host = config_.host,
          .path = config_.path,
          .ping_interval_sec = 20,
      }) {
    ws_.set_on_message([this](const std::string& msg) { on_message(msg); });
    ws_.set_on_disconnect([this](const std::string& reason) { on_disconnect(reason); });
}

HlWsPostTransport::~HlWsPostTransport() {
    stop();
}

void HlWsPostTransport::start() {
    ws_.connect();
}

void HlWsPostTransport::stop() {
    ws_.close();
}

bool HlWsPostTransport::is_connected() const noexcept {
    return ws_.is_connected();
}

bool HlWsPostTransport::wait_until_connected(int timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ws_.is_connected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ws_.is_connected();
}

std::string HlWsPostTransport::post_action(const std::string& payload_json) {
    const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    auto pending = std::make_shared<PendingResponse>();
    {
        std::lock_guard lock(pending_mu_);
        pending_.emplace(id, pending);
    }

    std::ostringstream req;
    req << "{\"method\":\"post\",\"id\":" << id
        << ",\"request\":{\"type\":\"action\",\"payload\":" << payload_json << "}}";
    ws_.send(req.str());

    std::unique_lock lock(pending->mu);
    const bool ready = pending->cv.wait_for(
        lock,
        std::chrono::milliseconds(config_.timeout_ms),
        [&] { return pending->ready; }
    );

    if (!ready) {
        std::lock_guard pending_lock(pending_mu_);
        pending_.erase(id);
        return make_error_body("ws_post_timeout");
    }
    return pending->body;
}

void HlWsPostTransport::on_message(const std::string& msg) {
    const auto id = parse_post_id(msg);
    if (!id.has_value()) {
        return;
    }

    std::shared_ptr<PendingResponse> pending;
    {
        std::lock_guard lock(pending_mu_);
        const auto it = pending_.find(*id);
        if (it == pending_.end()) {
            return;
        }
        pending = it->second;
        pending_.erase(it);
    }

    std::string body = make_error_body("ws_post_missing_payload");
    if (const auto type = extract_json_value(msg, "\"type\""); type.has_value()) {
        if (*type == "\"error\"") {
            if (const auto payload = extract_json_value(msg, "\"payload\""); payload.has_value()) {
                body = make_error_body(*payload);
            } else {
                body = make_error_body("ws_post_error");
            }
        } else if (const auto payload = extract_json_value(msg, "\"payload\""); payload.has_value()) {
            body = *payload;
        }
    }

    {
        std::lock_guard lock(pending->mu);
        pending->body = std::move(body);
        pending->ready = true;
    }
    pending->cv.notify_all();
}

void HlWsPostTransport::on_disconnect(const std::string& reason) {
    std::lock_guard lock(pending_mu_);
    for (auto& [_, pending] : pending_) {
        {
            std::lock_guard pending_lock(pending->mu);
            pending->body = make_error_body("ws_disconnect_" + reason);
            pending->ready = true;
        }
        pending->cv.notify_all();
    }
    pending_.clear();
}

}  // namespace arb
