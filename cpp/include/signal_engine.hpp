#pragma once

#include "types.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include "orderbook_engine.hpp"
#include "order_manager.hpp"
#include "market_data_feed.hpp"
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
class MarketDataFeed;

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
    
    MarketMakingQuote() : side(QuoteSide::BID), price(0.0), quantity(0.0),
                          state(QuoteState::INACTIVE), order_id(0),
                          spread_bps(0.0), is_aggressive(false) {}
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
    
    MarketMakingConfig() : default_quote_size(10.0), min_spread_bps(5.0),
                          max_spread_bps(50.0), target_spread_bps(15.0),
                          max_position(100.0), inventory_skew_factor(0.1),
                          max_inventory_skew_bps(20.0), max_daily_loss(1000.0),
                          max_drawdown(0.05), max_orders_per_second(100),
                          quote_refresh_ms(1000), cooldown_ms(500),
                          enable_aggressive_quotes(false) {}
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
    
    // Timing metrics
    double avg_signal_generation_latency_us;
    double avg_quote_placement_latency_us;
    
    MarketMakingStats() : total_quotes_placed(0), total_quotes_filled(0),
                          total_quotes_cancelled(0), fill_rate(0.0),
                          avg_spread_captured_bps(0.0), total_pnl(0.0),
                          realized_pnl(0.0), unrealized_pnl(0.0),
                          sharpe_ratio(0.0), max_drawdown(0.0),
                          risk_violations(0), current_position(0.0),
                          position_limit_utilization(0.0),
                          avg_signal_generation_latency_us(0.0),
                          avg_quote_placement_latency_us(0.0) {}
};

/**
 * Callback function types
 */
using SignalCallback = std::function<void(const TradingSignal&)>;
using QuoteUpdateCallback = std::function<void(const MarketMakingQuote&)>;
using RiskAlertCallback = std::function<void(const std::string&, double)>;

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
    explicit SignalEngine(MemoryManager& memory_manager,
                         LatencyTracker& latency_tracker,
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
     * Process trade execution and update internal state
     * PERFORMANCE: Target < 500 nanoseconds
     */
    void process_trade_execution(const TradeExecution& trade);
    
    /**
     * Process market depth update for quote optimization
     * PERFORMANCE: Target < 1 microsecond
     */
    void process_market_depth_update(const MarketDepth& depth);
    
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
    // RISK MANAGEMENT OPERATIONS
    // =========================================================================
    
    /**
     * Check pre-trade risk before generating signals
     * PERFORMANCE: Target < 500 nanoseconds
     */
    bool check_pre_trade_risk(const TradingSignal& signal);
    
    /**
     * Update risk metrics based on market data
     * PERFORMANCE: Target < 300 nanoseconds
     */
    void update_risk_metrics(const TopOfBook& market_data);
    
    /**
     * Check position limits and risk constraints
     * PERFORMANCE: Target < 200 nanoseconds
     */
    bool check_position_limits(Side side, quantity_t quantity);
    
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
    
    /**
     * Set market data feed reference
     */
    void set_market_data_feed(MarketDataFeed* market_data_feed);
    
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
     * Get current position and P&L
     */
    PositionInfo get_position_info() const;
    
    /**
     * Print performance report
     */
    void print_performance_report() const;
    
    /**
     * Reset daily statistics
     */
    void reset_daily_stats();
    
    /**
     * Check if signal engine is healthy
     */
    bool is_healthy() const;
    
    /**
     * Get signal generation latency statistics
     */
    LatencyStatistics get_signal_generation_latency() const;
    
private:
    // =========================================================================
    // PRIVATE MEMBER VARIABLES
    // =========================================================================
    
    // Core components
    MemoryManager& memory_manager_;
    LatencyTracker& latency_tracker_;
    MarketMakingConfig config_;
    
    // External component references
    OrderBookEngine* orderbook_engine_;
    OrderManager* order_manager_;
    MarketDataFeed* market_data_feed_;
    
    // Market making state
    std::atomic<bool> is_running_;
    std::atomic<bool> should_stop_;
    timestamp_t session_start_time_;
    
    // Quote management
    std::unordered_map<QuoteSide, MarketMakingQuote> active_quotes_;
    mutable std::mutex quotes_mutex_;
    
    // Market data state
    TopOfBook current_top_of_book_;
    MarketDepth current_market_depth_;
    std::atomic<price_t> last_mid_price_;
    std::atomic<price_t> last_spread_bps_;
    
    // Position and risk tracking
    std::atomic<position_t> current_position_;
    std::atomic<double> current_pnl_;
    std::atomic<double> peak_equity_;
    std::atomic<double> max_drawdown_;
    
    // Statistics and monitoring
    mutable std::mutex stats_mutex_;
    MarketMakingStats statistics_;
    
    // Callbacks
    SignalCallback signal_callback_;
    QuoteUpdateCallback quote_update_callback_;
    RiskAlertCallback risk_alert_callback_;
    
    // Performance tracking
    std::queue<timestamp_t> recent_signals_;
    mutable std::mutex signal_rate_mutex_;
    
    // =========================================================================
    // PRIVATE HELPER METHODS
    // =========================================================================
    
    /**
     * Initialize market making state
     */
    void initialize_market_making_state();
    
    /**
     * Update quote state based on market conditions
     */
    void update_quote_state(const TopOfBook& top_of_book);
    
    /**
     * Calculate spread-based quote adjustments
     */
    void calculate_spread_adjustments(price_t& bid_price, price_t& ask_price);
    
    /**
     * Track signal generation performance
     */
    void track_signal_generation_latency(timestamp_t start_time);
    
    /**
     * Update statistics with new signal
     */
    void update_statistics(const TradingSignal& signal);
    
    /**
     * Notify callbacks of signal generation
     */
    void notify_signal_generated(const TradingSignal& signal);
    
    /**
     * Notify callbacks of quote updates
     */
    void notify_quote_updated(const MarketMakingQuote& quote);
    
    /**
     * Notify callbacks of risk alerts
     */
    void notify_risk_alert(const std::string& alert, double value);
    
    /**
     * Validate market making configuration
     */
    bool validate_config(const MarketMakingConfig& config) const;
    
    /**
     * Check if market conditions are suitable for quoting
     */
    bool is_market_suitable_for_quoting() const;
    
    /**
     * Calculate market volatility for spread adjustment
     */
    double calculate_market_volatility() const;
    
    /**
     * Update position and P&L tracking
     */
    void update_position_tracking(const TradeExecution& trade);
    
    /**
     * Calculate realized and unrealized P&L
     */
    void calculate_pnl_metrics();
    
    /**
     * Check for risk violations
     */
    bool check_risk_violations();
    
    /**
     * Emergency shutdown procedures
     */
    void emergency_shutdown(const std::string& reason);
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

/**
 * Convert quote side to string
 */
std::string quote_side_to_string(QuoteSide side);

} // namespace hft
