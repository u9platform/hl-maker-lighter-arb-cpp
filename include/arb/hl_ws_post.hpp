#pragma once

#include "arb/ws_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace arb {

class HlWsPostTransport {
  public:
    struct Config {
        std::string host {"api.hyperliquid.xyz"};
        std::string path {"/ws"};
        int timeout_ms {5000};
    };

    HlWsPostTransport() : HlWsPostTransport(Config{}) {}
    explicit HlWsPostTransport(Config config);
    ~HlWsPostTransport();

    HlWsPostTransport(const HlWsPostTransport&) = delete;
    HlWsPostTransport& operator=(const HlWsPostTransport&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] bool wait_until_connected(int timeout_ms) const;

    [[nodiscard]] std::string post_action(const std::string& payload_json);

  private:
    struct PendingResponse {
        std::mutex mu;
        std::condition_variable cv;
        bool ready {false};
        std::string body;
    };

    void on_message(const std::string& msg);
    void on_disconnect(const std::string& reason);

    Config config_;
    WsClient ws_;
    mutable std::mutex pending_mu_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingResponse>> pending_;
    std::atomic<std::uint64_t> next_id_ {1};
};

}  // namespace arb
