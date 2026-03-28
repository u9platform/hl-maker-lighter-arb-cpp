#include "arb/ws_client.hpp"

#include <iostream>

namespace arb {

WsClient::WsClient(Config config) : config_(std::move(config)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_none);  // Exchange certs; skip pinning for speed.
}

WsClient::~WsClient() {
    close();
}

void WsClient::set_on_message(WsMessageCallback cb) {
    on_message_ = std::move(cb);
}

void WsClient::set_on_disconnect(WsDisconnectCallback cb) {
    on_disconnect_ = std::move(cb);
}

void WsClient::connect() {
    closing_.store(false, std::memory_order_relaxed);
    reconnect_count_ = 0;

    // Post the resolve to ioc_ so the thread picks it up.
    net::post(ioc_, [this] { do_resolve(); });

    // Start IO thread.
    io_thread_ = std::thread([this] {
        try {
            ioc_.run();
        } catch (const std::exception& e) {
            std::cerr << "[ws] io_context exception: " << e.what() << '\n';
        }
    });
}

void WsClient::send(const std::string& message) {
    net::post(ioc_, [this, msg = message] {
        if (!connected_.load(std::memory_order_relaxed)) {
            pending_sends_.push_back(msg);
            return;
        }
        ws_->async_write(
            net::buffer(msg),
            [](beast::error_code ec, std::size_t /*bytes*/) {
                if (ec) {
                    std::cerr << "[ws] write error: " << ec.message() << '\n';
                }
            }
        );
    });
}

void WsClient::close() {
    if (closing_.exchange(true, std::memory_order_relaxed)) {
        return;  // Already closing.
    }

    net::post(ioc_, [this] { do_close(); });

    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

bool WsClient::is_connected() const noexcept {
    return connected_.load(std::memory_order_relaxed);
}

// --- Async chain ---

void WsClient::do_resolve() {
    resolver_.async_resolve(
        config_.host, config_.port,
        [this](beast::error_code ec, net::ip::tcp::resolver::results_type results) {
            on_resolve(ec, std::move(results));
        }
    );
}

void WsClient::on_resolve(beast::error_code ec, net::ip::tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "[ws] resolve error: " << ec.message() << '\n';
        schedule_reconnect();
        return;
    }

    ws_ = std::make_unique<WsStream>(ioc_, ssl_ctx_);

    // Set TCP_NODELAY for low latency.
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(10));

    beast::get_lowest_layer(*ws_).async_connect(
        results,
        [this](beast::error_code ec2, net::ip::tcp::resolver::results_type::endpoint_type ep) {
            on_connect(ec2, ep);
        }
    );
}

void WsClient::on_connect(beast::error_code ec, net::ip::tcp::resolver::results_type::endpoint_type /*ep*/) {
    if (ec) {
        std::cerr << "[ws] connect error: " << ec.message() << '\n';
        schedule_reconnect();
        return;
    }

    // Set TCP_NODELAY.
    beast::get_lowest_layer(*ws_).socket().set_option(net::ip::tcp::no_delay(true));
    beast::get_lowest_layer(*ws_).expires_never();

    // Set SNI hostname for TLS.
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), config_.host.c_str())) {
        std::cerr << "[ws] SNI setup failed\n";
        schedule_reconnect();
        return;
    }

    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        [this](beast::error_code ec2) { on_ssl_handshake(ec2); }
    );
}

void WsClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[ws] ssl handshake error: " << ec.message() << '\n';
        schedule_reconnect();
        return;
    }

    // Disable permessage-deflate for latency.
    ws_->set_option(websocket::stream_base::decorator(
        [this](websocket::request_type& req) {
            req.set(boost::beast::http::field::host, config_.host);
            req.set(boost::beast::http::field::user_agent, "arb-cpp/0.1");
        }
    ));

    ws_->async_handshake(
        config_.host, config_.path,
        [this](beast::error_code ec2) { on_ws_handshake(ec2); }
    );
}

void WsClient::on_ws_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[ws] ws handshake error: " << ec.message() << '\n';
        schedule_reconnect();
        return;
    }

    connected_.store(true, std::memory_order_release);
    reconnect_count_ = 0;
    std::cerr << "[ws] connected to " << config_.host << config_.path << '\n';

    // Flush pending sends serially (Beast requires no concurrent async_write).
    // Copy and clear first, then chain writes.
    auto pending = std::move(pending_sends_);
    pending_sends_.clear();
    flush_pending(std::move(pending), 0);

    // Start read loop and ping timer.
    do_read();
    do_ping();
}

void WsClient::do_read() {
    ws_->async_read(
        read_buffer_,
        [this](beast::error_code ec, std::size_t bytes) {
            on_read(ec, bytes);
        }
    );
}

void WsClient::on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        connected_.store(false, std::memory_order_release);
        if (!closing_.load(std::memory_order_relaxed)) {
            std::cerr << "[ws] read error: " << ec.message() << '\n';
            if (on_disconnect_) {
                on_disconnect_(ec.message());
            }
            schedule_reconnect();
        }
        return;
    }

    if (on_message_) {
        const std::string msg = beast::buffers_to_string(read_buffer_.data());
        on_message_(msg);
    }
    read_buffer_.consume(read_buffer_.size());
    do_read();
}

void WsClient::do_ping() {
    if (closing_.load(std::memory_order_relaxed) || !connected_.load(std::memory_order_relaxed)) {
        return;
    }

    ping_timer_.expires_after(std::chrono::seconds(config_.ping_interval_sec));
    ping_timer_.async_wait([this](beast::error_code ec) {
        if (ec || closing_.load(std::memory_order_relaxed)) {
            return;
        }
        ws_->async_ping({}, [this](beast::error_code ec2) { on_ping(ec2); });
    });
}

void WsClient::on_ping(beast::error_code ec) {
    if (ec) {
        std::cerr << "[ws] ping error: " << ec.message() << '\n';
        return;
    }
    do_ping();
}

void WsClient::schedule_reconnect() {
    if (closing_.load(std::memory_order_relaxed)) {
        return;
    }
    if (config_.max_reconnect_attempts > 0 && reconnect_count_ >= config_.max_reconnect_attempts) {
        std::cerr << "[ws] max reconnect attempts reached, giving up\n";
        if (on_disconnect_) {
            on_disconnect_("max reconnect attempts");
        }
        return;
    }
    ++reconnect_count_;
    const int delay = config_.reconnect_delay_ms * reconnect_count_;
    std::cerr << "[ws] reconnecting in " << delay << "ms (attempt " << reconnect_count_ << ")\n";

    reconnect_timer_.expires_after(std::chrono::milliseconds(delay));
    reconnect_timer_.async_wait([this](beast::error_code ec) {
        if (ec || closing_.load(std::memory_order_relaxed)) {
            return;
        }
        do_resolve();
    });
}

void WsClient::flush_pending(std::vector<std::string> msgs, std::size_t idx) {
    if (idx >= msgs.size() || !connected_.load(std::memory_order_relaxed)) return;
    // Capture msgs by value (shared_ptr would be cleaner, but this is small)
    auto shared_msgs = std::make_shared<std::vector<std::string>>(std::move(msgs));
    ws_->async_write(
        net::buffer((*shared_msgs)[idx]),
        [this, shared_msgs, idx](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "[ws] pending write error: " << ec.message() << '\n';
                return;
            }
            // Chain next write
            flush_pending(std::move(*shared_msgs), idx + 1);
        }
    );
}

void WsClient::do_close() {
    ping_timer_.cancel();
    reconnect_timer_.cancel();

    if (ws_ && connected_.load(std::memory_order_relaxed)) {
        beast::error_code ec;
        ws_->close(websocket::close_code::normal, ec);
        connected_.store(false, std::memory_order_release);
    }

    ioc_.stop();
}

}  // namespace arb
