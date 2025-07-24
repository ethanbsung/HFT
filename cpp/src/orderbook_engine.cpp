#include "orderbook_engine.hpp"
#include "order_manager.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

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
        std::cout << "⚠️  WARNING: Found " << active_order_count << " remaining orders during shutdown" << std::endl;
        
        // Cancel all remaining orders if OrderManager is available
        if (order_manager_) {
            for (const auto& [order_id, order] : active_orders_) {
                order_manager_->handle_cancel_confirmation(order_id);
            }
            std::cout << "  Cancelled " << active_order_count << " remaining orders" << std::endl;
        } else {
            std::cout << "  (No OrderManager available - orders not cancelled)" << std::endl;
        }
    }
    
    // Print final order book statistics
    std::cout << "\n📊 FINAL ORDER BOOK STATISTICS:" << std::endl;
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
    std::cout << "\n💰 FINAL MARKET STATE:" << std::endl;
    std::cout << "  Best Bid: $" << std::fixed << std::setprecision(2) << best_bid_.load() 
              << " (" << best_bid_qty_.load() << ")" << std::endl;
    std::cout << "  Best Ask: $" << std::fixed << std::setprecision(2) << best_ask_.load() 
              << " (" << best_ask_qty_.load() << ")" << std::endl;
    std::cout << "  Price Levels: " << total_price_levels 
              << " (Bids: " << bids_.size() << ", Asks: " << asks_.size() << ")" << std::endl;
    
    // Performance metrics
    auto latency_metrics = latency_tracker_.get_metrics(LatencyType::ORDER_BOOK_UPDATE);
    if (latency_metrics.count > 0) {
        std::cout << "\n⚡ PERFORMANCE METRICS:" << std::endl;
        std::cout << "  Order Book Updates: " << latency_metrics.count << std::endl;
        std::cout << "  Avg Latency: " << std::fixed << std::setprecision(2) 
                  << latency_metrics.mean_us << " μs" << std::endl;
        std::cout << "  P99 Latency: " << std::fixed << std::setprecision(2) 
                  << latency_metrics.p99_us << " μs" << std::endl;
        std::cout << "  Max Latency: " << std::fixed << std::setprecision(2) 
                  << latency_metrics.max_us << " μs" << std::endl;
    }
    
    // Clean up data structures
    std::cout << "\n🧹 CLEANUP:" << std::endl;
    
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
    
    std::cout << "[ORDER BOOK] ✅ Order book engine shutdown complete for " << symbol_ << std::endl;
}

// =============================================================================
// CORE ORDER BOOK OPERATIONS (CRITICAL PATH)
// =============================================================================

MatchResult OrderBookEngine::add_order(const Order& order, std::vector<TradeExecution>& executions) {
    // This is the heart of the matching engine - order addition and matching
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_BOOK_UPDATE);
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Clear executions vector for this order
    executions.clear();
    
    // Validate incoming order
    if (!validate_order(order)) {
        notify_rejection(order.order_id, "Order validation failed");
        return MatchResult::REJECTED;
    }
    
    // Create a mutable copy of the order for matching
    Order working_order = order;
    MatchResult final_result = MatchResult::NO_MATCH;
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        statistics_.total_orders_processed++;
    }
    
    // Attempt to match the order against existing orders
    MatchResult match_result = match_order_internal(working_order, executions);
    
    // Handle matching results
    switch (match_result) {
        case MatchResult::FULL_FILL:
            // Order completely filled - nothing to add to book
            final_result = MatchResult::FULL_FILL;
            break;
            
        case MatchResult::PARTIAL_FILL:
            // Order partially filled - add remainder to book
            if (working_order.remaining_quantity > 0) {
                add_to_price_level(get_book_side(working_order.side), 
                                 working_order.price, working_order);
                active_orders_[working_order.order_id] = working_order;
                order_to_price_[working_order.order_id] = working_order.price;
            }
            final_result = MatchResult::PARTIAL_FILL;
            break;
            
        case MatchResult::NO_MATCH:
            // No match found - add entire order to book
            add_to_price_level(get_book_side(working_order.side), 
                             working_order.price, working_order);
            active_orders_[working_order.order_id] = working_order;
            order_to_price_[working_order.order_id] = working_order.price;
            final_result = MatchResult::NO_MATCH;
            break;
            
        case MatchResult::REJECTED:
            // Order was rejected during matching
            notify_rejection(working_order.order_id, "Order rejected during matching");
            final_result = MatchResult::REJECTED;
            break;
    }
    
    // Update top of book after any changes
    if (final_result != MatchResult::REJECTED) {
        update_top_of_book();
        notify_book_update();
        
        // If there were executions, notify about depth changes
        if (!executions.empty()) {
            notify_depth_update();
        }
    }
    
    // Process any trade executions
    for (const auto& execution : executions) {
        // Notify OrderManager about fills
        if (execution.aggressor_order_id == working_order.order_id) {
            // This order was the aggressor
            bool is_final_fill = (final_result == MatchResult::FULL_FILL);
            notify_fill(execution.aggressor_order_id, execution.quantity, 
                       execution.price, is_final_fill);
        }
        
        // Notify about the passive order fill
        auto passive_order_it = active_orders_.find(execution.passive_order_id);
        if (passive_order_it != active_orders_.end()) {
            // Update passive order's remaining quantity
            passive_order_it->second.remaining_quantity -= execution.quantity;
            
            bool passive_filled = (passive_order_it->second.remaining_quantity <= 0);
            notify_fill(execution.passive_order_id, execution.quantity, 
                       execution.price, passive_filled);
            
            // Remove passive order if completely filled
            if (passive_filled) {
                remove_from_price_level(get_book_side(get_opposite_side(working_order.side)),
                                      execution.price, execution.passive_order_id,
                                      execution.quantity);
                active_orders_.erase(execution.passive_order_id);
                order_to_price_.erase(execution.passive_order_id);
                order_to_quantity_.erase(execution.passive_order_id);
            }
        }
        
        // Notify trade callback
        notify_trade_execution(execution);
        
        // Update statistics
        update_statistics(execution);
    }
    
    return final_result;
}

bool OrderBookEngine::modify_order(uint64_t order_id, price_t new_price, quantity_t new_quantity) {
    // CRITICAL IMPLEMENTATION: Efficient order modification
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_BOOK_UPDATE);
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Find the order
    auto order_it = active_orders_.find(order_id);
    if (order_it == active_orders_.end()) {
        return false; // Order not found
    }
    
    Order& order = order_it->second;
    price_t old_price = order.price;
    quantity_t old_quantity = order.remaining_quantity;
    
    // If price changed, we need to move the order
    if (old_price != new_price) {
        // Remove from old price level
        auto& order_side = (order.side == Side::BUY) ? bids_ : asks_;
        remove_from_price_level(get_book_side(order.side), old_price, order_id, old_quantity);
        
        // Update order details
        order.price = new_price;
        order.remaining_quantity = new_quantity;
        order.last_update_time = now();
        
        // Add to new price level
        add_to_price_level(get_book_side(order.side), new_price, order);
        
        // Update tracking
        order_to_price_[order_id] = new_price;
        order_to_quantity_[order_id] = new_quantity;
    } else {
        // Just quantity change - update in place
        auto& order_side = (order.side == Side::BUY) ? bids_ : asks_;
        auto level_it = order_side.find(old_price);
        if (level_it != order_side.end()) {
            level_it->second.total_quantity = level_it->second.total_quantity - old_quantity + new_quantity;
        }
        
        // Update order details
        order.remaining_quantity = new_quantity;
        order.last_update_time = now();
        order_to_quantity_[order_id] = new_quantity;
    }
    
    // Update top of book
    update_best_prices();
    
    return true;
}

bool OrderBookEngine::cancel_order(uint64_t order_id) {
    // CRITICAL IMPLEMENTATION: Fast order removal from book structures
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_CANCELLATION);
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Find the order
    auto order_it = active_orders_.find(order_id);
    if (order_it == active_orders_.end()) {
        return false; // Order not found
    }
    
    const Order& order = order_it->second;
    price_t price = order_to_price_[order_id];
    quantity_t quantity = order_to_quantity_[order_id];
    
    // Remove from price level
    auto& order_side = (order.side == Side::BUY) ? bids_ : asks_;
    auto level_it = order_side.find(price);
    if (level_it != order_side.end()) {
        // Remove quantity from level
        level_it->second.remove_order(quantity);
        
        // Remove from order queue
        std::queue<uint64_t> temp_queue;
        while (!level_it->second.order_queue.empty()) {
            uint64_t front_id = level_it->second.order_queue.front();
            level_it->second.order_queue.pop();
            if (front_id != order_id) {
                temp_queue.push(front_id);
            }
        }
        level_it->second.order_queue = std::move(temp_queue);
        
        // Remove price level if empty
        if (level_it->second.total_quantity <= 0) {
            order_side.erase(level_it);
        }
    }
    
    // Clean up tracking
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
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    executions.clear();
    
    // Validate market order
    if (quantity <= 0) {
        return MatchResult::REJECTED;
    }
    
    // Determine which side of book to match against
    // Market buy orders match against asks (sell orders)
    // Market sell orders match against bids (buy orders)
    auto& matching_side = (side == Side::BUY) ? asks_ : bids_;
    
    if (matching_side.empty()) {
        return MatchResult::NO_MATCH; // No liquidity available
    }
    
    quantity_t remaining_quantity = quantity;
    bool any_matches = false;
    
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
                // Use memory pool for TradeExecution allocation
                TradeExecution* execution = memory_manager_.trade_execution_pool().acquire();
                if (!execution) {
                    std::cout << "❌ TradeExecution pool exhausted during market order!" << std::endl;
                    break;
                }
                
                // Generate unique trade ID for market order (use negative IDs to distinguish)
                uint64_t market_order_id = next_trade_id_.fetch_add(1) | 0x8000000000000000ULL; // Set MSB
                
                // Initialize the pooled trade execution
                execution->trade_id = next_trade_id_.fetch_add(1);
                execution->aggressor_order_id = market_order_id; // Market order gets synthetic ID
                execution->passive_order_id = passive_order_id;
                execution->price = level_price;  // Trade at passive order's price
                execution->quantity = trade_quantity;
                execution->aggressor_side = side;
                execution->timestamp = now();
                
                executions.push_back(*execution);
                memory_manager_.trade_execution_pool().release(execution);
                
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
    
    // Update book state
    update_best_prices();
    notify_book_update();
    
    // Notify callbacks about trades
    for (const auto& execution : executions) {
        notify_trade_execution(execution);
        update_statistics(execution);
    }
    
    // Determine final result
    if (remaining_quantity == 0) {
        return MatchResult::FULL_FILL;
    } else if (any_matches) {
        return MatchResult::PARTIAL_FILL;
    } else {
        return MatchResult::NO_MATCH;
    }
}

// =============================================================================
// MARKET DATA ACCESS (FOR SIGNAL ENGINE)
// =============================================================================

TopOfBook OrderBookEngine::get_top_of_book() const {
    // CRITICAL IMPLEMENTATION: Lock-free top of book read for maximum speed
    
    TopOfBook tob;
    
    // Read atomic values for best bid/ask (lock-free for speed)
    tob.bid_price = best_bid_.load(std::memory_order_relaxed);
    tob.ask_price = best_ask_.load(std::memory_order_relaxed);
    tob.bid_quantity = best_bid_qty_.load(std::memory_order_relaxed);
    tob.ask_quantity = best_ask_qty_.load(std::memory_order_relaxed);
    
    // Calculate derived values
    if (tob.bid_price > 0 && tob.ask_price > 0) {
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
    for (auto it = bids_.rbegin(); it != bids_.rend() && bid_count < levels; ++it, ++bid_count) {
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
    // Fast mid price calculation using atomic reads
    price_t bid = best_bid_.load(std::memory_order_relaxed);
    price_t ask = best_ask_.load(std::memory_order_relaxed);
    
    if (bid > 0 && ask > 0) {
        return (bid + ask) / 2.0;
    }
    return 0.0;
}

double OrderBookEngine::get_spread_bps() const {
    // Calculate spread in basis points for performance analysis
    price_t bid = best_bid_.load(std::memory_order_relaxed);
    price_t ask = best_ask_.load(std::memory_order_relaxed);
    
    if (bid > 0 && ask > 0 && ask > bid) {
        price_t mid = (bid + ask) / 2.0;
        if (mid > 0) {
            return ((ask - bid) / mid) * 10000.0; // Convert to basis points
        }
    }
    return 0.0;
}

bool OrderBookEngine::is_market_crossed() const {
    // Check if market is crossed (bid >= ask) - indicates data issue
    price_t bid = best_bid_.load(std::memory_order_relaxed);
    price_t ask = best_ask_.load(std::memory_order_relaxed);
    
    return (bid > 0 && ask > 0 && bid >= ask);
}

// =============================================================================
// ORDER BOOK STATE MANAGEMENT
// =============================================================================

void OrderBookEngine::apply_market_data_update(const MarketDepth& update) {
    // CRITICAL IMPLEMENTATION: Apply external market data feed efficiently
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
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
    
    // Update atomic best bid/ask
    update_best_prices();
    
    // Notify callbacks of update
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
    
    std::cout << "[ORDER BOOK] Cleared book for " << symbol_ << std::endl;
}

OrderBookStats OrderBookEngine::get_statistics() const {
    // Return comprehensive statistics for performance monitoring
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return statistics_;
}

void OrderBookEngine::print_book_state(uint32_t levels) const {
    // CRITICAL IMPLEMENTATION: Print current book state for debugging
    
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    std::cout << "\n=== ORDER BOOK STATE: " << symbol_ << " ===" << std::endl;
    std::cout << "Timestamp: " << std::chrono::duration_cast<std::chrono::milliseconds>(
        now().time_since_epoch()).count() << "ms" << std::endl;
    
    // Print ask levels (highest to lowest for visual clarity)
    std::cout << "\nASKS (Sell Orders):" << std::endl;
    std::cout << "Price     | Quantity  | Orders" << std::endl;
    std::cout << "----------|-----------|-------" << std::endl;
    
    uint32_t ask_count = 0;
    for (auto it = asks_.rbegin(); it != asks_.rend() && ask_count < levels; ++it, ++ask_count) {
        std::cout << std::fixed << std::setprecision(4) 
                  << std::setw(9) << it->first << " | "
                  << std::setw(9) << it->second.total_quantity << " | "
                  << it->second.order_queue.size() << std::endl;
    }
    
    // Print spread
    price_t bid = best_bid_.load();
    price_t ask = best_ask_.load();
    if (bid > 0 && ask > 0) {
        price_t spread = ask - bid;
        price_t mid = (bid + ask) / 2.0;
        double spread_bps = (spread / mid) * 10000.0;
        std::cout << "\n--- SPREAD: " << std::fixed << std::setprecision(4) 
                  << spread << " (" << std::setprecision(1) << spread_bps << " bps) ---" << std::endl;
    }
    
    // Print bid levels (highest to lowest)
    std::cout << "\nBIDS (Buy Orders):" << std::endl;
    std::cout << "Price     | Quantity  | Orders" << std::endl;
    std::cout << "----------|-----------|-------" << std::endl;
    
    uint32_t bid_count = 0;
    for (auto it = bids_.rbegin(); it != bids_.rend() && bid_count < levels; ++it, ++bid_count) {
        std::cout << std::fixed << std::setprecision(4)
                  << std::setw(9) << it->first << " | "
                  << std::setw(9) << it->second.total_quantity << " | "
                  << it->second.order_queue.size() << std::endl;
    }
    
    // Print summary statistics
    auto stats = get_statistics();
    std::cout << "\n=== SUMMARY STATISTICS ===" << std::endl;
    std::cout << "Total Orders Processed: " << stats.total_orders_processed << std::endl;
    std::cout << "Total Trades: " << stats.total_trades << std::endl;
    std::cout << "Total Volume: " << std::fixed << std::setprecision(2) << stats.total_volume << std::endl;
    std::cout << "Active Orders: " << active_orders_.size() << std::endl;
    std::cout << "Price Levels: " << (bids_.size() + asks_.size()) << 
                 " (" << bids_.size() << " bids, " << asks_.size() << " asks)" << std::endl;
    
    if (bid > 0 && ask > 0) {
        std::cout << "Best Bid: " << std::fixed << std::setprecision(4) << bid << std::endl;
        std::cout << "Best Ask: " << std::fixed << std::setprecision(4) << ask << std::endl;
        std::cout << "Mid Price: " << std::fixed << std::setprecision(4) << (bid + ask) / 2.0 << std::endl;
    }
    
    std::cout << "=================================" << std::endl;
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
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_BOOK_UPDATE);
    
    // Use the existing add_order implementation
    MatchResult result = add_order(order, executions);
    
    // Track that this is our own order (for market making)
    if (result != MatchResult::REJECTED) {
        std::lock_guard<std::shared_mutex> lock(our_orders_mutex_);
        our_orders_.insert(order.order_id);
    }
    
    return result;
}

void OrderBookEngine::notify_fill(uint64_t order_id, quantity_t fill_qty, 
                                 price_t fill_price, bool is_final_fill) {
    // TODO: Notify OrderManager about fill
    if (order_manager_) {
        order_manager_->handle_fill(order_id, fill_qty, fill_price, now(), is_final_fill);
    }
}

void OrderBookEngine::notify_rejection(uint64_t order_id, const std::string& reason) {
    // TODO: Notify OrderManager about rejection
    if (order_manager_) {
        order_manager_->handle_rejection(order_id, reason);
    }
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

LatencyMetrics OrderBookEngine::get_matching_latency() const {
    // Get latency metrics from latency tracker for performance analysis
    return latency_tracker_.get_statistics(LatencyType::ORDER_BOOK_UPDATE);
}

void OrderBookEngine::print_performance_report() const {
    // Print comprehensive performance report for analysis
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "📊 ORDER BOOK ENGINE PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    auto stats = get_statistics();
    auto latency_stats = get_matching_latency();
    
    // Print detailed statistics
    std::cout << "🔢 TRADING STATISTICS:" << std::endl;
    std::cout << "   Total Orders Processed: " << stats.total_orders_processed << std::endl;
    std::cout << "   Total Trades Executed: " << stats.total_trades << std::endl;
    std::cout << "   Total Volume Traded: " << std::fixed << std::setprecision(2) 
              << stats.total_volume << std::endl;
    
    if (stats.total_trades > 0) {
        std::cout << "   Average Trade Size: " << std::fixed << std::setprecision(2)
                  << (stats.total_volume / stats.total_trades) << std::endl;
    }
    
    std::cout << "\n⚡ PERFORMANCE METRICS:" << std::endl;
    std::cout << "   Average Latency: " << std::fixed << std::setprecision(1) 
              << latency_stats.mean_us << " μs" << std::endl;
    std::cout << "   95th Percentile: " << std::fixed << std::setprecision(1) 
              << latency_stats.p95_us << " μs" << std::endl;
    std::cout << "   99th Percentile: " << std::fixed << std::setprecision(1) 
              << latency_stats.p99_us << " μs" << std::endl;
    std::cout << "   Maximum Latency: " << std::fixed << std::setprecision(1) 
              << latency_stats.max_us << " μs" << std::endl;
    
    // Current market state
    std::cout << "\n📈 CURRENT MARKET STATE:" << std::endl;
    auto tob = get_top_of_book();
    if (tob.bid_price > 0 && tob.ask_price > 0) {
        std::cout << "   Best Bid: " << std::fixed << std::setprecision(4) << tob.bid_price 
                  << " (" << tob.bid_quantity << ")" << std::endl;
        std::cout << "   Best Ask: " << std::fixed << std::setprecision(4) << tob.ask_price 
                  << " (" << tob.ask_quantity << ")" << std::endl;
        std::cout << "   Mid Price: " << std::fixed << std::setprecision(4) << tob.mid_price << std::endl;
        std::cout << "   Spread: " << std::fixed << std::setprecision(1) << get_spread_bps() << " bps" << std::endl;
    } else {
        std::cout << "   No market data available" << std::endl;
    }
    
    // Add more metrics for HFT analysis
    std::cout << "\n🎯 HFT METRICS:" << std::endl;
    std::cout << "   Price Levels: " << (bids_.size() + asks_.size()) << std::endl;
    std::cout << "   Active Orders: " << active_orders_.size() << std::endl;
    std::cout << "   Market Crossed: " << (is_market_crossed() ? "YES ⚠️" : "NO") << std::endl;
    
    // Performance targets for HFT
    std::cout << "\n🎯 HFT PERFORMANCE TARGETS:" << std::endl;
    std::cout << "   Target Add Order: < 500ns" << std::endl;
    std::cout << "   Target Cancel: < 200ns" << std::endl;
    std::cout << "   Target Modify: < 300ns" << std::endl;
    std::cout << "   Current Avg: " << std::fixed << std::setprecision(0) 
              << latency_stats.mean_us * 1000 << "ns" << std::endl;
    
    std::cout << std::string(60, '=') << std::endl;
}

void OrderBookEngine::reset_performance_counters() {
    // Reset all performance counters for new session
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    statistics_ = OrderBookStats();
    
    std::cout << "[ORDER BOOK] Performance counters reset for " << symbol_ << std::endl;
}

// =============================================================================
// INTERNAL HELPER FUNCTIONS (PRIVATE)
// =============================================================================

MatchResult OrderBookEngine::match_order_internal(const Order& order, 
                                                  std::vector<TradeExecution>& executions) {
    auto& matching_side = (order.side == Side::BUY) ? asks_ : bids_;
    
    quantity_t remaining_quantity = order.quantity;
    bool any_matches = false;
    
    auto it = matching_side.begin();
    while (it != matching_side.end() && remaining_quantity > 0) {
        price_t level_price = it->first;
        PriceLevel& level = it->second;
        
        // Check if price is matchable
        bool can_match = false;
        if (order.side == Side::BUY) {
            can_match = (order.price >= level_price);
        } else {
            can_match = (order.price <= level_price);
        }
        
        if (!can_match) {
            break;  // No more matching prices
        }
        
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
                // **CRITICAL FIX: Use memory pool for TradeExecution allocation**
                TradeExecution* execution = memory_manager_.trade_execution_pool().acquire();
                if (!execution) {
                    std::cout << "❌ TradeExecution pool exhausted!" << std::endl;
                    break;  // Stop processing if we can't allocate
                }
                
                // Initialize the pooled trade execution
                execution->trade_id = next_trade_id_.fetch_add(1);
                execution->aggressor_order_id = order.order_id;
                execution->passive_order_id = passive_order_id;
                execution->price = level_price;  // Trade at passive order's price
                execution->quantity = trade_quantity;
                execution->aggressor_side = order.side;
                execution->timestamp = now();
                
                executions.push_back(*execution);  // Copy to vector for now
                
                // TODO: Optimize this - we should avoid copying and use pointers
                // Release back to pool after copying (not optimal, but safe for now)
                memory_manager_.trade_execution_pool().release(execution);
                
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
                }
            } else {
                // No quantity available, remove from queue
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
    
    // Determine match result
    if (remaining_quantity == 0) {
        return MatchResult::FULL_FILL;
    } else if (any_matches) {
        return MatchResult::PARTIAL_FILL;
    } else {
        return MatchResult::NO_MATCH;
    }
}

void OrderBookEngine::execute_trade(uint64_t aggressor_id, uint64_t passive_id, 
                                   price_t price, quantity_t quantity, Side aggressor_side) {
    // CRITICAL IMPLEMENTATION: Execute a trade between two orders
    
    TradeExecution* trade = memory_manager_.trade_execution_pool().acquire();
    if (!trade) {
        std::cout << "❌ TradeExecution pool exhausted in execute_trade!" << std::endl;
        return;
    }
    
    trade->trade_id = next_trade_id_.fetch_add(1);
    trade->aggressor_order_id = aggressor_id;
    trade->passive_order_id = passive_id;
    trade->price = price;
    trade->quantity = quantity;
    trade->aggressor_side = aggressor_side;
    trade->timestamp = now();
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        statistics_.total_trades++;
        statistics_.total_volume += quantity;
        statistics_.last_trade_time = trade->timestamp;
    }
    
    // Notify callbacks
    if (trade_callback_) {
        trade_callback_(*trade);
    }
    
    // Update OrderManager with fills
    if (order_manager_) {
        // Notify aggressor
        order_manager_->handle_fill(aggressor_id, quantity, price, trade->timestamp, true);
        // Notify passive order
        order_manager_->handle_fill(passive_id, quantity, price, trade->timestamp, true);
    }
    
    // Update last trade price
    last_trade_price_.store(price);
    
    // Release trade execution back to pool
    memory_manager_.trade_execution_pool().release(trade);
}

void OrderBookEngine::notify_matching_performance(const Order& order, double latency_us) {
    // Track matching performance for optimization
    latency_tracker_.add_order_placement_latency(latency_us);
    
    // Add to latency tracker for detailed analysis
    if (latency_us > 1000.0) { // > 1ms is slow for HFT
        std::cout << "⚠️ Slow matching detected: " << latency_us << "μs for order " 
                  << order.order_id << std::endl;
    }
}

void OrderBookEngine::add_to_price_level(BookSide side, price_t price, const Order& order) {
    auto& book_side = (side == BookSide::BID) ? bids_ : asks_;
    
    // Find or create price level
    auto& level = book_side[price];
    if (level.price == 0) {
        level.price = price;
    }
    
    // Add order to level
    level.add_order(order.order_id, order.remaining_quantity);
    
    // Update tracking maps
    order_to_quantity_[order.order_id] = order.remaining_quantity;
}

void OrderBookEngine::remove_from_price_level(BookSide side, price_t price, 
                                             uint64_t order_id, quantity_t quantity) {
    auto& book_side = (side == BookSide::BID) ? bids_ : asks_;
    
    // Find price level
    auto level_it = book_side.find(price);
    if (level_it == book_side.end()) {
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
        book_side.erase(level_it);
    }
    
    // Clean up tracking
    order_to_quantity_.erase(order_id);
}

void OrderBookEngine::update_price_level(BookSide side, price_t price, 
                                        quantity_t old_qty, quantity_t new_qty) {
    auto& book_side = (side == BookSide::BID) ? bids_ : asks_;
    
    auto level_it = book_side.find(price);
    if (level_it != book_side.end()) {
        level_it->second.total_quantity = level_it->second.total_quantity - old_qty + new_qty;
        level_it->second.last_update = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
}

void OrderBookEngine::update_top_of_book() {
    update_best_prices();
}

void OrderBookEngine::update_best_prices() {
    // Update best bid (highest price in bids) - optimized for HFT performance
    if (!bids_.empty()) {
        auto best_bid_it = bids_.rbegin(); // Highest price (last in sorted map)
        best_bid_.store(best_bid_it->first, std::memory_order_relaxed);
        best_bid_qty_.store(best_bid_it->second.total_quantity, std::memory_order_relaxed);
    } else {
        best_bid_.store(0.0, std::memory_order_relaxed);
        best_bid_qty_.store(0.0, std::memory_order_relaxed);
    }
    
    // Update best ask (lowest price in asks)  
    if (!asks_.empty()) {
        auto best_ask_it = asks_.begin(); // Lowest price (first in sorted map)
        best_ask_.store(best_ask_it->first, std::memory_order_relaxed);
        best_ask_qty_.store(best_ask_it->second.total_quantity, std::memory_order_relaxed);
    } else {
        best_ask_.store(std::numeric_limits<price_t>::max(), std::memory_order_relaxed);
        best_ask_qty_.store(0.0, std::memory_order_relaxed);
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
        // Notify about depth changes for both sides
        if (!bids_.empty()) {
            auto best_bid = bids_.rbegin();
            depth_update_callback_(symbol_, Side::BUY, best_bid->first, 
                                 best_bid->second.total_quantity);
        }
        if (!asks_.empty()) {
            auto best_ask = asks_.begin();
            depth_update_callback_(symbol_, Side::SELL, best_ask->first,
                                 best_ask->second.total_quantity);
        }
    }
}

void OrderBookEngine::track_matching_latency(timestamp_t start_time) {
    // TODO: Track matching performance
    auto end_time = now();
    auto latency_us = time_diff_us(start_time, end_time);
    // TODO: Add to latency tracker
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

BookSide OrderBookEngine::get_book_side(Side order_side) const {
    return (order_side == Side::BUY) ? BookSide::BID : BookSide::ASK;
}

Side OrderBookEngine::get_opposite_side(Side side) const {
    return (side == Side::BUY) ? Side::SELL : Side::BUY;
}

price_t OrderBookEngine::get_best_price(BookSide side) const {
    // Get best price for given side efficiently
    if (side == BookSide::BID) {
        return best_bid_.load(std::memory_order_relaxed);
    } else {
        return best_ask_.load(std::memory_order_relaxed);
    }
}

quantity_t OrderBookEngine::get_quantity_at_price(BookSide side, price_t price) const {
    // Get total quantity at specific price level
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    const auto& book_side = (side == BookSide::BID) ? bids_ : asks_;
    auto level_it = book_side.find(price);
    
    if (level_it != book_side.end()) {
        return level_it->second.total_quantity;
    }
    
    return 0.0;
}

// Market data integration methods
void OrderBookEngine::process_market_data_order(const Order& order) {
    std::lock_guard<std::shared_mutex> lock(book_mutex_);
    
    // Add market data order to our book
    active_orders_[order.order_id] = order;
    order_to_price_[order.order_id] = order.price;
    order_to_quantity_[order.order_id] = order.remaining_quantity;
    
    // Add to appropriate side of the book
    auto& order_side = (order.side == Side::BUY) ? bids_ : asks_;
    order_side[order.price].add_order(order.order_id, order.remaining_quantity);
    
    // Update best bid/ask
    update_best_prices();
    
    // Notify depth update
    if (depth_update_callback_) {
        depth_update_callback_(symbol_, order.side, order.price, 
                             order_side[order.price].total_quantity);
    }
}

void OrderBookEngine::process_market_data_cancel(uint64_t order_id) {
    std::lock_guard<std::shared_mutex> lock(book_mutex_);
    
    auto order_it = active_orders_.find(order_id);
    if (order_it == active_orders_.end()) {
        return; // Order not found
    }
    
    const Order& order = order_it->second;
    price_t price = order_to_price_[order_id];
    quantity_t quantity = order_to_quantity_[order_id];
    
    // Remove from price level
    auto& order_side = (order.side == Side::BUY) ? bids_ : asks_;
    auto level_it = order_side.find(price);
    if (level_it != order_side.end()) {
        level_it->second.remove_order(quantity);
        
        // Remove price level if empty
        if (level_it->second.total_quantity <= 0) {
            order_side.erase(level_it);
        }
    }
    
    // Clean up tracking
    active_orders_.erase(order_id);
    order_to_price_.erase(order_id);
    order_to_quantity_.erase(order_id);
    
    // Update best bid/ask
    update_best_prices();
    
    // Notify depth update
    if (depth_update_callback_) {
        depth_update_callback_(symbol_, order.side, price, 
                             level_it != order_side.end() ? level_it->second.total_quantity : 0);
    }
}

void OrderBookEngine::process_market_data_trade(const TradeExecution& trade) {
    std::lock_guard<std::shared_mutex> lock(book_mutex_);
    
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
}

void OrderBookEngine::add_market_maker_order(const Order& order) {
    {
        std::lock_guard<std::shared_mutex> lock(our_orders_mutex_);
        our_orders_.insert(order.order_id);
    }
    
    // Use the regular add_order method
    add_order(order);
}

bool OrderBookEngine::is_our_order(uint64_t order_id) const {
    std::shared_lock<std::shared_mutex> lock(our_orders_mutex_);
    return our_orders_.find(order_id) != our_orders_.end();
}

} // namespace hft
