#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace arb {

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

using WsStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

/// Callback invoked on the IO thread when a text message arrives.
using WsMessageCallback = std::function<void(const std::string& message)>;

/// Callback invoked on the IO thread when the connection drops.
using WsDisconnectCallback = std::function<void(const std::string& reason)>;

/// A single TLS WebSocket connection with async read loop.
/// Runs its own io_context on a dedicated thread.
class WsClient {
  public:
    struct Config {
        std::string host;
        std::string port {"443"};
        std::string path {"/ws"};
        int ping_interval_sec {20};
        int reconnect_delay_ms {1000};
        int max_reconnect_attempts {0};  // 0 = unlimited
    };

    explicit WsClient(Config config);
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    /// Set callbacks before calling connect().
    void set_on_message(WsMessageCallback cb);
    void set_on_disconnect(WsDisconnectCallback cb);

    /// Initiate connection + read loop on background thread.
    void connect();

    /// Send a text message (thread-safe, posts to the IO strand).
    void send(const std::string& message);

    /// Graceful shutdown.
    void close();

    [[nodiscard]] bool is_connected() const noexcept;

  private:
    void do_resolve();
    void on_resolve(beast::error_code ec, net::ip::tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, net::ip::tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_ws_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_ping();
    void on_ping(beast::error_code ec);
    void schedule_reconnect();
    void do_close();

    Config config_;
    net::io_context ioc_;
    ssl::context ssl_ctx_ {ssl::context::tlsv12_client};
    net::ip::tcp::resolver resolver_ {ioc_};
    std::unique_ptr<WsStream> ws_;
    beast::flat_buffer read_buffer_;
    net::steady_timer ping_timer_ {ioc_};
    net::steady_timer reconnect_timer_ {ioc_};
    std::thread io_thread_;

    std::atomic<bool> connected_ {false};
    std::atomic<bool> closing_ {false};
    int reconnect_count_ {0};

    WsMessageCallback on_message_;
    WsDisconnectCallback on_disconnect_;

    /// Messages queued to send after subscribe.
    std::vector<std::string> pending_sends_;
};

}  // namespace arb
