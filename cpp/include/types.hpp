#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace hft {

// Timing and latency types
using timestamp_t = std::chrono::time_point<std::chrono::high_resolution_clock>;
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

// Market data structures
struct PriceLevel {
    price_t price;
    quantity_t quantity;
    
    PriceLevel() : price(0.0), quantity(0.0) {}
    PriceLevel(price_t p, quantity_t q) : price(p), quantity(q) {}
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

// Order management structures
struct Order {
    uint64_t order_id;
    Side side;
    price_t price;
    quantity_t original_quantity;
    quantity_t remaining_quantity;
    quantity_t queue_ahead;
    OrderStatus status;
    timestamp_t entry_time;
    timestamp_t last_update_time;
    price_t mid_price_at_entry;
    
    Order() : order_id(0), side(Side::BUY), price(0.0), 
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
    return std::chrono::high_resolution_clock::now();
}

inline double to_microseconds(const duration_us_t& duration) {
    return static_cast<double>(duration.count());
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

// Constants
constexpr double TICK_SIZE = 0.01;
constexpr double MAKER_FEE_RATE = 0.0000;
constexpr double TAKER_FEE_RATE = 0.0005;
constexpr uint32_t DEFAULT_ORDER_TTL_SEC = 120;
constexpr uint32_t LATENCY_WINDOW_SIZE = 1000;

} // namespace hft
