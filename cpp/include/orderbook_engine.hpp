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
 * Matching result for order execution
 */
enum class MatchResult : uint8_t {
    NO_MATCH = 0,
    PARTIAL_FILL = 1,
    FULL_FILL = 2,
    REJECTED = 3
};

/**
 * Single price level in the order book
 */
struct PriceLevel {
    price_t price;
    quantity_t total_quantity;
    std::queue<uint64_t> order_queue;  // Queue of order IDs at this price level
    timestamp_t last_update;
    
    PriceLevel() : price(0), total_quantity(0), last_update(0) {}
    PriceLevel(price_t p) : price(p), total_quantity(0), last_update(0) {}
    
    void add_order(uint64_t order_id, quantity_t quantity) {
        order_queue.push(order_id);
        total_quantity += quantity;
        last_update = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
    
    void remove_order(quantity_t quantity) {
        total_quantity -= quantity;
        last_update = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
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
 * Market depth information (Level 2 data)
 */
struct MarketDepth {
    std::vector<PriceLevel> bids;    // Sorted highest to lowest
    std::vector<PriceLevel> asks;    // Sorted lowest to highest
    timestamp_t timestamp;
    uint32_t depth_levels;
    
    MarketDepth(uint32_t levels = 10) : depth_levels(levels), timestamp(now()) {
        bids.reserve(levels);
        asks.reserve(levels);
    }
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
    
    TradeExecution() : trade_id(0), aggressor_order_id(0), passive_order_id(0),
                       price(0.0), quantity(0.0), aggressor_side(Side::BUY),
                       timestamp(now()) {}
};

/**
 * Order book statistics for analytics
 */
struct OrderBookStats {
    uint64_t total_orders_processed;
    uint64_t total_trades;
    quantity_t total_volume;
    double avg_spread_bps;
    double avg_depth_bids;
    double avg_depth_asks;
    uint32_t updates_per_second;
    timestamp_t last_trade_time;
    
    OrderBookStats() : total_orders_processed(0), total_trades(0), total_volume(0.0),
                       avg_spread_bps(0.0), avg_depth_bids(0.0), avg_depth_asks(0.0),
                       updates_per_second(0), last_trade_time(now()) {}
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
     * Reset book to empty state
     */
    void clear_book();
    
    /**
     * Get order book statistics
     */
    OrderBookStats get_statistics() const;
    
    /**
     * Print current book state (for debugging)
     */
    void print_book_state(uint32_t levels = 5) const;
    
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
    
    /**
     * Notify about fill (called by matching engine)
     */
    void notify_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price, 
                    bool is_final_fill);
    
    /**
     * Notify about rejection
     */
    void notify_rejection(uint64_t order_id, const std::string& reason);
    
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
    LatencyMetrics get_matching_latency() const;
    
    /**
     * Print performance report
     */
    void print_performance_report() const;
    
    /**
     * Reset performance counters
     */
    void reset_performance_counters();
    
    // Market data integration methods
    void process_market_data_order(const Order& order);
    void process_market_data_cancel(uint64_t order_id);
    void process_market_data_trade(const TradeExecution& trade);
    
    // Market making specific methods
    void add_market_maker_order(const Order& order);
    bool is_our_order(uint64_t order_id) const;
    
private:
    // =========================================================================
    // INTERNAL ORDER BOOK DATA STRUCTURES
    // =========================================================================
    
    // Price-level maps (sorted for efficient matching)
    std::map<price_t, PriceLevel, std::greater<price_t>> bids_;  // Highest first
    std::map<price_t, PriceLevel, std::less<price_t>> asks_;     // Lowest first
    
    // Order tracking
    std::unordered_map<uint64_t, Order> active_orders_;
    
    // Market state (atomic for lock-free reads)
    std::atomic<price_t> best_bid_;
    std::atomic<price_t> best_ask_;
    std::atomic<quantity_t> best_bid_qty_;
    std::atomic<quantity_t> best_ask_qty_;
    std::atomic<price_t> last_trade_price_;  // Last trade price for statistics
    
    // Threading and synchronization
    mutable std::mutex book_mutex_;
    mutable std::mutex stats_mutex_;
    
    // External dependencies
    MemoryManager& memory_manager_;
    LatencyTracker& latency_tracker_;
    OrderManager* order_manager_;
    
    // Configuration
    std::string symbol_;
    
    // Statistics and monitoring
    OrderBookStats statistics_;
    std::atomic<uint64_t> next_trade_id_;
    
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
    
    // =========================================================================
    // INTERNAL HELPER FUNCTIONS
    // =========================================================================
    
    // Matching engine core
    MatchResult match_order_internal(const Order& order, std::vector<TradeExecution>& executions);
    void execute_trade(uint64_t aggressor_id, uint64_t passive_id, price_t price, 
                      quantity_t quantity, Side aggressor_side);
    
    // Price level management
    void add_to_price_level(BookSide side, price_t price, const Order& order);
    void remove_from_price_level(BookSide side, price_t price, uint64_t order_id, 
                                quantity_t quantity);
    void update_price_level(BookSide side, price_t price, quantity_t old_qty, 
                           quantity_t new_qty);
    
    // Market data updates
    void update_top_of_book();
    void notify_book_update();
    void notify_trade_execution(const TradeExecution& trade);
    void notify_depth_update();
    
    // Performance tracking
    void track_matching_latency(timestamp_t start_time);
    void update_statistics(const TradeExecution& trade);
    
    // Validation helpers
    bool validate_order(const Order& order) const;
    bool is_valid_price(price_t price) const;
    bool is_valid_quantity(quantity_t quantity) const;
    
    // Performance tracking
    void notify_matching_performance(const Order& order, double latency_us);

    // Utility functions
    BookSide get_book_side(Side order_side) const;
    Side get_opposite_side(Side side) const;
    price_t get_best_price(BookSide side) const;
    quantity_t get_quantity_at_price(BookSide side, price_t price) const;
};

} // namespace hft
