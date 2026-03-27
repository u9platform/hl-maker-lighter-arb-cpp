#include "arb/bridge_exchange.hpp"
#include "arb/process.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace arb {

namespace {

double parse_double(const std::map<std::string, std::string>& values, const std::string& key) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return 0.0;
    }
    return std::stod(it->second);
}

bool parse_ok(const std::map<std::string, std::string>& values) {
    const auto it = values.find("status");
    return it != values.end() && it->second == "ok";
}

}  // namespace

BridgeHyperliquidExchange::BridgeHyperliquidExchange(std::string bridge_script, std::string python_bin)
    : bridge_script_(std::move(bridge_script)), python_bin_(std::move(python_bin)) {}

Bbo BridgeHyperliquidExchange::get_bbo(const std::string& coin) {
    const auto output = run_bridge_command("hl orderbook --coin " + shell_escape(coin));
    const auto values = parse_key_value_lines(output);
    return Bbo {
        .bid = parse_double(values, "bid"),
        .ask = parse_double(values, "ask"),
        .quote_age_ms = static_cast<std::int64_t>(parse_double(values, "quote_age_ms")),
    };
}

HlLimitOrderAck BridgeHyperliquidExchange::place_limit_order(const HlLimitOrderRequest& request) {
    std::ostringstream cmd;
    cmd << "hl place-limit --coin " << shell_escape(request.coin)
        << " --side " << (request.is_buy ? "buy" : "sell")
        << " --price " << request.price
        << " --size " << request.size;
    if (request.post_only) {
        cmd << " --post-only";
    }
    if (request.dry_run) {
        cmd << " --dry-run";
    }
    const auto output = run_bridge_command(cmd.str());
    const auto values = parse_key_value_lines(output);
    return HlLimitOrderAck {
        .ok = parse_ok(values),
        .message = values.contains("message") ? values.at("message") : "",
        .oid = values.contains("oid") ? values.at("oid") : "",
    };
}

HlCancelAck BridgeHyperliquidExchange::cancel_order(const std::string& coin, const std::string& oid, bool dry_run) {
    std::ostringstream cmd;
    cmd << "hl cancel --coin " << shell_escape(coin) << " --oid " << shell_escape(oid);
    if (dry_run) {
        cmd << " --dry-run";
    }
    const auto output = run_bridge_command(cmd.str());
    const auto values = parse_key_value_lines(output);
    return HlCancelAck {
        .ok = parse_ok(values),
        .message = values.contains("message") ? values.at("message") : "",
        .oid = values.contains("oid") ? values.at("oid") : oid,
    };
}

HlReduceAck BridgeHyperliquidExchange::reduce_position(const std::string& coin, bool is_buy, double size, bool dry_run) {
    std::ostringstream cmd;
    cmd << "hl reduce --coin " << shell_escape(coin)
        << " --side " << (is_buy ? "buy" : "sell")
        << " --size " << size;
    if (dry_run) {
        cmd << " --dry-run";
    }
    const auto output = run_bridge_command(cmd.str());
    const auto values = parse_key_value_lines(output);
    return HlReduceAck {
        .ok = parse_ok(values),
        .message = values.contains("message") ? values.at("message") : "",
        .filled_size = parse_double(values, "filled_size"),
        .avg_fill_price = parse_double(values, "avg_fill_price"),
    };
}

std::string BridgeHyperliquidExchange::run_bridge_command(const std::string& args) const {
    const std::string command = shell_escape(python_bin_) + " " + shell_escape(bridge_script_) + " " + args;
    const ProcessResult result = run_command(command);
    if (result.exit_code != 0) {
        throw std::runtime_error("bridge command failed: " + result.output);
    }
    return result.output;
}

BridgeLighterExchange::BridgeLighterExchange(std::string bridge_script, std::string python_bin)
    : bridge_script_(std::move(bridge_script)), python_bin_(std::move(python_bin)) {}

Bbo BridgeLighterExchange::get_bbo(std::int64_t market_id) {
    const auto output = run_bridge_command("lighter orderbook --market-id " + std::to_string(market_id));
    const auto values = parse_key_value_lines(output);
    return Bbo {
        .bid = parse_double(values, "bid"),
        .ask = parse_double(values, "ask"),
        .quote_age_ms = static_cast<std::int64_t>(parse_double(values, "quote_age_ms")),
    };
}

LighterIocAck BridgeLighterExchange::place_ioc_order(const LighterIocRequest& request) {
    std::ostringstream cmd;
    cmd << "lighter place-ioc --side " << (request.is_ask ? "sell" : "buy")
        << " --price " << request.price
        << " --size " << request.size;
    if (request.dry_run) {
        cmd << " --dry-run";
    }
    const auto output = run_bridge_command(cmd.str());
    const auto values = parse_key_value_lines(output);
    return LighterIocAck {
        .ok = parse_ok(values),
        .message = values.contains("message") ? values.at("message") : "",
        .tx_hash = values.contains("tx_hash") ? values.at("tx_hash") : "",
    };
}

std::string BridgeLighterExchange::run_bridge_command(const std::string& args) const {
    const std::string command = shell_escape(python_bin_) + " " + shell_escape(bridge_script_) + " " + args;
    const ProcessResult result = run_command(command);
    if (result.exit_code != 0) {
        throw std::runtime_error("bridge command failed: " + result.output);
    }
    return result.output;
}

}  // namespace arb
