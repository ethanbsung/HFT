#pragma once

#include "types.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include "orderbook_engine.hpp"
#include "order_manager.hpp"
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace hft {

// Forward declarations
class OrderBookEngine;
class OrderManager;

/**
 * Market making quote side
 */
enum class QuoteSide : uint8_t {
    BID = 0,
    ASK = 1,
    BOTH = 2
};

/**
 * Quote state for tracking active quotes
 */
enum class QuoteState : uint8_t {
    INACTIVE = 0,
    PENDING = 1,
    ACTIVE = 2,
    CANCELLING = 3,
    REPLACING = 4
};

/**
 * Market making signal types
 */
enum class SignalType : uint8_t {
    PLACE_BID = 0,
    PLACE_ASK = 1,
    CANCEL_BID = 2,
    CANCEL_ASK = 3,
    MODIFY_BID = 4,
    MODIFY_ASK = 5,
    HOLD = 6,
    EMERGENCY_CANCEL = 7
};

/**
 * Market making quote information
 */
struct MarketMakingQuote {
    QuoteSide side;
    price_t price;
    quantity_t quantity;
    QuoteState state;
    timestamp_t creation_time;
    timestamp_t last_update_time;
    uint64_t order_id;
    double spread_bps;
    bool is_aggressive;
    quantity_t filled_quantity;  // Track filled quantity
    
    MarketMakingQuote() : side(QuoteSide::BID), price(0.0), quantity(0.0),
                          state(QuoteState::INACTIVE), order_id(0),
                          spread_bps(0.0), is_aggressive(false), filled_quantity(0.0) {}
};

/**
 * Market making signal for order generation
 */
struct TradingSignal {
    SignalType type;
    Side side;
    price_t price;
    quantity_t quantity;
    uint64_t order_id;  // For modifications/cancellations
    timestamp_t timestamp;
    std::string reason;
    
    TradingSignal() : type(SignalType::HOLD), side(Side::BUY), price(0.0),
                      quantity(0.0), order_id(0), timestamp(now()) {}
};

/**
 * Market making configuration
 */
struct MarketMakingConfig {
    // Quote parameters
    quantity_t default_quote_size;
    double min_spread_bps;
    double max_spread_bps;
    double target_spread_bps;
    
    // Inventory management
    position_t max_position;
    double inventory_skew_factor;
    double max_inventory_skew_bps;
    
    // Risk management
    double max_daily_loss;
    double max_drawdown;
    uint32_t max_orders_per_second;
    
    // Performance settings
    uint32_t quote_refresh_ms;
    uint32_t cooldown_ms;
    bool enable_aggressive_quotes;
    
    // Capital and P&L tracking
    price_t initial_capital;
    
    MarketMakingConfig() : default_quote_size(10.0), min_spread_bps(5.0),
                          max_spread_bps(50.0), target_spread_bps(15.0),
                          max_position(100.0), inventory_skew_factor(0.1),
                          max_inventory_skew_bps(20.0), max_daily_loss(1000.0),
                          max_drawdown(0.05), max_orders_per_second(100),
                          quote_refresh_ms(1000), cooldown_ms(500),
                          enable_aggressive_quotes(false), initial_capital(10000.0) {}
};

/**
 * Market making statistics
 */
struct MarketMakingStats {
    // Quote statistics
    uint64_t total_quotes_placed;
    uint64_t total_quotes_filled;
    uint64_t total_quotes_cancelled;
    double fill_rate;
    double avg_spread_captured_bps;
    
    // Performance metrics
    double total_pnl;
    double realized_pnl;
    double unrealized_pnl;
    double sharpe_ratio;
    double max_drawdown;
    
    // Risk metrics
    uint32_t risk_violations;
    double current_position;
    double position_limit_utilization;
    
    // Note: Latency metrics are tracked by LatencyTracker component
    
    MarketMakingStats() : total_quotes_placed(0), total_quotes_filled(0),
                          total_quotes_cancelled(0), fill_rate(0.0),
                          avg_spread_captured_bps(0.0), total_pnl(0.0),
                          realized_pnl(0.0), unrealized_pnl(0.0),
                          sharpe_ratio(0.0), max_drawdown(0.0),
                          risk_violations(0), current_position(0.0),
                          position_limit_utilization(0.0) {}
};

/**
 * Callback function types
 */
using SignalCallback = std::function<void(const TradingSignal&)>;
using QuoteUpdateCallback = std::function<void(const MarketMakingQuote&)>;
using RiskAlertCallback = std::function<void(const std::string&, double)>;

/**
 * Convert quote side to string
 */
std::string quote_side_to_string(QuoteSide side);

/**
 * Market depth analysis metrics
 */
struct DepthMetrics {
    double bid_liquidity_bps;      // Liquidity on bid side (basis points)
    double ask_liquidity_bps;      // Liquidity on ask side (basis points)
    double bid_ask_imbalance;      // Ratio of bid to ask liquidity
    double market_pressure;        // -1.0 to 1.0 (negative = bearish, positive = bullish)
    double spread_impact;          // Expected impact on spread
    bool significant_change;       // Whether this represents a significant change
    price_t optimal_bid_price;     // Calculated optimal bid price
    price_t optimal_ask_price;     // Calculated optimal ask price
    quantity_t optimal_bid_size;   // Calculated optimal bid size
    quantity_t optimal_ask_size;   // Calculated optimal ask size
    
    DepthMetrics() : bid_liquidity_bps(0.0), ask_liquidity_bps(0.0),
                     bid_ask_imbalance(1.0), market_pressure(0.0),
                     spread_impact(0.0), significant_change(false),
                     optimal_bid_price(0.0), optimal_ask_price(0.0),
                     optimal_bid_size(0.0), optimal_ask_size(0.0) {}
};

/**
 * High-Performance Market Making Signal Engine
 * 
 * Design Goals:
 * - Sub-microsecond signal generation
 * - Two-sided market making with inventory management
 * - Real-time risk monitoring and position control
 * - Integration with OrderManager for execution
 * - Memory pool integration for zero-allocation fast path
 * - Comprehensive latency tracking and performance monitoring
 */
class SignalEngine {
public:
    explicit SignalEngine(LatencyTracker& latency_tracker,
                         const MarketMakingConfig& config = MarketMakingConfig());
    
    ~SignalEngine();
    
    // Non-copyable, non-movable for safety
    SignalEngine(const SignalEngine&) = delete;
    SignalEngine& operator=(const SignalEngine&) = delete;
    SignalEngine(SignalEngine&&) = delete;
    SignalEngine& operator=(SignalEngine&&) = delete;
    
    // =========================================================================
    // CORE SIGNAL ENGINE OPERATIONS
    // =========================================================================
    
    /**
     * Process market data update and generate trading signals
     * PERFORMANCE: Target < 1 microsecond
     */
    void process_market_data_update(const TopOfBook& top_of_book);
    
    /**
     * Generate trading signals based on current market conditions
     * PERFORMANCE: Target < 2 microseconds
     */
    std::vector<TradingSignal> generate_trading_signals();
    
    // =========================================================================
    // MARKET MAKING STRATEGY OPERATIONS
    // =========================================================================
    
    /**
     * Calculate optimal bid and ask quotes
     * PERFORMANCE: Target < 1 microsecond
     */
    void calculate_optimal_quotes(price_t& bid_price, price_t& ask_price,
                                 quantity_t& bid_size, quantity_t& ask_size);
    
    /**
     * Apply inventory-based skewing to quotes
     * PERFORMANCE: Target < 200 nanoseconds
     */
    void apply_inventory_skew(price_t& bid_price, price_t& ask_price);
    
    /**
     * Determine if quotes should be placed/modified/cancelled
     * PERFORMANCE: Target < 500 nanoseconds
     */
    bool should_place_quote(QuoteSide side, price_t price, quantity_t size);
    
    /**
     * Check if existing quote should be replaced
     * PERFORMANCE: Target < 300 nanoseconds
     */
    bool should_replace_quote(QuoteSide side, price_t current_price, price_t new_price);
    
    // =========================================================================
    // CONFIGURATION AND CONTROL
    // =========================================================================

    /**
     * Start the signal engine
     */
    bool start();
    
    /**
     * Stop the signal engine
     */
    void stop();
    
    /**
     * Update market making configuration
     */
    void update_config(const MarketMakingConfig& config);
    
    /**
     * Set order book engine reference
     */
    void set_orderbook_engine(OrderBookEngine* orderbook_engine);
    
    /**
     * Set order manager reference
     */
    void set_order_manager(OrderManager* order_manager);
    
    // =========================================================================
    // CALLBACK MANAGEMENT
    // =========================================================================
    
    /**
     * Set signal generation callback
     */
    void set_signal_callback(SignalCallback callback);
    
    /**
     * Set quote update callback
     */
    void set_quote_update_callback(QuoteUpdateCallback callback);
    
    /**
     * Set risk alert callback
     */
    void set_risk_alert_callback(RiskAlertCallback callback);
    
    /**
     * Clear all callbacks (for safe cleanup)
     */
    void clear_all_callbacks();
    
    // =========================================================================
    // MONITORING AND STATISTICS
    // =========================================================================
    
    /**
     * Get current market making statistics
     */
    MarketMakingStats get_statistics() const;
    
    /**
     * Get current quote state
     */
    std::vector<MarketMakingQuote> get_active_quotes() const;
    
    /**
     * Print performance report
     */
    void print_performance_report() const;
    
    /**
     * Get signal generation latency statistics
     */
    LatencyStatistics get_signal_generation_latency() const;

    // =========================================================================
    // TESTING INTERFACE METHODS (made public for testing)
    // =========================================================================
    
    /**
     * Analyze market depth and calculate metrics
     */
    DepthMetrics analyze_market_depth(const MarketDepth& depth);
    
    /**
     * Calculate liquidity in basis points for a side
     */
    double calculate_liquidity_bps(const std::vector<PriceLevel>& levels, 
                                  price_t mid_price, Side side);
    
    /**
     * Calculate market pressure based on depth
     */
    double calculate_market_pressure(const MarketDepth& depth);
    
    /**
     * Calculate expected spread impact from depth
     */
    double calculate_spread_impact(const MarketDepth& depth, price_t mid_price);
    
    /**
     * Check if quote should be cancelled (consolidates cancellation logic)
     */
    bool should_cancel_quote(const MarketMakingQuote& quote, price_t mid_price) const;
    
    /**
     * Calculate position-adjusted quote size (consolidates size logic)
     */
    quantity_t calculate_position_adjusted_size(quantity_t base_size, QuoteSide side) const;
    
    /**
     * Update statistics with signal information
     */
    void update_statistics(const TradingSignal& signal);
    
    /**
     * Notify risk alert callback
     */
    void notify_risk_alert(const std::string& alert, double value);

    // =========================================================================
    // QUOTE MANAGEMENT METHODS
    // =========================================================================
    
    /**
     * Track order placement for quote management
     */
    void track_order_placement(uint64_t order_id, QuoteSide side, price_t price, quantity_t quantity);
    
    /**
     * Track order cancellation for quote management
     */
    void track_order_cancellation(uint64_t order_id);
    
    /**
     * Track order fill for quote management
     */
    void track_order_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price);
    
    /**
     * Clear stale quotes from tracking
     */
    void clear_stale_quotes();
    
    /**
     * Generate targeted quote cancellation signals for specific side
     */
    void generate_targeted_cancellation_signals(std::vector<TradingSignal>& signals, QuoteSide side);
    
    /**
     * Generate quote cancellation signals (for testing)
     */
    void generate_cancellation_signals(std::vector<TradingSignal>& signals);

private:
    // =========================================================================
    // PRIVATE MEMBER VARIABLES
    // =========================================================================
    
    // Core components
    LatencyTracker* latency_tracker_;  // Stored as pointer for safe teardown checks
    MarketMakingConfig config_;
    
    // External component references
    OrderBookEngine* orderbook_engine_;
    OrderManager* order_manager_;
    
    // Market making state
    std::atomic<bool> is_running_;
    std::atomic<bool> should_stop_;
    std::atomic<bool> is_destroying_;  // Flag to prevent operations during destruction
    timestamp_t session_start_time_;
    
    // Quote management
    std::unordered_map<uint64_t, MarketMakingQuote> active_quotes_;  // order_id -> quote
    mutable std::mutex quotes_mutex_;
    
    // Market state
    TopOfBook current_top_of_book_;
    
    // Statistics and monitoring
    mutable std::mutex stats_mutex_;
    MarketMakingStats statistics_;
    
    // Callbacks
    SignalCallback signal_callback_;
    QuoteUpdateCallback quote_update_callback_;
    RiskAlertCallback risk_alert_callback_;
    
    // Performance tracking
    mutable std::queue<timestamp_t> recent_signals_;
    mutable std::mutex signal_rate_mutex_;
    
    // Signal ID tracking
    std::atomic<uint64_t> next_signal_id_;
    
    // =========================================================================
    // PRIVATE HELPER METHODS
    // =========================================================================

    /**
     * Notify callbacks of signal generation
     */
    void notify_signal_generated(const TradingSignal& signal);
    
    // =========================================================================
    // DEPTH ANALYSIS METHODS
    // =========================================================================
    
    /**
     * Detect significant changes in market depth
     */
    bool detect_significant_depth_change(const MarketDepth& depth);
    
    // =========================================================================
    // SIGNAL GENERATION METHODS
    // =========================================================================
    
    /**
     * Generate quote placement signals
     */
    void generate_quote_signals(std::vector<TradingSignal>& signals,
                               price_t bid_price, price_t ask_price,
                               quantity_t bid_size, quantity_t ask_size);
    
    /**
     * Apply rate limiting to signals
     */
    void apply_rate_limiting(std::vector<TradingSignal>& signals);
};

// =========================================================================
// UTILITY FUNCTIONS
// =========================================================================

/**
 * Create default market making configuration
 */
MarketMakingConfig create_default_market_making_config();

/**
 * Create aggressive market making configuration
 */
MarketMakingConfig create_aggressive_market_making_config();

/**
 * Create conservative market making configuration
 */
MarketMakingConfig create_conservative_market_making_config();

/**
 * Validate market making signal
 */
bool validate_trading_signal(const TradingSignal& signal);

/**
 * Convert signal type to string
 */
std::string signal_type_to_string(SignalType type);

} // namespace hft
