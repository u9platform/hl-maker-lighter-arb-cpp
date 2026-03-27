#include "arb/native_trading.hpp"

#include <iostream>

int main() {
    arb::NativeHyperliquidTrading hl(arb::HyperliquidConfig {
        .api_url = "https://api.hyperliquid.xyz",
        .private_key = "",
        .coin = "HYPE",
    });
    arb::NativeLighterTrading lighter(arb::LighterConfig {
        .api_url = "https://mainnet.zklighter.elliot.ai",
        .api_private_key = "",
        .account_index = 0,
        .api_key_index = 0,
        .market_index = 24,
    });

    const auto hl_bbo = hl.get_bbo("HYPE");
    const auto lt_bbo = lighter.get_bbo(24);
    const auto hl_ack = hl.place_limit_order(arb::HlLimitOrderRequest {
        .coin = "HYPE",
        .is_buy = true,
        .price = hl_bbo.bid,
        .size = 1.0,
        .post_only = true,
        .dry_run = true,
    });
    const auto lt_ack = lighter.place_ioc_order(arb::LighterIocRequest {
        .is_ask = true,
        .price = lt_bbo.bid,
        .size = 1.0,
        .dry_run = true,
    });

    std::cout << "hl_bid=" << hl_bbo.bid << " hl_ask=" << hl_bbo.ask << '\n';
    std::cout << "lt_bid=" << lt_bbo.bid << " lt_ask=" << lt_bbo.ask << '\n';
    std::cout << "hl_dry_run_ok=" << hl_ack.ok << " oid=" << hl_ack.oid << '\n';
    std::cout << "lt_dry_run_ok=" << lt_ack.ok << " tx=" << lt_ack.tx_hash << '\n';
    return 0;
}
