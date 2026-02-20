#include "orderbook_engine.hpp"
#include "order_manager.hpp"
#include "log_control.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <random>

namespace hft {

// =============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =============================================================================

OrderBookEngine::OrderBookEngine(MemoryManager& memory_manager,
                                LatencyTracker& latency_tracker,
                                const std::string& symbol)
    : memory_manager_(memory_manager)
    , latency_tracker_(latency_tracker)
    , order_manager_(nullptr)
    , symbol_(symbol)
    , next_trade_id_(1)
    , best_bid_(0.0)
    , best_ask_(0.0)
    , best_bid_qty_(0.0)
    , best_ask_qty_(0.0)
    , last_trade_price_(0.0) {
    
    std::cout << "[ORDER BOOK] Initializing order book engine for symbol: " << symbol_ << std::endl;

    statistics_ = OrderBookStats();
    
    constexpr size_t INITIAL_ORDER_CAPACITY = 10000;
    active_orders_.reserve(INITIAL_ORDER_CAPACITY);
    order_to_price_.reserve(INITIAL_ORDER_CAPACITY);
    order_to_quantity_.reserve(INITIAL_ORDER_CAPACITY);
    
    // Initialize atomic market state to invalid values
    best_bid_.store(0.0, std::memory_order_relaxed);
    best_ask_.store(0.0, std::memory_order_relaxed);
    best_bid_qty_.store(0.0, std::memory_order_relaxed);
    best_ask_qty_.store(0.0, std::memory_order_relaxed);
    last_trade_price_.store(0.0, std::memory_order_relaxed);
    
    std::cout << "[ORDER BOOK] Order book engine initialized successfully" << std::endl;
    std::cout << "  Symbol: " << symbol_ << std::endl;
    std::cout << "  Reserved capacity: " << INITIAL_ORDER_CAPACITY << " orders" << std::endl;
    std::cout << "  Memory manager: " << &memory_manager_ << std::endl;
    std::cout << "  Latency tracker: " << &latency_tracker_ << std::endl;
}

OrderBookEngine::~OrderBookEngine() {
    std::cout << "[ORDER BOOK] Shutting down order book engine for symbol: " << symbol_ << std::endl;
    
    // Get final statistics before cleanup
    auto final_stats = get_statistics();
    
    // Check for any remaining orders and handle them
    size_t active_order_count = active_orders_.size();
    size_t total_price_levels = bids_.size() + asks_.size();
    
    if (active_order_count > 0) {
        std::cout << "  WARNING: Found " << active_order_count << " remaining orders during shutdown" << std::endl;
        
        // Note: OrderManager handles order cancellation during its own shutdown
        // We just clean up the order book data structures here
        std::cout << "  (Orders will be cancelled by OrderManager during shutdown)" << std::endl;
    }
    
    // Print final order book statistics
    std::cout << "\n FINAL ORDER BOOK STATISTICS:" << std::endl;
    std::cout << "  Total Orders Processed: " << final_stats.total_orders_processed << std::endl;
    std::cout << "  Total Trades Executed: " << final_stats.total_trades << std::endl;
    std::cout << "  Total Volume Traded: " << std::fixed << std::setprecision(2) 
              << final_stats.total_volume << std::endl;
    std::cout << "  Average Spread (bps): " << std::fixed << std::setprecision(1) 
              << final_stats.avg_spread_bps << std::endl;
    
    if (final_stats.total_trades > 0) {
        std::cout << "  Average Trade Size: " << std::fixed << std::setprecision(4) 
                  << (final_stats.total_volume / final_stats.total_trades) << std::endl;
    }
    
    // Print final market state
    std::cout << "\n FINAL MARKET STATE:" << std::endl;
    std::cout << "  Best Bid: $" << std::fixed << std::setprecision(2) << best_bid_.load() 
              << " (" << best_bid_qty_.load() << ")" << std::endl;
    std::cout << "  Best Ask: $" << std::fixed << std::setprecision(2) << best_ask_.load() 
              << " (" << best_ask_qty_.load() << ")" << std::endl;
    std::cout << "  Price Levels: " << total_price_levels 
              << " (Bids: " << bids_.size() << ", Asks: " << asks_.size() << ")" << std::endl;
    
    // Performance metrics
    auto latency_metrics = latency_tracker_.get_statistics(LatencyType::ORDER_BOOK_UPDATE);
    if (latency_metrics.count > 0) {
        std::cout << "\n PERFORMANCE METRICS:" << std::endl;
        std::cout << "  Order Book Updates: " << latency_metrics.count << std::endl;
        std::cout << "  Avg Latency: " << std::fixed << std::setprecision(2) 
                  << latency_metrics.mean_us << " us" << std::endl;
        std::cout << "  P99 Latency: " << std::fixed << std::setprecision(2) 
                  << latency_metrics.p99_us << " us" << std::endl;
        std::cout << "  Max Latency: " << std::fixed << std::setprecision(2) 
                  << latency_metrics.max_us << " us" << std::endl;
    }
    
    // Clean up data structures
    std::cout << "\n CLEANUP:" << std::endl;
    
    // Clear order book data
    active_orders_.clear();
    order_to_price_.clear();
    bids_.clear();
    asks_.clear();
    
    // Reset atomic values
    best_bid_.store(0.0, std::memory_order_relaxed);
    best_ask_.store(0.0, std::memory_order_relaxed);
    best_bid_qty_.store(0.0, std::memory_order_relaxed);
    best_ask_qty_.store(0.0, std::memory_order_relaxed);
    last_trade_price_.store(0.0, std::memory_order_relaxed);
    
    // Reset trade ID counter
    next_trade_id_.store(1);
    
    std::cout << "  Cleared " << active_order_count << " orders" << std::endl;
    std::cout << "  Cleared " << total_price_levels << " price levels" << std::endl;
    std::cout << "  Reset atomic market state" << std::endl;
    
    // Calculate session duration
    auto session_end = now();
    if (final_stats.last_trade_time > timestamp_t{}) {
        auto session_duration = time_diff_us(final_stats.last_trade_time, session_end);
        double session_seconds = to_microseconds(session_duration) / 1000000.0;
        std::cout << "  Session Duration: " << std::fixed << std::setprecision(1) 
                  << session_seconds << " seconds" << std::endl;
    }
    
    std::cout << "[ORDER BOOK]  Order book engine shutdown complete for " << symbol_ << std::endl;
}

// =============================================================================
// CORE ORDER BOOK OPERATIONS (CRITICAL PATH)
// =============================================================================

MatchResult OrderBookEngine::add_order(const Order& order, std::vector<TradeExecution>& executions) {
    // This is the heart of the matching engine - order addition and matching
    MEASURE_ORDER_BOOK_UPDATE_FAST(latency_tracker_);
    
    // Clear executions vector for this order
    executions.clear();
    
    // Validate incoming order before acquiring lock
    if (!validate_order(order)) {
        if (order_manager_) {
            order_manager_->handle_rejection(order.order_id, "Order validation failed");
        }
        return MatchResult::REJECTED;
    }
    
    // Variables to store callback data outside the lock
    bool should_notify_book_update = false;
    bool should_notify_depth_update = false;
    MatchResult final_result = MatchResult::NO_MATCH;
    
    // Critical section - hold lock only for book operations
    {
        std::lock_guard<std::mutex> lock(book_mutex_);
        
        // Create a mutable copy of the order for matching
        Order working_order = order;
        
        // Attempt to match the order against existing orders
        MatchResult match_result = match_order_internal(working_order, executions);
        
        // Update working_order's remaining_quantity based on executions
        quantity_t total_filled = 0.0;
        for (const auto& execution : executions) {
            total_filled += execution.quantity;
        }
        working_order.remaining_quantity = working_order.original_quantity - total_filled;
        
        // Handle matching results
        switch (match_result) {
            case MatchResult::FULL_FILL:
                // Order completely filled - nothing to add to book
                final_result = MatchResult::FULL_FILL;
                break;
                
            case MatchResult::PARTIAL_FILL:
                // Order partially filled - add remainder to book
                if (working_order.remaining_quantity > 0) {
                    // CRITICAL FIX: Add to active_orders_ BEFORE add_to_price_level for queue position tracking
                    active_orders_[working_order.order_id] = working_order;
                    order_to_price_[working_order.order_id] = working_order.price;
                    add_to_price_level(get_book_side(working_order.side), 
                                     working_order.price, working_order);
                }
                final_result = MatchResult::PARTIAL_FILL;
                break;
                
            case MatchResult::NO_MATCH:
                // No match found - add entire order to book
                // CRITICAL FIX: Add to active_orders_ BEFORE add_to_price_level for queue position tracking
                active_orders_[working_order.order_id] = working_order;
                order_to_price_[working_order.order_id] = working_order.price;
                add_to_price_level(get_book_side(working_order.side), 
                                 working_order.price, working_order);
                final_result = MatchResult::NO_MATCH;
                break;
                
            case MatchResult::REJECTED:
                // Order was rejected during matching
                final_result = MatchResult::REJECTED;
                break;
        }
        
        // Update top of book after any changes
        if (final_result != MatchResult::REJECTED) {
            update_top_of_book();
            should_notify_book_update = true;
            
            // Always notify about depth changes when book state changes
            should_notify_depth_update = true;
        }
        
        // Update statistics for trade executions
        for (const auto& execution : executions) {
            update_statistics(execution);
        }
        
        // Update order processing statistics (thread-safe update)
        if (final_result != MatchResult::REJECTED) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            statistics_.total_orders_processed++;
        }
    } // Lock released here
    
    // Call callbacks AFTER releasing the lock to prevent deadlock
    if (final_result == MatchResult::REJECTED) {
        if (order_manager_) {
            order_manager_->handle_rejection(order.order_id, "Order rejected during matching");
        }
    }
    
    if (should_notify_book_update) {
        notify_book_update();
    }
    
    if (should_notify_depth_update) {
        notify_depth_update();
    }
    
    // Process trade execution callbacks outside the lock
    for (const auto& execution : executions) {
        // Notify OrderManager about fills
        if (execution.aggressor_order_id == order.order_id) {
            // This order was the aggressor
            bool is_final_fill = (final_result == MatchResult::FULL_FILL);
            if (order_manager_) {
                order_manager_->handle_fill(execution.aggressor_order_id, execution.quantity,
                                           execution.price, execution.timestamp, is_final_fill);
            }
        }
        
        // Notify about passive order fills
        if (order_manager_) {
            order_manager_->handle_fill(execution.passive_order_id, execution.quantity,
                                       execution.price, execution.timestamp, true);
        }
        
        // Notify trade callback
        notify_trade_execution(execution);
    }
    
    return final_result;
}

bool OrderBookEngine::modify_order(uint64_t order_id, price_t new_price, quantity_t new_quantity) {
    // OPTIMIZED IMPLEMENTATION: Efficient order modification
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_BOOK_UPDATE);
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Find the order
    auto order_it = active_orders_.find(order_id);
    if (order_it == active_orders_.end()) {
        // Idempotent cancel behavior keeps OrderManager and SignalEngine from getting stuck
        // when market-data snapshots have already removed this order from the local book view.
        return true;
    }
    
    // Check if order is cancelled (lazy cancellation)
    if (cancelled_orders_.find(order_id) != cancelled_orders_.end()) {
        return false; // Order is cancelled
    }
    
    Order& order = order_it->second;
    price_t old_price = order.price;
    quantity_t old_quantity = order.remaining_quantity;
    BookSide book_side = get_book_side(order.side);
    
    // If price changed, we need to move the order
    if (old_price != new_price) {
        // Remove from old price level
        remove_from_price_level(book_side, old_price, order_id, old_quantity);
        
        // Update order details
        order.price = new_price;
        order.remaining_quantity = new_quantity;
        order.last_update_time = now();
        
        // Add to new price level
        add_to_price_level(book_side, new_price, order);
        
        // Update tracking maps
        order_to_price_[order_id] = new_price;
        order_to_quantity_[order_id] = new_quantity;
    } else {
        // Just update quantity at same price
        update_price_level(book_side, old_price, old_quantity, new_quantity);
        
        // Update order details
        order.remaining_quantity = new_quantity;
        order.last_update_time = now();
        
        // Update tracking maps
        order_to_quantity_[order_id] = new_quantity;
    }
    
    // Update top of book
    update_best_prices();
    
    return true;
}

bool OrderBookEngine::cancel_order(uint64_t order_id) {
    // OPTIMIZED IMPLEMENTATION: Efficient order cancellation using lazy cleanup
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_CANCELLATION);
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Find the order
    auto order_it = active_orders_.find(order_id);
    if (order_it == active_orders_.end()) {
        // Idempotent cancel keeps the higher-level order lifecycle from getting stuck
        // when this order has already been dropped from the local book representation.
        return true;
    }
    
    const Order& order = order_it->second;
    price_t price = order_to_price_[order_id];
    quantity_t quantity = order_to_quantity_[order_id];
    
    // Mark as cancelled for lazy cleanup (O(1) operation)
    cancelled_orders_.insert(order_id);
    
    // Update price level quantities immediately for accurate market data
    if (order.side == Side::BUY) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            level_it->second.remove_order(quantity);
            
            // Remove price level if empty
            if (level_it->second.total_quantity <= 0) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            level_it->second.remove_order(quantity);
            
            // Remove price level if empty
            if (level_it->second.total_quantity <= 0) {
                asks_.erase(level_it);
            }
        }
    }
    
    // Clean up tracking maps
    active_orders_.erase(order_id);
    order_to_price_.erase(order_id);
    order_to_quantity_.erase(order_id);
    
    // Update top of book
    update_best_prices();
    
    // Notify OrderManager of cancellation
    if (order_manager_) {
        order_manager_->handle_cancel_confirmation(order_id);
    }
    
    return true;
}

MatchResult OrderBookEngine::process_market_order(Side side, quantity_t quantity, 
                                                 std::vector<TradeExecution>& executions) {
    // CRITICAL IMPLEMENTATION: How market orders consume liquidity
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_BOOK_UPDATE);
    
    // Variables to hold data for callbacks outside the lock
    MatchResult final_result;
    std::vector<TradeExecution> executions_for_callbacks;
    
    // Critical section - hold lock only for book operations
    {
        std::lock_guard<std::mutex> lock(book_mutex_);
    
    executions.clear();
    
    // Validate market order
    if (quantity <= 0) {
        return MatchResult::REJECTED;
    }
    
    // Declare variables before if-else blocks to ensure proper scope
    quantity_t remaining_quantity = quantity;
    bool any_matches = false;
    
    // Get the opposite side to match against
    if (side == Side::BUY) {
        // For buy orders, match against ask side (lowest prices first)
        auto& matching_side = asks_;
        if (matching_side.empty()) {
            return MatchResult::NO_MATCH; // No liquidity available
        }
        
        // Walk through price levels consuming liquidity
        auto it = matching_side.begin();
        while (it != matching_side.end() && remaining_quantity > 0) {
            price_t level_price = it->first;
            PriceLevel& level = it->second;
            
            // Process orders in the queue at this price level (FIFO)
            while (!level.order_queue.empty() && remaining_quantity > 0) {
                uint64_t passive_order_id = level.order_queue.front();
                
                // Find the passive order details
                auto passive_order_it = active_orders_.find(passive_order_id);
                if (passive_order_it == active_orders_.end()) {
                    // Order not found - remove from queue and continue
                    level.order_queue.pop();
                    continue;
                }
                
                Order& passive_order = passive_order_it->second;
                quantity_t available_quantity = passive_order.remaining_quantity;
                quantity_t trade_quantity = std::min(remaining_quantity, available_quantity);
                
                if (trade_quantity > 0) {
                    // Create trade execution directly - more efficient than pool for short-lived objects
                    TradeExecution execution;
                    
                    // Generate unique trade ID for market order (use negative IDs to distinguish)
                    uint64_t market_order_id = next_trade_id_.fetch_add(1) | 0x8000000000000000ULL; // Set MSB
                    
                    // Initialize the trade execution
                    execution.trade_id = next_trade_id_.fetch_add(1);
                    execution.aggressor_order_id = market_order_id; // Market order gets synthetic ID
                    execution.passive_order_id = passive_order_id;
                    execution.price = level_price;  // Trade at passive order's price
                    execution.quantity = trade_quantity;
                    execution.aggressor_side = side;
                    execution.timestamp = now();
                    
                    executions.push_back(execution);
                    
                    // Update quantities
                    remaining_quantity -= trade_quantity;
                    passive_order.remaining_quantity -= trade_quantity;
                    level.total_quantity -= trade_quantity;
                    
                    any_matches = true;
                    
                    // Remove passive order if completely filled
                    if (passive_order.remaining_quantity <= 0) {
                        level.order_queue.pop();
                        active_orders_.erase(passive_order_id);
                        order_to_price_.erase(passive_order_id);
                        order_to_quantity_.erase(passive_order_id);
                        
                        // Notify OrderManager about passive order fill
                        if (order_manager_) {
                            order_manager_->handle_fill(passive_order_id, trade_quantity, 
                                                      level_price, now(), true);
                        }
                    } else {
                        // Notify about partial fill
                        if (order_manager_) {
                            order_manager_->handle_fill(passive_order_id, trade_quantity, 
                                                      level_price, now(), false);
                        }
                    }
                } else {
                    level.order_queue.pop();
                }
            }
            
            // Remove price level if no more orders
            if (level.order_queue.empty() || level.total_quantity <= 0) {
                it = matching_side.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        // For sell orders, match against bid side (highest prices first)
        auto& matching_side = bids_;
        if (matching_side.empty()) {
            return MatchResult::NO_MATCH; // No liquidity available
        }
        
        // Walk through price levels consuming liquidity
        auto it = matching_side.begin();
        while (it != matching_side.end() && remaining_quantity > 0) {
            price_t level_price = it->first;
            PriceLevel& level = it->second;
            
            // Process orders in the queue at this price level (FIFO)
            while (!level.order_queue.empty() && remaining_quantity > 0) {
                uint64_t passive_order_id = level.order_queue.front();
                
                // Find the passive order details
                auto passive_order_it = active_orders_.find(passive_order_id);
                if (passive_order_it == active_orders_.end()) {
                    // Order not found - remove from queue and continue
                    level.order_queue.pop();
                    continue;
                }
                
                Order& passive_order = passive_order_it->second;
                quantity_t available_quantity = passive_order.remaining_quantity;
                quantity_t trade_quantity = std::min(remaining_quantity, available_quantity);
                
                if (trade_quantity > 0) {
                    // Create trade execution directly - more efficient than pool for short-lived objects
                    TradeExecution execution;
                    
                    // Generate unique trade ID for market order (use negative IDs to distinguish)
                    uint64_t market_order_id = next_trade_id_.fetch_add(1) | 0x8000000000000000ULL; // Set MSB
                    
                    // Initialize the trade execution
                    execution.trade_id = next_trade_id_.fetch_add(1);
                    execution.aggressor_order_id = market_order_id; // Market order gets synthetic ID
                    execution.passive_order_id = passive_order_id;
                    execution.price = level_price;  // Trade at passive order's price
                    execution.quantity = trade_quantity;
                    execution.aggressor_side = side;
                    execution.timestamp = now();
                    
                    executions.push_back(execution);
                    
                    // Update quantities
                    remaining_quantity -= trade_quantity;
                    passive_order.remaining_quantity -= trade_quantity;
                    level.total_quantity -= trade_quantity;
                    
                    any_matches = true;
                    
                    // Remove passive order if completely filled
                    if (passive_order.remaining_quantity <= 0) {
                        level.order_queue.pop();
                        active_orders_.erase(passive_order_id);
                        order_to_price_.erase(passive_order_id);
                        order_to_quantity_.erase(passive_order_id);
                        
                        // Notify OrderManager about passive order fill
                        if (order_manager_) {
                            order_manager_->handle_fill(passive_order_id, trade_quantity, 
                                                      level_price, now(), true);
                        }
                    } else {
                        // Notify about partial fill
                        if (order_manager_) {
                            order_manager_->handle_fill(passive_order_id, trade_quantity, 
                                                      level_price, now(), false);
                        }
                    }
                } else {
                    level.order_queue.pop();
                }
            }
            
            // Remove price level if no more orders
            if (level.order_queue.empty() || level.total_quantity <= 0) {
                it = matching_side.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Update book state
    update_best_prices();
    
    // Determine final result before releasing lock (don't redeclare)
    if (remaining_quantity == 0) {
        final_result = MatchResult::FULL_FILL;
    } else if (any_matches) {
        final_result = MatchResult::PARTIAL_FILL;
    } else {
        final_result = MatchResult::NO_MATCH;
    }
    
    // Update statistics while still holding lock
    for (const auto& execution : executions) {
        update_statistics(execution);
    }
    
    // Store data for callbacks outside the lock
    executions_for_callbacks = executions;
    
    } // Release lock here (end of the critical section)
    
    // Call callbacks AFTER releasing the lock to prevent deadlock
    notify_book_update();
    
    // Notify callbacks about trades
    for (const auto& execution : executions_for_callbacks) {
        notify_trade_execution(execution);
    }
    
    return final_result;
}

// =============================================================================
// MARKET DATA ACCESS (FOR SIGNAL ENGINE)
// =============================================================================

TopOfBook OrderBookEngine::get_top_of_book() const {
    // CRITICAL IMPLEMENTATION: Lock-free top of book read for maximum speed
    
    TopOfBook tob;
    
    // Read atomic values with acquire semantics to ensure consistency
    tob.bid_price = best_bid_.load(std::memory_order_acquire);
    tob.ask_price = best_ask_.load(std::memory_order_acquire);
    tob.bid_quantity = best_bid_qty_.load(std::memory_order_acquire);
    tob.ask_quantity = best_ask_qty_.load(std::memory_order_acquire);
    
    // Calculate derived values
    if (tob.bid_price > 0 && tob.ask_price > 0 && tob.ask_price != std::numeric_limits<price_t>::max()) {
        tob.mid_price = (tob.bid_price + tob.ask_price) / 2.0;
        tob.spread = tob.ask_price - tob.bid_price;
    } else {
        tob.mid_price = 0.0;
        tob.spread = 0.0;
    }
    
    tob.timestamp = now();
    return tob;
}

MarketDepth OrderBookEngine::get_market_depth(uint32_t levels) const {
    // CRITICAL IMPLEMENTATION: Extract Level 2 market data efficiently
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    MarketDepth depth(levels);
    
    // Extract top N levels from bids (highest to lowest)
    uint32_t bid_count = 0;
    for (auto it = bids_.begin(); it != bids_.end() && bid_count < levels; ++it, ++bid_count) {
        depth.bids.emplace_back(it->first, it->second.total_quantity);
    }
    
    // Extract top N levels from asks (lowest to highest)
    uint32_t ask_count = 0;
    for (auto it = asks_.begin(); it != asks_.end() && ask_count < levels; ++it, ++ask_count) {
        depth.asks.emplace_back(it->first, it->second.total_quantity);
    }
    
    depth.timestamp = now();
    return depth;
}

price_t OrderBookEngine::get_mid_price() const {
    // Fast mid price calculation using atomic reads with proper memory ordering
    price_t bid = best_bid_.load(std::memory_order_acquire);
    price_t ask = best_ask_.load(std::memory_order_acquire);
    
    if (bid > 0 && ask > 0 && ask != std::numeric_limits<price_t>::max()) {
        return (bid + ask) / 2.0;
    }
    return 0.0;
}

double OrderBookEngine::get_spread_bps() const {
    // Calculate spread in basis points for performance analysis
    price_t bid = best_bid_.load(std::memory_order_acquire);
    price_t ask = best_ask_.load(std::memory_order_acquire);
    
    if (bid > 0 && ask > 0 && ask != std::numeric_limits<price_t>::max() && ask > bid) {
        price_t mid = (bid + ask) / 2.0;
        if (mid > 0) {
            return ((ask - bid) / mid) * 10000.0; // Convert to basis points
        }
    }
    return 0.0;
}

bool OrderBookEngine::is_market_crossed() const {
    // Check if market is crossed (bid >= ask) - indicates data issue
    price_t bid = best_bid_.load(std::memory_order_acquire);
    price_t ask = best_ask_.load(std::memory_order_acquire);
    
    return (bid > 0 && ask > 0 && ask != std::numeric_limits<price_t>::max() && bid >= ask);
}

// =============================================================================
// ORDER BOOK STATE MANAGEMENT
// =============================================================================

void OrderBookEngine::apply_market_data_update(const MarketDepth& update) {
    // CRITICAL IMPLEMENTATION: Apply external market data feed efficiently
    
    // Critical section - hold lock only for book operations
    {
        std::lock_guard<std::mutex> lock(book_mutex_);

        // Preserve our own resting orders across external snapshot refreshes.
        std::vector<Order> resting_our_orders;
        {
            std::shared_lock<std::shared_mutex> our_orders_lock(our_orders_mutex_);
            resting_our_orders.reserve(our_orders_.size());
            for (const auto& order_id : our_orders_) {
                auto it = active_orders_.find(order_id);
                if (it != active_orders_.end() && it->second.remaining_quantity > 0.0) {
                    resting_our_orders.push_back(it->second);
                }
            }
        }
        
        // Clear current book state for snapshot updates
        if (update.depth_levels > 0) {
            bids_.clear();
            asks_.clear();
            active_orders_.clear();
            order_to_price_.clear();
            order_to_quantity_.clear();
        }
        
        // Apply new price levels from market data
        uint64_t synthetic_order_id = 1000000; // Start synthetic IDs high to avoid conflicts
        
        // Apply bid levels
        for (const auto& bid_level : update.bids) {
            if (bid_level.quantity > 0) {
                PriceLevel& level = bids_[bid_level.price];
                level.price = bid_level.price;
                level.total_quantity = bid_level.quantity;
                level.last_update = update.timestamp;
                
                // Create synthetic order for this price level
                level.order_queue.push(synthetic_order_id);
                
                // Track in active orders (for market data orders)
                Order synthetic_order;
                synthetic_order.order_id = synthetic_order_id;
                synthetic_order.side = Side::BUY;
                synthetic_order.price = bid_level.price;
                synthetic_order.original_quantity = bid_level.quantity;
                synthetic_order.remaining_quantity = bid_level.quantity;
                synthetic_order.status = OrderStatus::ACTIVE;
                synthetic_order.entry_time = update.timestamp;
                
                active_orders_[synthetic_order_id] = synthetic_order;
                order_to_price_[synthetic_order_id] = bid_level.price;
                order_to_quantity_[synthetic_order_id] = bid_level.quantity;
                
                synthetic_order_id++;
            }
        }
        
        // Apply ask levels
        for (const auto& ask_level : update.asks) {
            if (ask_level.quantity > 0) {
                PriceLevel& level = asks_[ask_level.price];
                level.price = ask_level.price;
                level.total_quantity = ask_level.quantity;
                level.last_update = update.timestamp;
                
                // Create synthetic order for this price level
                level.order_queue.push(synthetic_order_id);
                
                // Track in active orders (for market data orders)
                Order synthetic_order;
                synthetic_order.order_id = synthetic_order_id;
                synthetic_order.side = Side::SELL;
                synthetic_order.price = ask_level.price;
                synthetic_order.original_quantity = ask_level.quantity;
                synthetic_order.remaining_quantity = ask_level.quantity;
                synthetic_order.status = OrderStatus::ACTIVE;
                synthetic_order.entry_time = update.timestamp;
                
                active_orders_[synthetic_order_id] = synthetic_order;
                order_to_price_[synthetic_order_id] = ask_level.price;
                order_to_quantity_[synthetic_order_id] = ask_level.quantity;
                
                synthetic_order_id++;
            }
        }

        // Reinsert our own resting orders so cancellations/requotes continue to work.
        for (const auto& order : resting_our_orders) {
            active_orders_[order.order_id] = order;
            order_to_price_[order.order_id] = order.price;
            add_to_price_level(get_book_side(order.side), order.price, order);
        }
        
        // Update atomic best bid/ask
        update_best_prices();
    } // Lock released here
    
    // Call callbacks AFTER releasing the lock to prevent deadlock
    notify_book_update();
    notify_depth_update();
}

void OrderBookEngine::clear_book() {
    // Reset book to empty state (used during disconnections)
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    bids_.clear();
    asks_.clear();
    active_orders_.clear();
    order_to_price_.clear();
    order_to_quantity_.clear();
    
    best_bid_.store(0.0, std::memory_order_relaxed);
    best_ask_.store(0.0, std::memory_order_relaxed);
    best_bid_qty_.store(0.0, std::memory_order_relaxed);
    best_ask_qty_.store(0.0, std::memory_order_relaxed);
    last_trade_price_.store(0.0, std::memory_order_relaxed);
}

OrderBookStats OrderBookEngine::get_statistics() const {
    // Return comprehensive statistics for performance monitoring
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return statistics_;
}

// =============================================================================
// INTEGRATION WITH ORDER MANAGER
// =============================================================================

void OrderBookEngine::set_order_manager(OrderManager* order_manager) {
    order_manager_ = order_manager;
    std::cout << "[ORDER BOOK] Connected to OrderManager" << std::endl;
}

MatchResult OrderBookEngine::submit_order_from_manager(const Order& order, std::vector<TradeExecution>& executions) {
    // This is the integration point between OrderManager and OrderBookEngine
    // OrderManager calls this when submit_order() is successful
    
    MEASURE_ORDER_BOOK_UPDATE_FAST(latency_tracker_);
    
    // Use the existing add_order implementation
    MatchResult result = add_order(order, executions);
    
    // Track that this is our own order (for market making)
    if (result != MatchResult::REJECTED) {
        std::lock_guard<std::shared_mutex> lock(our_orders_mutex_);
        our_orders_.insert(order.order_id);
        
        // Track queue position for realistic fill simulation
        track_queue_position(order.order_id, order.price, order.side, order.remaining_quantity);
    }
    
    return result;
}


// =============================================================================
// EVENT CALLBACKS (FOR SIGNAL ENGINE)
// =============================================================================

void OrderBookEngine::set_book_update_callback(BookUpdateCallback callback) {
    book_update_callback_ = callback;
}

void OrderBookEngine::set_trade_callback(TradeCallback callback) {
    trade_callback_ = callback;
}

void OrderBookEngine::set_depth_update_callback(DepthUpdateCallback callback) {
    depth_update_callback_ = callback;
}

// =============================================================================
// PERFORMANCE MONITORING
// =============================================================================

LatencyStatistics OrderBookEngine::get_matching_latency() const {
    // Get latency metrics from latency tracker for performance analysis
    return latency_tracker_.get_statistics(LatencyType::ORDER_BOOK_UPDATE);
}

void OrderBookEngine::reset_performance_counters() {
    // Reset all performance counters for new session
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    statistics_ = OrderBookStats();
}

// =============================================================================
// INTERNAL HELPER FUNCTIONS (PRIVATE)
// =============================================================================

MatchResult OrderBookEngine::match_order_internal(const Order& order, 
                                                  std::vector<TradeExecution>& executions) {
    quantity_t remaining_quantity = order.remaining_quantity;  // Use remaining_quantity not quantity
    bool any_matches = false;
    
    if (order.side == Side::BUY) {
        // Buy orders match against asks (lowest prices first)
        auto& matching_side = asks_;
        auto it = matching_side.begin();
        
        
        while (it != matching_side.end() && remaining_quantity > 0) {
            price_t level_price = it->first;
            PriceLevel& level = it->second;
            
            
            // Check if price is matchable (buy order price >= ask price)
            if (order.price < level_price) {
                break;  // No more matching prices
            }
            
            // Process orders in the queue at this price level (FIFO)
            while (!level.order_queue.empty() && remaining_quantity > 0) {
                uint64_t passive_order_id = level.order_queue.front();
                
                // Skip cancelled orders (efficient lazy cleanup)
                if (cancelled_orders_.find(passive_order_id) != cancelled_orders_.end()) {
                    level.order_queue.pop();
                    continue;
                }
                
                // Find the passive order details
                auto passive_order_it = active_orders_.find(passive_order_id);
                if (passive_order_it == active_orders_.end()) {
                    level.order_queue.pop();
                    continue;
                }
                
                Order& passive_order = passive_order_it->second;
                quantity_t available_quantity = passive_order.remaining_quantity;
                quantity_t trade_quantity = std::min(remaining_quantity, available_quantity);
                
                
                if (trade_quantity > 0) {
                    // Create trade execution directly in vector to avoid pool copy overhead
                    TradeExecution execution;
                    execution.trade_id = next_trade_id_.fetch_add(1);
                    execution.aggressor_order_id = order.order_id;
                    execution.passive_order_id = passive_order_id;
                    execution.price = level_price;  // Trade at passive order's price
                    execution.quantity = trade_quantity;
                    execution.aggressor_side = order.side;
                    execution.timestamp = now();
                    
                    executions.push_back(execution);
                    
                    // Update quantities
                    remaining_quantity -= trade_quantity;
                    passive_order.remaining_quantity -= trade_quantity;
                    level.total_quantity -= trade_quantity;
                    
                    any_matches = true;
                    
                    // Remove passive order if completely filled
                    if (passive_order.remaining_quantity <= 0) {
                        level.order_queue.pop();
                        active_orders_.erase(passive_order_id);
                        order_to_price_.erase(passive_order_id);
                        order_to_quantity_.erase(passive_order_id);
                    } else {
                        // Order partially filled - update tracking map and keep in queue
                        order_to_quantity_[passive_order_id] = passive_order.remaining_quantity;
                        
                        // Recalculate level total_quantity to ensure consistency
                        level.total_quantity = 0.0;
                        std::queue<uint64_t> temp_queue = level.order_queue;
                        while (!temp_queue.empty()) {
                            uint64_t oid = temp_queue.front();
                            temp_queue.pop();
                            auto order_it = active_orders_.find(oid);
                            if (order_it != active_orders_.end()) {
                                level.total_quantity += order_it->second.remaining_quantity;
                            }
                        }
                        
                        // Stop processing this level since the first order is partially filled
                        break;
                    }
                } else {
                    level.order_queue.pop();
                }
            }
            
            // Remove price level if no more orders
            if (level.order_queue.empty() || level.total_quantity <= 0) {
                it = matching_side.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        // SELL orders match against bids (highest prices first)
        auto& matching_side = bids_;
        auto it = matching_side.begin();  // Begin with highest prices
        
        while (it != matching_side.end() && remaining_quantity > 0) {
            price_t level_price = it->first;
            PriceLevel& level = it->second;
            
            // Check if price is matchable (sell order price <= bid price)
            if (order.price > level_price) {
                break;  // No more matching prices
            }
            
            // Process orders in the queue at this price level (FIFO)
            while (!level.order_queue.empty() && remaining_quantity > 0) {
                uint64_t passive_order_id = level.order_queue.front();
                
                // Skip cancelled orders (efficient lazy cleanup)
                if (cancelled_orders_.find(passive_order_id) != cancelled_orders_.end()) {
                    level.order_queue.pop();
                    continue;
                }
                
                // Find the passive order details
                auto passive_order_it = active_orders_.find(passive_order_id);
                if (passive_order_it == active_orders_.end()) {
                    level.order_queue.pop();
                    continue;
                }
                
                Order& passive_order = passive_order_it->second;
                quantity_t available_quantity = passive_order.remaining_quantity;
                quantity_t trade_quantity = std::min(remaining_quantity, available_quantity);
                
                if (trade_quantity > 0) {
                    // Create trade execution directly in vector
                    TradeExecution execution;
                    execution.trade_id = next_trade_id_.fetch_add(1);
                    execution.aggressor_order_id = order.order_id;
                    execution.passive_order_id = passive_order_id;
                    execution.price = level_price;  // Trade at passive order's price
                    execution.quantity = trade_quantity;
                    execution.aggressor_side = order.side;
                    execution.timestamp = now();
                    
                    executions.push_back(execution);
                    
                    // Update quantities
                    remaining_quantity -= trade_quantity;
                    passive_order.remaining_quantity -= trade_quantity;
                    level.total_quantity -= trade_quantity;
                    
                    any_matches = true;
                    
                    // Remove passive order if completely filled
                    if (passive_order.remaining_quantity <= 0) {
                        level.order_queue.pop();
                        active_orders_.erase(passive_order_id);
                        order_to_price_.erase(passive_order_id);
                        order_to_quantity_.erase(passive_order_id);
                    } else {
                        // Order partially filled - update tracking map and keep in queue
                        order_to_quantity_[passive_order_id] = passive_order.remaining_quantity;
                        // Recalculate level total_quantity to ensure consistency
                        level.total_quantity = 0.0;
                        std::queue<uint64_t> temp_queue = level.order_queue;
                        while (!temp_queue.empty()) {
                            uint64_t oid = temp_queue.front();
                            temp_queue.pop();
                            auto order_it = active_orders_.find(oid);
                            if (order_it != active_orders_.end()) {
                                level.total_quantity += order_it->second.remaining_quantity;
                            }
                        }
                        // Stop processing this level since the first order is partially filled
                        break;
                    }
                } else {
                    level.order_queue.pop();
                }
            }
            
            // Remove price level if no more orders
            if (level.order_queue.empty() || level.total_quantity <= 0) {
                it = matching_side.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Determine match result
    if (remaining_quantity == 0) {
        return MatchResult::FULL_FILL;
    } else if (any_matches) {
        return MatchResult::PARTIAL_FILL;
    } else {
        return MatchResult::NO_MATCH;
    }
}

void OrderBookEngine::add_to_price_level(BookSide side, price_t price, const Order& order) {
    // Calculate queue position BEFORE adding the order (FIFO queue simulation)
    quantity_t queue_ahead = 0.0;
    
    // Handle different map types for bids (std::greater) and asks (std::less)
    if (side == BookSide::BID) {
        auto& level = bids_[price];
        if (level.price == 0) {
            level.price = price;
        }
        
        // Calculate queue position: we're behind all existing orders at this price level
        queue_ahead = level.total_quantity;
        
        level.add_order(order.order_id, order.remaining_quantity);
    } else {
        auto& level = asks_[price];
        if (level.price == 0) {
            level.price = price;
        }
        
        // Calculate queue position: we're behind all existing orders at this price level
        queue_ahead = level.total_quantity;
        
        level.add_order(order.order_id, order.remaining_quantity);
    }
    
    // Update tracking maps with queue position
    order_to_quantity_[order.order_id] = order.remaining_quantity;
    
    // CRITICAL: Store the queue position for this order (FIFO queue simulation)
    // This mimics the Python execution simulator's queue_ahead calculation
    auto order_it = active_orders_.find(order.order_id);
    if (order_it != active_orders_.end()) {
        order_it->second.queue_ahead = queue_ahead;
        
        // Add to queue position tracking for fill simulation with precise queue position
        track_queue_position_with_exact_position(order.order_id, price, order.side, order.remaining_quantity, queue_ahead);
    }
}

void OrderBookEngine::remove_from_price_level(BookSide side, price_t price, 
                                             uint64_t order_id, quantity_t quantity) {
    // Handle different map types for bids (std::greater) and asks (std::less)
    if (side == BookSide::BID) {
        auto level_it = bids_.find(price);
        if (level_it == bids_.end()) {
            return; // Price level not found
        }
        
        // Remove order quantity from level
        level_it->second.remove_order(quantity);
        
        // Remove from order queue (this is expensive but necessary for correctness)
        std::queue<uint64_t> temp_queue;
        while (!level_it->second.order_queue.empty()) {
            uint64_t front_id = level_it->second.order_queue.front();
            level_it->second.order_queue.pop();
            if (front_id != order_id) {
                temp_queue.push(front_id);
            }
        }
        level_it->second.order_queue = std::move(temp_queue);
        
        // Remove level if empty
        if (level_it->second.total_quantity <= 0 || level_it->second.order_queue.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it == asks_.end()) {
            return; // Price level not found
        }
        
        // Remove order quantity from level
        level_it->second.remove_order(quantity);
        
        // Remove from order queue (this is expensive but necessary for correctness)
        std::queue<uint64_t> temp_queue;
        while (!level_it->second.order_queue.empty()) {
            uint64_t front_id = level_it->second.order_queue.front();
            level_it->second.order_queue.pop();
            if (front_id != order_id) {
                temp_queue.push(front_id);
            }
        }
        level_it->second.order_queue = std::move(temp_queue);
        
        // Remove level if empty
        if (level_it->second.total_quantity <= 0 || level_it->second.order_queue.empty()) {
            asks_.erase(level_it);
        }
    }
    
    // Clean up tracking
    order_to_quantity_.erase(order_id);
}

void OrderBookEngine::update_price_level(BookSide side, price_t price, 
                                        quantity_t old_qty, quantity_t new_qty) {
    // Handle different map types for bids (std::greater) and asks (std::less)
    if (side == BookSide::BID) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            level_it->second.total_quantity = level_it->second.total_quantity - old_qty + new_qty;
            level_it->second.last_update = now();
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            level_it->second.total_quantity = level_it->second.total_quantity - old_qty + new_qty;
            level_it->second.last_update = now();
        }
    }
}

void OrderBookEngine::update_top_of_book() {
    update_best_prices();
}

void OrderBookEngine::update_best_prices() {
    // Update atomic variables with proper synchronization
    // Note: This method should only be called while holding book_mutex_
    
    // Update best bid atomically
    if (!bids_.empty()) {
        auto best_bid_it = bids_.begin(); // Highest price (first in descending order map)
        price_t new_bid = best_bid_it->first;
        quantity_t new_bid_qty = best_bid_it->second.total_quantity;
        
        // Use release semantics to ensure all book updates are visible before price updates
        best_bid_.store(new_bid, std::memory_order_release);
        best_bid_qty_.store(new_bid_qty, std::memory_order_release);
    } else {
        best_bid_.store(0.0, std::memory_order_release);
        best_bid_qty_.store(0.0, std::memory_order_release);
    }
    
    // Update best ask atomically
    if (!asks_.empty()) {
        auto best_ask_it = asks_.begin(); // Lowest price (first in ascending order map)
        price_t new_ask = best_ask_it->first;
        quantity_t new_ask_qty = best_ask_it->second.total_quantity;
        
        // Use release semantics to ensure all book updates are visible before price updates
        best_ask_.store(new_ask, std::memory_order_release);
        best_ask_qty_.store(new_ask_qty, std::memory_order_release);
    } else {
        // PRODUCTION FIX: Use 0.0 instead of max value for empty ask side
        best_ask_.store(0.0, std::memory_order_release);
        best_ask_qty_.store(0.0, std::memory_order_release);
    }
}

void OrderBookEngine::notify_book_update() {
    if (book_update_callback_) {
        auto tob = get_top_of_book();
        book_update_callback_(tob);
    }
}

void OrderBookEngine::notify_trade_execution(const TradeExecution& trade) {
    if (trade_callback_) {
        trade_callback_(trade);
    }
}

void OrderBookEngine::notify_depth_update() {
    if (depth_update_callback_) {
        // Create a MarketDepth object with current book state
        MarketDepth depth = get_market_depth(10);  // Get top 10 levels
        depth_update_callback_(depth);
    }
}

void OrderBookEngine::update_statistics(const TradeExecution& trade) {
    // Update comprehensive statistics efficiently
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    statistics_.total_trades++;
    statistics_.total_volume += trade.quantity;
    statistics_.last_trade_time = trade.timestamp;
    
    // Update more detailed statistics for analysis
    if (statistics_.total_trades > 0) {
        // Calculate average spread (simplified - should use weighted average)
        double current_spread_bps = get_spread_bps();
        if (current_spread_bps > 0) {
            statistics_.avg_spread_bps = 
                ((statistics_.avg_spread_bps * (statistics_.total_trades - 1)) + current_spread_bps) / 
                statistics_.total_trades;
        }
    }
}

bool OrderBookEngine::validate_order(const Order& order) const {
    // Validate order parameters for safety and correctness
    return is_valid_price(order.price) && 
           is_valid_quantity(order.remaining_quantity) &&
           order.order_id > 0 &&
           order.original_quantity > 0 &&
           order.remaining_quantity <= order.original_quantity;
}

bool OrderBookEngine::is_valid_price(price_t price) const {
    // Price validation logic for HFT safety
    return price > 0.0 && 
           price < 1000000.0 && // Reasonable upper bound
           !std::isnan(price) && 
           !std::isinf(price);
}

bool OrderBookEngine::is_valid_quantity(quantity_t quantity) const {
    // Quantity validation logic for HFT safety
    return quantity > 0.0 && 
           quantity < 1000000.0 && // Reasonable upper bound
           !std::isnan(quantity) && 
           !std::isinf(quantity);
}



price_t OrderBookEngine::get_best_price(BookSide side) const {
    // Get best price for given side efficiently with proper memory ordering
    if (side == BookSide::BID) {
        return best_bid_.load(std::memory_order_acquire);
    } else {
        return best_ask_.load(std::memory_order_acquire);
    }
}

// Market data integration methods

void OrderBookEngine::process_market_data_trade(const TradeExecution& trade) {
    ScopedCoutSilencer silence_hot_path(!kEnableHotPathLogging);
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        statistics_.total_trades++;
    }
    last_trade_price_.store(trade.price);
    
    // Notify trade callback
    if (trade_callback_) {
        trade_callback_(trade);
    }
    
    
    // QUEUE-BASED FILL SIMULATION: Process fills based on queue position
    process_fills_from_queue_positions(trade);
    
    // Update queue positions for remaining orders
    update_queue_positions_from_trade(trade);
}

void OrderBookEngine::process_fills_from_queue_positions(const TradeExecution& trade) {
    // Process fills for all our orders based on their queue positions
    std::vector<std::pair<uint64_t, quantity_t>> fills_to_process;
    
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        
        for (auto& [order_id, pos] : queue_positions_) {
            if (pos.remaining_quantity <= 0.0) {
                continue; // Skip fully filled orders
            }
            
            quantity_t fill_qty = calculate_fill_from_queue_position(order_id, trade);
            if (fill_qty > 0.0) {
                fills_to_process.emplace_back(order_id, fill_qty);
            }
        }
    }
    
    // Process fills outside the queue lock to avoid deadlocks
    for (const auto& [order_id, fill_qty] : fills_to_process) {
        auto order_it = active_orders_.find(order_id);
        if (order_it != active_orders_.end()) {
            Order& order = order_it->second;
            
            // Update order state
            order.remaining_quantity -= fill_qty;
            bool is_final_fill = (order.remaining_quantity <= 0.0);
            
            // Create trade execution
            TradeExecution fill_trade;
            fill_trade.trade_id = next_trade_id_++;
            fill_trade.aggressor_order_id = trade.aggressor_order_id;
            fill_trade.passive_order_id = order_id;
            fill_trade.price = order.price;  // Fill at our order price
            fill_trade.quantity = fill_qty;
            fill_trade.aggressor_side = trade.aggressor_side;
            fill_trade.timestamp = now();
            
            
            // Notify fill
            if (order_manager_) {
                order_manager_->handle_fill(order_id, fill_qty, order.price, fill_trade.timestamp, is_final_fill);
            }
            
            // Update statistics
            update_statistics(fill_trade);
            
            // If fully filled, remove from active orders and queue positions
            if (is_final_fill) {
                active_orders_.erase(order_it);
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                queue_positions_.erase(order_id);
                
                std::lock_guard<std::shared_mutex> our_orders_lock(our_orders_mutex_);
                our_orders_.erase(order_id);
            }
        }
    }
}

void OrderBookEngine::simulate_market_order_from_trade(const TradeExecution& trade) {
    // REAL MARKET DATA APPROACH: Process actual trade to update FIFO queue positions
    // This mimics the Python execution simulator's _process_trade_update() logic
    
    
    // Process all our resting orders to update queue positions based on this real trade
    std::vector<uint64_t> orders_to_fill;
    
    {
        std::lock_guard<std::mutex> lock(book_mutex_);
        
        // const double TICK_SIZE = 0.01; // BTC-USD tick size (1 cent) - unused
        
        for (auto& [order_id, order] : active_orders_) {
            // FIXED: Check if this trade crosses our price levels (not just exact matches)
            bool trade_crosses_order = false;
            
            if (order.side == Side::BUY && trade.aggressor_side == Side::SELL) {
                // Buy order gets filled when someone sells at or below our bid price
                trade_crosses_order = (trade.price <= order.price);
            } else if (order.side == Side::SELL && trade.aggressor_side == Side::BUY) {
                // Sell order gets filled when someone buys at or above our ask price  
                trade_crosses_order = (trade.price >= order.price);
            }
            
            if (trade_crosses_order) {
                // CRITICAL LOGIC: Buy orders fill when someone SELLS (takes our bid)
                // Sell orders fill when someone BUYS (takes our ask)
                bool trade_affects_order = true;
                
                if (trade_affects_order) {
                    // Update queue position (reduce queue_ahead by trade quantity)
                    quantity_t old_queue = order.queue_ahead;
                    quantity_t new_queue = std::max(0.0, order.queue_ahead - trade.quantity);
                    order.queue_ahead = new_queue;
                    
                    std::cout << " QUEUE UPDATE: Order " << order_id 
                              << " queue: " << old_queue << "  " << new_queue 
                              << " (trade: " << trade.quantity << ")" << std::endl;
                    
                    // Check for fills when queue_ahead reaches 0
                    if (new_queue <= 0.0 && old_queue > 0.0) {
                        // Calculate how much volume reached our order
                        quantity_t volume_that_reached_us = std::max(0.0, trade.quantity - old_queue);
                        
                        // We can fill at most our remaining quantity or the volume that reached us
                        quantity_t fill_qty = std::min(order.remaining_quantity, volume_that_reached_us);
                        fill_qty = std::max(0.0, fill_qty);
                        
                        if (fill_qty > 0.0) {
                            
                            // Schedule this order for filling (outside the lock)
                            orders_to_fill.push_back(order_id);
                            
                            // Update order state for partial fills
                            if (fill_qty < order.remaining_quantity) {
                                order.remaining_quantity -= fill_qty;
                                order.queue_ahead = 0.0; // Now at front of queue for remaining quantity
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Process fills outside the lock to avoid deadlocks
    for (uint64_t order_id : orders_to_fill) {
        auto order_it = active_orders_.find(order_id);
        if (order_it != active_orders_.end()) {
            const auto& order = order_it->second;
            
            // Calculate fill quantity again (in case order changed)
            quantity_t fill_qty = std::min(order.original_quantity - order.remaining_quantity, trade.quantity);
            fill_qty = std::max(0.0, fill_qty);
            
            if (fill_qty > 0.0) {
                bool is_final_fill = (order.remaining_quantity <= 0.0);
                
                
                // Notify order manager about the fill
                if (order_manager_) {
                    order_manager_->handle_fill(order_id, fill_qty, trade.price, now(), is_final_fill);
                }
                
                // Remove completely filled orders
                if (is_final_fill) {
                    active_orders_.erase(order_id);
                    order_to_price_.erase(order_id);
                    order_to_quantity_.erase(order_id);
                    
                    // Remove from our order tracking
                    {
                        std::unique_lock<std::shared_mutex> lock(our_orders_mutex_);
                        our_orders_.erase(order_id);
                    }
                }
            }
        }
    }
}


// =============================================================================
// REAL MARKET DATA PROCESSING
// =============================================================================


// =============================================================================
// QUEUE POSITION TRACKING FOR REALISTIC FILLS
// =============================================================================

void OrderBookEngine::track_queue_position(uint64_t order_id, price_t price, Side side, quantity_t quantity) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    QueuePosition pos;
    pos.order_id = order_id;
    pos.price = price;
    pos.side = side;
    pos.original_quantity = quantity;
    pos.remaining_quantity = quantity;
    pos.entry_time = now();
    
    // FIXED: Calculate realistic queue position based on actual orderbook volumes
    // This implements the same logic as the Python version
    quantity_t queue_ahead = 0.0;
    
    if (side == Side::BUY) {
        // Find our price level in the bid stack
        std::lock_guard<std::mutex> book_lock(book_mutex_);
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            // Found our price level - we're joining existing orders
            // Assume we're behind 70-90% of existing volume (realistic time priority)
            quantity_t existing_volume = it->second.total_quantity;
            queue_ahead = existing_volume * (0.70 + (std::rand() % 21) / 100.0); // 70-90%
            queue_ahead = std::max(0.1, queue_ahead); // Minimum 0.1
        } else {
            // Price level doesn't exist yet - we'll be first at this level
            auto best_bid_it = bids_.begin();
            if (best_bid_it != bids_.end()) {
                price_t best_bid = best_bid_it->first;
                if (price < best_bid) {
                    // We're worse than best bid - small queue expected
                    double ticks_away = (best_bid - price) / 0.01; // Assuming 0.01 tick size
                    if (ticks_away <= 1.0) {
                        queue_ahead = 0.1 + (std::rand() % 10) / 10.0; // 0.1-1.0
                    } else {
                        queue_ahead = 0.05 + (std::rand() % 5) / 10.0; // 0.05-0.5
                    }
                } else if (price == best_bid) {
                    // Joining best bid - worst time priority
                    quantity_t best_bid_vol = best_bid_it->second.total_quantity;
                    queue_ahead = best_bid_vol * (0.85 + (std::rand() % 11) / 100.0); // 85-95%
                    queue_ahead = std::max(1.0, queue_ahead);
                }
            }
        }
    } else { // SELL
        // Find our price level in the ask stack
        std::lock_guard<std::mutex> book_lock(book_mutex_);
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            // Found our price level - we're joining existing orders
            quantity_t existing_volume = it->second.total_quantity;
            queue_ahead = existing_volume * (0.70 + (std::rand() % 21) / 100.0); // 70-90%
            queue_ahead = std::max(0.1, queue_ahead);
        } else {
            // Price level doesn't exist yet
            auto best_ask_it = asks_.begin();
            if (best_ask_it != asks_.end()) {
                price_t best_ask = best_ask_it->first;
                if (price > best_ask) {
                    // We're worse than best ask
                    double ticks_away = (price - best_ask) / 0.01;
                    if (ticks_away <= 1.0) {
                        queue_ahead = 0.1 + (std::rand() % 10) / 10.0;
                    } else {
                        queue_ahead = 0.05 + (std::rand() % 5) / 10.0;
                    }
                } else if (price == best_ask) {
                    // Joining best ask - worst time priority
                    quantity_t best_ask_vol = best_ask_it->second.total_quantity;
                    queue_ahead = best_ask_vol * (0.85 + (std::rand() % 11) / 100.0);
                    queue_ahead = std::max(1.0, queue_ahead);
                }
            }
        }
    }
    
    pos.queue_ahead = queue_ahead;
    queue_positions_[order_id] = pos;
    
}

void OrderBookEngine::track_queue_position_with_exact_position(uint64_t order_id, price_t price, Side side, quantity_t quantity, quantity_t exact_queue_ahead) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    QueuePosition pos;
    pos.order_id = order_id;
    pos.price = price;
    pos.side = side;
    pos.original_quantity = quantity;
    pos.remaining_quantity = quantity;
    pos.entry_time = now();
    pos.queue_ahead = exact_queue_ahead; // Use the exact queue position calculated
    
    queue_positions_[order_id] = pos;
    
}

void OrderBookEngine::update_queue_positions_from_trade(const TradeExecution& trade) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    // FIXED: Implement deterministic queue updates like Python version
    // Only update orders at the EXACT same price level where the trade occurred
    for (auto& [order_id, pos] : queue_positions_) {
        bool should_update = false;
        
        // Check if this order is at the exact price level where trade occurred
        if (pos.side == Side::BUY && trade.aggressor_side == Side::SELL) {
            // Buy orders at exactly the trade price get queue updates
            should_update = (pos.price == trade.price);
        } else if (pos.side == Side::SELL && trade.aggressor_side == Side::BUY) {
            // Sell orders at exactly the trade price get queue updates  
            should_update = (pos.price == trade.price);
        }
        
        if (should_update && pos.queue_ahead > 0.0) {
            // DETERMINISTIC: Queue advances by EXACTLY the trade quantity
            // This matches the Python logic: order.current_queue = max(0, order.current_queue - volume_decrease)
            quantity_t queue_reduction = std::min(pos.queue_ahead, trade.quantity);
            pos.queue_ahead = std::max(0.0, pos.queue_ahead - queue_reduction);
            
            // Queue position updated
        }
    }
}

quantity_t OrderBookEngine::calculate_fill_from_queue_position(uint64_t order_id, const TradeExecution& trade) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    auto it = queue_positions_.find(order_id);
    if (it == queue_positions_.end()) {
        return 0.0; // Order not tracked
    }
    
    QueuePosition& pos = it->second;
    
    // FIXED: Proper price crossing logic
    bool crosses = false;
    if (pos.side == Side::BUY && trade.aggressor_side == Side::SELL) {
        // Buy order fills when market sells at or below our bid price
        crosses = (trade.price <= pos.price);
    } else if (pos.side == Side::SELL && trade.aggressor_side == Side::BUY) {
        // Sell order fills when market buys at or above our ask price
        crosses = (trade.price >= pos.price);
    }
    
    if (!crosses) {
        return 0.0; // No fill - trade doesn't cross our order
    }
    
    // Fill check for order " << order_id
    
    // FIXED: Implement proper FIFO queue logic like Python version
    // Trade quantity must exceed queue ahead for any fill
    if (trade.quantity <= pos.queue_ahead) {
        // No fill: insufficient trade size
        return 0.0; // No fill - trade consumed by queue ahead (FIFO)
    }
    
    // Calculate available quantity after queue ahead is satisfied
    quantity_t available_to_fill = trade.quantity - pos.queue_ahead;
    
    // Our fill is the minimum of available quantity and our remaining quantity
    quantity_t fill_quantity = std::min(available_to_fill, pos.remaining_quantity);
    
    // Fill calculated: " << fill_quantity
    
    // Update position state
    pos.remaining_quantity -= fill_quantity;
    // After being filled, our queue position resets (we're no longer in queue for filled amount)
    pos.queue_ahead = std::max(0.0, pos.queue_ahead - trade.quantity);
    
    return fill_quantity;
}

} // namespace hft
