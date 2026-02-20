#pragma once

#include "types.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>
#include <queue>

namespace hft {

// Forward declarations
class OrderBookEngine;

/**
 * Order execution states for tracking order lifecycle
 */
enum class ExecutionState : uint8_t {
    PENDING_SUBMISSION = 0,    // Order created, not yet sent
    SUBMITTED = 1,             // Order sent to exchange
    ACKNOWLEDGED = 2,          // Exchange confirmed receipt
    PARTIALLY_FILLED = 3,      // Some quantity executed
    FILLED = 4,                // Fully executed
    CANCELLED = 5,             // Order cancelled
    REJECTED = 6,              // Exchange rejected order
    EXPIRED = 7                // Order TTL expired
};

/**
 * Order modification types for efficient updates
 */
enum class ModificationType : uint8_t {
    PRICE_ONLY = 0,           // Change price only
    QUANTITY_ONLY = 1,        // Change quantity only  
    PRICE_AND_QUANTITY = 2    // Change both
};

/**
 * Risk violation types for monitoring
 */
enum class RiskViolationType : uint8_t {
    NONE = 0,
    POSITION_LIMIT = 1,
    DAILY_LOSS_LIMIT = 2,
    ORDER_RATE_LIMIT = 3,
    CONCENTRATION_RISK = 4,
    VAR_LIMIT = 5,
    LATENCY_THRESHOLD = 6,
    CRITICAL_BREACH = 7
};

/**
 * Extended order information for HFT operations
 */
struct OrderInfo {
    // Core order data (inherits from Order in types.hpp)
    Order order;
    
    // Execution tracking
    ExecutionState execution_state;
    quantity_t filled_quantity;
    price_t average_fill_price;
    
    // Timing information for latency analysis
    timestamp_t creation_time;
    timestamp_t submission_time;
    timestamp_t acknowledgment_time;
    timestamp_t completion_time;
    
    // Risk and performance tracking
    price_t slippage;
    double time_in_queue_ms;
    bool is_aggressive;            // Taker vs maker
    uint32_t modification_count;
    
    // Market context
    price_t mid_price_at_creation;
    price_t mid_price_at_fill;
    double market_impact_bps;
    
    OrderInfo() : execution_state(ExecutionState::PENDING_SUBMISSION),
                  filled_quantity(0.0), average_fill_price(0.0),
                  slippage(0.0), time_in_queue_ms(0.0),
                  is_aggressive(false), modification_count(0),
                  mid_price_at_creation(0.0), mid_price_at_fill(0.0),
                  market_impact_bps(0.0) {}
};

/**
 * Real-time position and P&L tracking
 */
struct PositionInfo {
    position_t net_position;      // Current position size
    price_t avg_price;           // Volume-weighted average price
    double unrealized_pnl;       // Mark-to-market P&L
    double realized_pnl;         // Closed P&L
    double gross_exposure;       // Total exposure (long + short)
    quantity_t daily_volume;     // Total volume traded today
    uint32_t trade_count;        // Number of trades today
    
    // Risk metrics
    double var_contribution;     // Value at Risk contribution
    double concentration_ratio;  // Position as % of total portfolio
    timestamp_t last_update;
    
    PositionInfo() : net_position(0.0), avg_price(0.0), 
                     unrealized_pnl(0.0), realized_pnl(0.0),
                     gross_exposure(0.0), daily_volume(0.0),
                     trade_count(0), var_contribution(0.0),
                     concentration_ratio(0.0) {}
};

/**
 * Performance statistics for order execution analysis
 */
struct ExecutionStats {
    // Order statistics
    uint64_t total_orders;
    uint64_t filled_orders;
    uint64_t cancelled_orders;
    uint64_t rejected_orders;
    
    // Timing statistics (microseconds)
    double avg_submission_latency_us;
    double avg_fill_time_ms;
    double avg_cancel_time_ms;
    
    // Execution quality
    double fill_rate;             // % orders filled
    double avg_slippage_bps;      // Average slippage in basis points
    double avg_market_impact_bps; // Average market impact
    
    // Risk metrics
    uint32_t risk_violations;
    double max_daily_loss;
    double current_drawdown;
    
    ExecutionStats() : total_orders(0), filled_orders(0), cancelled_orders(0),
                       rejected_orders(0), avg_submission_latency_us(0.0),
                       avg_fill_time_ms(0.0), avg_cancel_time_ms(0.0),
                       fill_rate(0.0), avg_slippage_bps(0.0),
                       avg_market_impact_bps(0.0), risk_violations(0),
                       max_daily_loss(0.0), current_drawdown(0.0) {}
};

/**
 * Callback function types for order events
 */
using OrderCallback = std::function<void(const OrderInfo&)>;
using FillCallback = std::function<void(const OrderInfo&, quantity_t fill_qty, price_t fill_price, bool is_final_fill)>;
using RiskCallback = std::function<void(RiskViolationType, const std::string& message)>;

/**
 * High-performance Order Manager for HFT systems
 * 
 * Design Goals:
 * - Sub-microsecond order operations in critical path
 * - Zero-allocation fast path using memory pools
 * - Real-time risk management and position tracking
 * - Comprehensive performance analytics
 * - Lock-free operations where possible
 */
class OrderManager {
public:
    explicit OrderManager(MemoryManager& memory_manager,
                         LatencyTracker& latency_tracker,
                         const RiskLimits& risk_limits);
    
    ~OrderManager();
    
    // Non-copyable, non-movable for safety
    OrderManager(const OrderManager&) = delete;
    OrderManager& operator=(const OrderManager&) = delete;
    OrderManager(OrderManager&&) = delete;
    OrderManager& operator=(OrderManager&&) = delete;
    
    // =========================================================================
    // CORE ORDER OPERATIONS (CRITICAL PATH - OPTIMIZED FOR SPEED)
    // =========================================================================
    
    /**
     * Create new order - Returns order ID or 0 if rejected
     * PERFORMANCE: Target < 1 microsecond
     */
    uint64_t create_order(Side side, price_t price, quantity_t quantity,
                         price_t current_mid_price = 0.0);
    
    /**
     * Modify existing order - Returns true if successful
     * PERFORMANCE: Target < 500 nanoseconds  
     */
    bool modify_order(uint64_t order_id, price_t new_price, quantity_t new_quantity,
                     ModificationType mod_type = ModificationType::PRICE_AND_QUANTITY);
    
    /**
     * Cancel order - Returns true if successful
     * PERFORMANCE: Target < 300 nanoseconds
     */
    bool cancel_order(uint64_t order_id);
    
    /**
     * Submit order to exchange - Returns true if submitted
     * PERFORMANCE: Target < 2 microseconds including risk checks
     */
    bool submit_order(uint64_t order_id);
    
    // =========================================================================
    // ORDER LIFECYCLE MANAGEMENT
    // =========================================================================
    
    /**
     * Handle order acknowledgment from exchange
     */
    bool handle_order_ack(uint64_t order_id, timestamp_t ack_time);
    
    /**
     * Handle partial or full fill
     */
    bool handle_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price,
                    timestamp_t fill_time, bool is_final_fill = false);
    
    /**
     * Handle order rejection from exchange
     */
    bool handle_rejection(uint64_t order_id, const std::string& reason);
    
    /**
     * Handle order cancellation confirmation
     */
    bool handle_cancel_confirmation(uint64_t order_id);
    
    // =========================================================================
    // REAL-TIME RISK MANAGEMENT
    // =========================================================================
    
    /**
     * Pre-trade risk check - MUST be fast (< 100ns)
     */
    RiskCheckResult check_pre_trade_risk(Side side, quantity_t quantity, 
                                        price_t price) const;
    
    /**
     * Update risk limits in real-time
     */
    void update_risk_limits(const RiskLimits& new_limits);
    
    /**
     * Emergency risk shutdown - Cancel all orders
     */
    void emergency_shutdown(const std::string& reason);
    
    // =========================================================================
    // POSITION AND P&L TRACKING
    // =========================================================================
    
    /**
     * Get current position - Lock-free read
     */
    PositionInfo get_position() const;
    
    /**
     * Update position with external trade
     */
    void update_position(quantity_t quantity, price_t price, Side side);
    
    /**
     * Calculate unrealized P&L with current market price
     */
    double calculate_unrealized_pnl(price_t current_mid_price) const;
    
    // =========================================================================
    // PERFORMANCE MONITORING AND ANALYTICS
    // =========================================================================
    
    /**
     * Get comprehensive execution statistics
     */
    ExecutionStats get_execution_stats() const;
    
    /**
     * Get order information by ID
     */
    const OrderInfo* get_order_info(uint64_t order_id) const;
    
    /**
     * Get all active orders
     */
    std::vector<uint64_t> get_active_orders() const;
    
    /**
     * Print detailed performance report
     */
    void print_performance_report() const;
    
    /**
     * Reset daily statistics (called at start of trading day)
     */
    void reset_daily_stats();
    
    // =========================================================================
    // EVENT CALLBACKS AND NOTIFICATIONS
    // =========================================================================
    
    /**
     * Register callbacks for order events
     */
    void set_order_callback(OrderCallback callback);
    void set_fill_callback(FillCallback callback);
    void set_risk_callback(RiskCallback callback);
    
    // =========================================================================
    // INTEGRATION WITH ORDER BOOK ENGINE
    // =========================================================================
    
    /**
     * Set OrderBookEngine reference for order execution
     */
    void set_orderbook_engine(OrderBookEngine* orderbook_engine);
    
    // =========================================================================
    // SYSTEM MONITORING AND DEBUG
    // =========================================================================
    
    /**
     * Get system health metrics
     */
    bool is_healthy() const;
    size_t get_active_order_count() const;
    size_t get_pending_order_count() const;
    
private:
    // =========================================================================
    // FAST PATH HELPERS (INLINE FOR PERFORMANCE)
    // =========================================================================
    
    // Order ID generation (lock-free)
    uint64_t generate_order_id() noexcept;
    
    // Fast order lookup (hash table)
    OrderInfo* find_order(uint64_t order_id) noexcept;
    const OrderInfo* find_order(uint64_t order_id) const noexcept;
    
    // Risk calculation helpers
    bool check_position_limit(Side side, quantity_t quantity) const noexcept;
    bool check_daily_loss_limit() const noexcept;
    bool check_order_rate_limit() const noexcept;
    
    // Market impact calculation
    double calculate_market_impact(quantity_t quantity, price_t price) const;
    
    // Force cancel order during shutdown (bypasses engine to avoid deadlocks)
    bool force_cancel_order_during_shutdown(uint64_t order_id);
    
    // Performance tracking
    void update_execution_stats(const OrderInfo& order_info);
    
    // =========================================================================
    // MEMBER VARIABLES (ORGANIZED FOR CACHE EFFICIENCY)
    // =========================================================================
    
    // =========================================================================
    // CORE COMPONENTS
    // =========================================================================
    
    MemoryManager& memory_manager_;
    LatencyTracker& latency_tracker_;
    OrderBookEngine* orderbook_engine_;  // Integration point for order execution
    bool engine_was_connected_;          // Track if engine was ever connected

    // Order storage and lookup (hot path data)
    std::unordered_map<uint64_t, OrderInfo> orders_;           // All orders
    std::unordered_set<uint64_t> pending_orders_;              // Orders awaiting submission
    std::unordered_set<uint64_t> active_orders_;               // Orders in market
    
    // **CRITICAL: Track pooled orders for proper memory management**
    std::unordered_map<uint64_t, Order*> pooled_orders_;       // Pointers to pooled orders

    // Position and risk management (frequently accessed) - MOVED UP for init order
    RiskLimits risk_limits_;
    
    // Order ID generation (lock-free)
    std::atomic<uint64_t> next_order_id_;
    
    // Position tracking (frequently accessed)
    mutable std::mutex position_mutex_;
    PositionInfo current_position_;
    
    // Performance statistics (atomic for lock-free reads)
    mutable std::mutex stats_mutex_;
    ExecutionStats execution_stats_;
    
    // Rate limiting for risk management
    mutable std::queue<timestamp_t> recent_orders_;
    mutable std::mutex rate_limit_mutex_;
    
    // Event callbacks
    OrderCallback order_callback_;
    FillCallback fill_callback_;
    RiskCallback risk_callback_;
    
    // System state
    std::atomic<bool> is_emergency_shutdown_;
    timestamp_t session_start_time_;
    
};

} // namespace hft
