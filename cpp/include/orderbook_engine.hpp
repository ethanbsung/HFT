#pragma once

#include "types.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <queue>
#include <chrono>
#include <shared_mutex>
#include <cmath>  // For std::isnan, std::isinf

namespace hft {

// Forward declarations
class OrderManager;

/**
 * Order book side for bid/ask distinction
 */
enum class BookSide : uint8_t {
    BID = 0,    // Buy orders
    ASK = 1     // Sell orders
};

/**
 * Market data update types
 */
enum class UpdateType : uint8_t {
    NEW_ORDER = 0,
    MODIFY_ORDER = 1,
    CANCEL_ORDER = 2,
    TRADE = 3,
    BOOK_SNAPSHOT = 4,
    TOP_OF_BOOK = 5
};

/**
 * Market data snapshot for top-of-book
 */
struct TopOfBook {
    price_t bid_price;
    quantity_t bid_quantity;
    price_t ask_price;
    quantity_t ask_quantity;
    price_t mid_price;
    price_t spread;
    timestamp_t timestamp;
    
    TopOfBook() : bid_price(0.0), bid_quantity(0.0), ask_price(0.0), 
                  ask_quantity(0.0), mid_price(0.0), spread(0.0), 
                  timestamp(now()) {}
};

/**
 * Callback function types for order book events
 */
using BookUpdateCallback = std::function<void(const TopOfBook&)>;
using TradeCallback = std::function<void(const TradeExecution&)>;
using DepthUpdateCallback = std::function<void(const MarketDepth&)>;

/**
 * High-performance Order Book Engine for HFT systems
 * 
 * Design Goals:
 * - Sub-microsecond order matching
 * - Lock-free operations where possible
 * - Integration with OrderManager for order lifecycle
 * - Real-time market data feeds for SignalEngine
 * - Memory pool integration for zero-allocation fast path
 * - Comprehensive latency tracking
 */
class OrderBookEngine {
public:
    explicit OrderBookEngine(MemoryManager& memory_manager,
                            LatencyTracker& latency_tracker,
                            const std::string& symbol = "DEFAULT");
    
    ~OrderBookEngine();
    
    // Non-copyable, non-movable for safety
    OrderBookEngine(const OrderBookEngine&) = delete;
    OrderBookEngine& operator=(const OrderBookEngine&) = delete;
    OrderBookEngine(OrderBookEngine&&) = delete;
    OrderBookEngine& operator=(OrderBookEngine&&) = delete;
    
    // =========================================================================
    // CORE ORDER BOOK OPERATIONS (CRITICAL PATH)
    // =========================================================================
    
    /**
     * Add order to book - Returns match result
     * PERFORMANCE: Target < 500 nanoseconds
     */
    MatchResult add_order(const Order& order, std::vector<TradeExecution>& executions);
    
    /**
     * Modify existing order in book
     * PERFORMANCE: Target < 300 nanoseconds
     */
    bool modify_order(uint64_t order_id, price_t new_price, quantity_t new_quantity);
    
    /**
     * Cancel order from book
     * PERFORMANCE: Target < 200 nanoseconds
     */
    bool cancel_order(uint64_t order_id);
    
    /**
     * Process market order (immediate execution)
     * PERFORMANCE: Target < 1 microsecond
     */
    MatchResult process_market_order(Side side, quantity_t quantity, 
                                   std::vector<TradeExecution>& executions);
    
    // =========================================================================
    // MARKET DATA ACCESS (FOR SIGNAL ENGINE)
    // =========================================================================
    
    /**
     * Get current top of book (best bid/offer)
     * PERFORMANCE: Target < 50 nanoseconds (lock-free read)
     */
    TopOfBook get_top_of_book() const;
    
    /**
     * Get market depth (Level 2 data)
     * PERFORMANCE: Target < 200 nanoseconds
     */
    MarketDepth get_market_depth(uint32_t levels = 10) const;
    
    /**
     * Get mid price (for P&L calculations)
     */
    price_t get_mid_price() const;
    
    /**
     * Get spread in basis points
     */
    double get_spread_bps() const;
    
    /**
     * Check if market is crossed (bid >= ask)
     */
    bool is_market_crossed() const;
    
    // =========================================================================
    // ORDER BOOK STATE MANAGEMENT
    // =========================================================================
    
    /**
     * Apply external market data update
     */
    void apply_market_data_update(const MarketDepth& update);
    
    
    /**
     * Process actual market trades to determine fills for our orders
     * This replaces the old probabilistic fill simulation
     */
    void process_market_data_trade(const TradeExecution& trade);
    
    /**
     * Queue position tracking for realistic fill simulation
     */
    struct QueuePosition {
        uint64_t order_id;
        price_t price;
        Side side;
        quantity_t original_quantity;
        quantity_t remaining_quantity;
        quantity_t queue_ahead;  // Amount in queue ahead of this order
        timestamp_t entry_time;
        
        QueuePosition() : order_id(0), price(0.0), side(Side::BUY), 
                         original_quantity(0.0), remaining_quantity(0.0), 
                         queue_ahead(0.0), entry_time() {}
    };
    
    // Queue position tracking
    std::unordered_map<uint64_t, QueuePosition> queue_positions_;
    std::mutex queue_mutex_;
    
    void track_queue_position(uint64_t order_id, price_t price, Side side, quantity_t quantity);
    void track_queue_position_with_exact_position(uint64_t order_id, price_t price, Side side, quantity_t quantity, quantity_t exact_queue_ahead);
    void update_queue_positions_from_trade(const TradeExecution& trade);
    quantity_t calculate_fill_from_queue_position(uint64_t order_id, const TradeExecution& trade);
    void process_fills_from_queue_positions(const TradeExecution& trade);
    
    /**
     * Reset book to empty state
     */
    void clear_book();
    
    /**
     * Get order book statistics
     */
    OrderBookStats get_statistics() const;
    
    // =========================================================================
    // INTEGRATION WITH ORDER MANAGER
    // =========================================================================
    
    /**
     * Set OrderManager reference for callbacks
     */
    void set_order_manager(OrderManager* order_manager);
    
    /**
     * Submit order from OrderManager - This is the missing integration point!
     * Called by OrderManager::submit_order() to actually execute the order
     */
    MatchResult submit_order_from_manager(const Order& order, std::vector<TradeExecution>& executions);
    
    
    // =========================================================================
    // EVENT CALLBACKS (FOR SIGNAL ENGINE)
    // =========================================================================
    
    /**
     * Register callbacks for market events
     */
    void set_book_update_callback(BookUpdateCallback callback);
    void set_trade_callback(TradeCallback callback);
    void set_depth_update_callback(DepthUpdateCallback callback);
    
    // =========================================================================
    // PERFORMANCE MONITORING
    // =========================================================================
    
    /**
     * Get order processing latency statistics
     */
    LatencyStatistics get_matching_latency() const;
    
    /**
     * Reset performance counters
     */
    void reset_performance_counters();
    
    
    // Simulation methods
    void simulate_market_order_from_trade(const TradeExecution& trade);
    
    
private:
    // =========================================================================
    // INTERNAL ORDER BOOK DATA STRUCTURES
    // =========================================================================
    
    // Price-level maps (sorted for efficient matching)
    std::map<price_t, PriceLevel, std::greater<price_t>> bids_;  // Highest first
    std::map<price_t, PriceLevel, std::less<price_t>> asks_;     // Lowest first
    
    // Order tracking
    std::unordered_map<uint64_t, Order> active_orders_;
    
    // External dependencies (initialized first)
    MemoryManager& memory_manager_;
    LatencyTracker& latency_tracker_;
    OrderManager* order_manager_;
    
    // Configuration (initialized early)
    std::string symbol_;
    
    // Trade ID generation (initialized before market state)
    std::atomic<uint64_t> next_trade_id_;
    
    // Market state (atomic for lock-free reads)
    std::atomic<price_t> best_bid_;
    std::atomic<price_t> best_ask_;
    std::atomic<quantity_t> best_bid_qty_;
    std::atomic<quantity_t> best_ask_qty_;
    std::atomic<price_t> last_trade_price_;  // Last trade price for statistics
    
    // Threading and synchronization
    mutable std::mutex book_mutex_;
    mutable std::mutex stats_mutex_;
    
    // Statistics and monitoring
    OrderBookStats statistics_;
    
    // Event callbacks
    BookUpdateCallback book_update_callback_;
    TradeCallback trade_callback_;
    DepthUpdateCallback depth_update_callback_;
    
    // Market maker order tracking
    std::unordered_set<uint64_t> our_orders_;  // Track which orders are ours
    mutable std::shared_mutex our_orders_mutex_;
    
    // Enhanced order tracking per price level
    std::unordered_map<uint64_t, price_t> order_to_price_;  // Order ID -> Price
    std::unordered_map<uint64_t, quantity_t> order_to_quantity_;  // Order ID -> Remaining quantity
    
    // Efficient order cancellation tracking (avoids O(n) queue reconstruction)
    std::unordered_set<uint64_t> cancelled_orders_;  // Track cancelled orders for lazy cleanup
    
    // =========================================================================
    // INTERNAL HELPER FUNCTIONS
    // =========================================================================
    
    // Matching engine core
    MatchResult match_order_internal(const Order& order, std::vector<TradeExecution>& executions);
    // Price level management
    void add_to_price_level(BookSide side, price_t price, const Order& order);
    void remove_from_price_level(BookSide side, price_t price, uint64_t order_id, 
                                quantity_t quantity);
    void update_price_level(BookSide side, price_t price, quantity_t old_qty, 
                           quantity_t new_qty);
    
    // Market data updates
    void update_top_of_book();
    void update_best_prices();  // Update cached best bid/ask prices
    void notify_book_update();
    void notify_trade_execution(const TradeExecution& trade);
    void notify_depth_update();
    
    // Performance tracking
    void update_statistics(const TradeExecution& trade);
    
    // Validation helpers
    bool validate_order(const Order& order) const;
    bool is_valid_price(price_t price) const;
    bool is_valid_quantity(quantity_t quantity) const;
    
    // Utility functions (inlined for performance)
    inline BookSide get_book_side(Side order_side) const {
        return (order_side == Side::BUY) ? BookSide::BID : BookSide::ASK;
    }
    
    inline Side get_opposite_side(Side side) const {
        return (side == Side::BUY) ? Side::SELL : Side::BUY;
    }
    
    price_t get_best_price(BookSide side) const;

    // Simple helper methods for book access
    std::map<price_t, PriceLevel, std::greater<price_t>>& get_bids() { return bids_; }
    std::map<price_t, PriceLevel, std::less<price_t>>& get_asks() { return asks_; }
    const std::map<price_t, PriceLevel, std::greater<price_t>>& get_bids() const { return bids_; }
    const std::map<price_t, PriceLevel, std::less<price_t>>& get_asks() const { return asks_; }
};

} // namespace hft
