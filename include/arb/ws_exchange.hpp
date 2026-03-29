#pragma once

#include "arb/exchange.hpp"
#include "arb/market_feed.hpp"
#include "arb/native_trading.hpp"

namespace arb {

/// HL exchange adapter that uses WS-cached BBO for reads, and native signing for writes.
class WsHyperliquidExchange final : public HyperliquidExchange {
  public:
    WsHyperliquidExchange(AtomicBbo& bbo, NativeHyperliquidTrading& trading)
        : bbo_(bbo), trading_(trading) {}

    Bbo get_bbo(const std::string& /*coin*/) override {
        return bbo_.load();
    }

    HlLimitOrderAck place_limit_order(const HlLimitOrderRequest& request) override {
        return trading_.place_limit_order(request);
    }

    HlIocOrderAck place_ioc_order(const HlIocOrderRequest& request) override {
        return trading_.place_ioc_order(request);
    }

    HlCancelAck cancel_order(const std::string& coin, const std::string& oid, bool dry_run) override {
        return trading_.cancel_order(coin, oid, dry_run);
    }

    HlReduceAck reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) override {
        return trading_.reduce_position(coin, is_buy, size, dry_run);
    }

  private:
    AtomicBbo& bbo_;
    NativeHyperliquidTrading& trading_;
};

/// Lighter exchange adapter that uses WS-cached BBO for reads, and native signing for writes.
class WsLighterExchange final : public LighterExchange {
  public:
    WsLighterExchange(AtomicBbo& bbo, NativeLighterTrading& trading)
        : bbo_(bbo), trading_(trading) {}

    Bbo get_bbo(std::int64_t /*market_id*/) override {
        return bbo_.load();
    }

    LighterLimitOrderAck place_limit_order(const LighterLimitOrderRequest& request) override {
        return trading_.place_limit_order(request);
    }

    LighterCancelAck cancel_order(std::int64_t order_index, bool dry_run) override {
        return trading_.cancel_order(order_index, dry_run);
    }

    LighterIocAck place_ioc_order(const LighterIocRequest& request) override {
        return trading_.place_ioc_order(request);
    }

  private:
    AtomicBbo& bbo_;
    NativeLighterTrading& trading_;
};

}  // namespace arb
