#include "arb/public_exchange.hpp"
#include "arb/http.hpp"

#include <regex>
#include <stdexcept>

namespace arb {

namespace {

double first_match_as_double(const std::string& text, const std::regex& pattern) {
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        throw std::runtime_error("failed to parse expected field from response");
    }
    return std::stod(match[1].str());
}

}  // namespace

NativeHyperliquidExchange::NativeHyperliquidExchange(std::string api_url) : api_url_(std::move(api_url)) {}

Bbo NativeHyperliquidExchange::get_bbo(const std::string& coin) {
    const std::string payload = "{\"type\":\"l2Book\",\"coin\":\"" + coin + "\",\"nSigFigs\":5}";
    const HttpResponse response = http_post(
        api_url_ + "/info",
        payload,
        {{"Content-Type", "application/json"}}
    );
    if (response.status_code != 200) {
        throw std::runtime_error("HL orderbook request failed with status " + std::to_string(response.status_code));
    }

    const std::regex bid_pattern(R"REGEX("levels":\[\[\{"px":"([^"]+)")REGEX");
    const std::regex ask_pattern(R"REGEX(\],\[\{"px":"([^"]+)")REGEX");
    return Bbo {
        .bid = first_match_as_double(response.body, bid_pattern),
        .ask = first_match_as_double(response.body, ask_pattern),
        .quote_age_ms = 0,
    };
}

HlLimitOrderAck NativeHyperliquidExchange::place_limit_order(const HlLimitOrderRequest&) {
    throw std::runtime_error("NativeHyperliquidExchange trading is not implemented yet");
}

HlCancelAck NativeHyperliquidExchange::cancel_order(const std::string&, const std::string&, bool) {
    throw std::runtime_error("NativeHyperliquidExchange cancel is not implemented yet");
}

HlReduceAck NativeHyperliquidExchange::reduce_position(const std::string&, bool, double, bool) {
    throw std::runtime_error("NativeHyperliquidExchange reduce is not implemented yet");
}

NativeLighterExchange::NativeLighterExchange(std::string api_url) : api_url_(std::move(api_url)) {}

Bbo NativeLighterExchange::get_bbo(std::int64_t market_id) {
    const HttpResponse response = http_get(
        api_url_ + "/api/v1/orderBookOrders?market_id=" + std::to_string(market_id) + "&limit=5"
    );
    if (response.status_code != 200) {
        throw std::runtime_error("Lighter orderbook request failed with status " + std::to_string(response.status_code));
    }

    const std::regex ask_pattern(R"REGEX("asks":\[\{[^}]*"price":"([^"]+)")REGEX");
    const std::regex bid_pattern(R"REGEX("bids":\[\{[^}]*"price":"([^"]+)")REGEX");
    return Bbo {
        .bid = first_match_as_double(response.body, bid_pattern),
        .ask = first_match_as_double(response.body, ask_pattern),
        .quote_age_ms = 0,
    };
}

LighterIocAck NativeLighterExchange::place_ioc_order(const LighterIocRequest&) {
    throw std::runtime_error("NativeLighterExchange trading is not implemented yet");
}

}  // namespace arb
