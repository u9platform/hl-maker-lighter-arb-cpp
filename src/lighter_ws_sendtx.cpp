#include "arb/lighter_ws_sendtx.hpp"

#include <chrono>
#include <thread>

namespace arb {

namespace {

std::string make_error_body(const std::string& message) {
    return "{\"status\":\"error\",\"error\":\"" + message + "\"}";
}

}  // namespace

LighterWsSendTxTransport::LighterWsSendTxTransport(Config config)
    : config_(std::move(config)),
      ws_(WsClient::Config {
          .host = config_.host,
          .path = config_.path,
          .ping_interval_sec = 15,
      }) {
    ws_.set_on_message([this](const std::string& msg) { on_message(msg); });
    ws_.set_on_disconnect([this](const std::string& reason) { on_disconnect(reason); });
}

LighterWsSendTxTransport::~LighterWsSendTxTransport() {
    stop();
}

void LighterWsSendTxTransport::start() {
    ws_.connect();
}

void LighterWsSendTxTransport::stop() {
    ws_.close();
}

bool LighterWsSendTxTransport::is_connected() const noexcept {
    return ws_.is_connected();
}

bool LighterWsSendTxTransport::wait_until_connected(int timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ws_.is_connected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ws_.is_connected();
}

std::string LighterWsSendTxTransport::send_tx(std::uint8_t tx_type, const std::string& tx_info_json) {
    std::unique_lock lock(request_mu_);
    if (request_inflight_) {
        return make_error_body("lighter_ws_sendtx_busy");
    }
    request_inflight_ = true;
    response_ready_ = false;
    response_body_.clear();

    const std::string request = "{\"type\":\"jsonapi/sendtx\",\"data\":{\"tx_type\":"
        + std::to_string(static_cast<unsigned int>(tx_type))
        + ",\"tx_info\":" + tx_info_json + "}}";
    ws_.send(request);

    const bool ready = request_cv_.wait_for(
        lock,
        std::chrono::milliseconds(config_.timeout_ms),
        [&] { return response_ready_; }
    );

    request_inflight_ = false;
    if (!ready) {
        response_body_ = make_error_body("lighter_ws_sendtx_timeout");
    }
    return response_body_;
}

void LighterWsSendTxTransport::on_message(const std::string& msg) {
    if (msg.find("\"type\":\"connected\"") != std::string::npos) {
        return;
    }

    std::lock_guard lock(request_mu_);
    if (!request_inflight_) {
        return;
    }
    response_body_ = msg;
    response_ready_ = true;
    request_cv_.notify_all();
}

void LighterWsSendTxTransport::on_disconnect(const std::string& reason) {
    std::lock_guard lock(request_mu_);
    if (!request_inflight_) {
        return;
    }
    response_body_ = make_error_body("lighter_ws_disconnect_" + reason);
    response_ready_ = true;
    request_cv_.notify_all();
}

}  // namespace arb
