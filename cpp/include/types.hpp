#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <queue> // Added for order_queue

namespace hft {

// Timing and latency types
using hft_clock_t = std::chrono::steady_clock;
using timestamp_t = std::chrono::time_point<hft_clock_t>;
using duration_us_t = std::chrono::microseconds;

// Price and quantity types - use fixed point for precision
using price_t = double;
using quantity_t = double;
using position_t = double;

// Order side enumeration
enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

// Order status enumeration
enum class OrderStatus : uint8_t {
    PENDING = 0,
    ACTIVE = 1,
    PARTIALLY_FILLED = 2,
    FILLED = 3,
    CANCELLED = 4,
    REJECTED = 5
};

// Risk check results
enum class RiskCheckResult : uint8_t {
    APPROVED = 0,
    POSITION_LIMIT_EXCEEDED = 1,
    DAILY_LOSS_LIMIT_EXCEEDED = 2,
    DRAWDOWN_LIMIT_EXCEEDED = 3,
    CONCENTRATION_RISK = 4,
    VAR_LIMIT_EXCEEDED = 5,
    ORDER_RATE_LIMIT_EXCEEDED = 6,
    LATENCY_LIMIT_EXCEEDED = 7,
    CRITICAL_BREACH = 8
};

// Matching result for order execution
enum class MatchResult : uint8_t {
    NO_MATCH = 0,
    PARTIAL_FILL = 1,
    FULL_FILL = 2,
    REJECTED = 3
};

// Market data structures
struct PriceLevel {
    price_t price;
    quantity_t quantity;
    quantity_t total_quantity;
    std::queue<uint64_t> order_queue;  // Queue of order IDs at this price level
    timestamp_t last_update;

    PriceLevel() : price(0.0), quantity(0.0), total_quantity(0.0), last_update() {}
    PriceLevel(price_t p, quantity_t q) : price(p), quantity(q), total_quantity(q), last_update() {}
    PriceLevel(price_t p) : price(p), quantity(0.0), total_quantity(0.0), last_update() {}
    PriceLevel(price_t p, quantity_t q, timestamp_t t) : price(p), quantity(q), total_quantity(q), last_update(t) {}

    void add_order(uint64_t order_id, quantity_t qty) {
        order_queue.push(order_id);
        total_quantity += qty;
        last_update = hft_clock_t::now();
    }

    void remove_order(quantity_t qty) {
        total_quantity -= qty;
        last_update = hft_clock_t::now();
    }
};

struct OrderbookSnapshot {
    timestamp_t timestamp;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    price_t mid_price;
    
    OrderbookSnapshot() : mid_price(0.0) {}
    
    void calculate_mid_price() {
        if (!bids.empty() && !asks.empty()) {
            mid_price = (bids[0].price + asks[0].price) / 2.0;
        }
    }
};

struct Trade {
    timestamp_t timestamp;
    price_t price;
    quantity_t quantity;
    Side side;
    
    Trade() : price(0.0), quantity(0.0), side(Side::BUY) {}
    Trade(timestamp_t ts, price_t p, quantity_t q, Side s) 
        : timestamp(ts), price(p), quantity(q), side(s) {}
};

// Market depth structure for Level 2 data
struct MarketDepth {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    uint32_t depth_levels;
    timestamp_t timestamp;
    
    MarketDepth(uint32_t levels = 10);  // Declaration only, definition comes later
};

/**
 * Trade execution information
 */
struct TradeExecution {
    uint64_t trade_id;
    uint64_t aggressor_order_id;
    uint64_t passive_order_id;
    price_t price;
    quantity_t quantity;
    Side aggressor_side;
    timestamp_t timestamp;
    
    TradeExecution();  // Declaration only, definition comes later
};

// Order book statistics structure
struct OrderBookStats {
    uint64_t total_orders_processed;
    uint64_t total_trades;
    uint64_t total_updates;  // For market data updates
    quantity_t total_volume;
    timestamp_t last_trade_time;
    double avg_spread_bps;
    
    OrderBookStats() : total_orders_processed(0), total_trades(0), total_updates(0),
                       total_volume(0.0), avg_spread_bps(0.0) {}
};

// Order management structures
struct Order {
    uint64_t order_id;
    Side side;
    price_t price;
    quantity_t quantity;  // Current order quantity (for immediate use)
    quantity_t original_quantity;
    quantity_t remaining_quantity;
    quantity_t queue_ahead;
    OrderStatus status;
    timestamp_t entry_time;
    timestamp_t last_update_time;
    price_t mid_price_at_entry;
    
    Order() : order_id(0), side(Side::BUY), price(0.0), quantity(0.0),
              original_quantity(0.0), remaining_quantity(0.0), 
              queue_ahead(0.0), status(OrderStatus::PENDING),
              mid_price_at_entry(0.0) {}
};

// Position and PnL tracking
struct Position {
    position_t size;
    price_t average_price;
    double unrealized_pnl;
    double realized_pnl;
    
    Position() : size(0.0), average_price(0.0), 
                 unrealized_pnl(0.0), realized_pnl(0.0) {}
};

// Risk limits configuration
struct RiskLimits {
    position_t max_position;
    double max_daily_loss;
    double max_drawdown;
    double position_concentration;
    double var_limit;
    uint32_t max_orders_per_second;
    double max_latency_ms;
    
    RiskLimits() : max_position(0.5), max_daily_loss(1000.0),
                   max_drawdown(0.05), position_concentration(0.3),
                   var_limit(500.0), max_orders_per_second(100),
                   max_latency_ms(50.0) {}
};

// Performance metrics
struct LatencyMetrics {
    double mean_us;
    double median_us;
    double p95_us;
    double p99_us;
    double max_us;
    double min_us;
    uint64_t count;
    
    LatencyMetrics() : mean_us(0.0), median_us(0.0), p95_us(0.0),
                       p99_us(0.0), max_us(0.0), min_us(0.0), count(0) {}
};

struct PerformanceStats {
    double total_pnl;
    double sharpe_ratio;
    double win_rate;
    double max_drawdown;
    double order_to_trade_ratio;
    uint64_t total_trades;
    uint64_t winning_trades;
    
    PerformanceStats() : total_pnl(0.0), sharpe_ratio(0.0), win_rate(0.0),
                         max_drawdown(0.0), order_to_trade_ratio(0.0),
                         total_trades(0), winning_trades(0) {}
};

// Utility functions
inline timestamp_t now() {
    return hft_clock_t::now();
}

// High-resolution timestamp for HFT measurements
inline timestamp_t now_monotonic_raw() {
    return hft_clock_t::now();
}

// MarketDepth constructor implementation (after now() is defined)
inline MarketDepth::MarketDepth(uint32_t levels) : depth_levels(levels), timestamp(now()) {
    bids.reserve(levels);
    asks.reserve(levels);
}

// TradeExecution constructor implementation (after now() is defined)
inline TradeExecution::TradeExecution() : trade_id(0), aggressor_order_id(0), passive_order_id(0),
                                          price(0.0), quantity(0.0), aggressor_side(Side::BUY),
                                          timestamp(now()) {}

template<typename Duration>
inline double to_microseconds(const Duration& duration) {
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

inline duration_us_t time_diff_us(timestamp_t start, timestamp_t end) {
    return std::chrono::duration_cast<duration_us_t>(end - start);
}

inline std::string side_to_string(Side side) {
    return (side == Side::BUY) ? "BUY" : "SELL";
}

inline Side string_to_side(const std::string& side_str) {
    return (side_str == "BUY" || side_str == "buy") ? Side::BUY : Side::SELL;
}

inline std::string risk_check_result_to_string(RiskCheckResult result) {
    switch (result) {
        case RiskCheckResult::APPROVED: return "APPROVED";
        case RiskCheckResult::POSITION_LIMIT_EXCEEDED: return "POSITION_LIMIT_EXCEEDED";
        case RiskCheckResult::DAILY_LOSS_LIMIT_EXCEEDED: return "DAILY_LOSS_LIMIT_EXCEEDED";
        case RiskCheckResult::DRAWDOWN_LIMIT_EXCEEDED: return "DRAWDOWN_LIMIT_EXCEEDED";
        case RiskCheckResult::CONCENTRATION_RISK: return "CONCENTRATION_RISK";
        case RiskCheckResult::VAR_LIMIT_EXCEEDED: return "VAR_LIMIT_EXCEEDED";
        case RiskCheckResult::ORDER_RATE_LIMIT_EXCEEDED: return "ORDER_RATE_LIMIT_EXCEEDED";
        case RiskCheckResult::LATENCY_LIMIT_EXCEEDED: return "LATENCY_LIMIT_EXCEEDED";
        case RiskCheckResult::CRITICAL_BREACH: return "CRITICAL_BREACH";
        default: return "UNKNOWN";
    }
}

// Constants
constexpr double TICK_SIZE = 0.01;
constexpr double MAKER_FEE_RATE = 0.0000;
constexpr double TAKER_FEE_RATE = 0.0005;
constexpr uint32_t DEFAULT_ORDER_TTL_SEC = 120;
constexpr uint32_t LATENCY_WINDOW_SIZE = 1000;

} // namespace hft
