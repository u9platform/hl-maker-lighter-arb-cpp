#pragma once

#include "arb/ws_client.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace arb {

class LighterWsSendTxTransport {
  public:
    struct Config {
        std::string host {"mainnet.zklighter.elliot.ai"};
        std::string path {"/stream"};
        int timeout_ms {5000};
    };

    explicit LighterWsSendTxTransport(Config config = {});
    ~LighterWsSendTxTransport();

    LighterWsSendTxTransport(const LighterWsSendTxTransport&) = delete;
    LighterWsSendTxTransport& operator=(const LighterWsSendTxTransport&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] bool wait_until_connected(int timeout_ms) const;

    [[nodiscard]] std::string send_tx(std::uint8_t tx_type, const std::string& tx_info_json);

  private:
    void on_message(const std::string& msg);
    void on_disconnect(const std::string& reason);

    Config config_;
    WsClient ws_;

    mutable std::mutex request_mu_;
    std::condition_variable request_cv_;
    bool request_inflight_ {false};
    bool response_ready_ {false};
    std::string response_body_;
};

}  // namespace arb
