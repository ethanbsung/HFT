#include "order_manager.hpp"
#include "orderbook_engine.hpp"  // Full definition needed for method calls
#include "log_control.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace hft {

// =============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =============================================================================

OrderManager::OrderManager(MemoryManager& memory_manager,
                          LatencyTracker& latency_tracker,
                          const RiskLimits& risk_limits)
    : memory_manager_(memory_manager)
    , latency_tracker_(latency_tracker)
    , orderbook_engine_(nullptr)
    , engine_was_connected_(false)
    , risk_limits_(risk_limits)
    , next_order_id_(1)  // Start order IDs at 1 (0 = invalid)
    , is_emergency_shutdown_(false)
    , session_start_time_(now()) {
    
    std::cout << "[ORDER MANAGER] Initialized with risk limits:" << std::endl;
    std::cout << "  Max Position: " << risk_limits_.max_position << std::endl;
    std::cout << "  Max Daily Loss: $" << risk_limits_.max_daily_loss << std::endl;
    std::cout << "  Max Orders/sec: " << risk_limits_.max_orders_per_second << std::endl;

    current_position_ = PositionInfo();
    execution_stats_ = ExecutionStats();
    emergency_cancel_list_.reserve(risk_limits_.max_position);
}

OrderManager::~OrderManager() {
    
    std::cout << "[ORDER MANAGER] Shutting down..." << std::endl;
    
    // PRODUCTION FIX: Actually cancel all remaining orders instead of just warning
    std::vector<uint64_t> orders_to_cancel;
    orders_to_cancel.reserve(active_orders_.size() + pending_orders_.size());
    
    // Collect all orders that need cancellation
    for (const auto& order_id : active_orders_) {
        orders_to_cancel.push_back(order_id);
    }
    for (const auto& order_id : pending_orders_) {
        orders_to_cancel.push_back(order_id);
    }
    
    // Cancel them properly during shutdown
    if (!orders_to_cancel.empty()) {
        std::cout << " Cancelling " << orders_to_cancel.size() << " remaining orders..." << std::endl;
        for (uint64_t order_id : orders_to_cancel) {
            // Force cancel during shutdown - bypass engine callbacks to avoid deadlocks
            force_cancel_order_during_shutdown(order_id);
        }
        std::cout << " All remaining orders cancelled successfully" << std::endl;
    }
    
    // Get final statistics after cleanup
    auto final_stats = get_execution_stats();
    auto final_position = get_position();
    
    // Print final execution statistics
    std::cout << "\n FINAL SESSION STATISTICS:" << std::endl;
    std::cout << "  Orders Sent: " << final_stats.total_orders << std::endl;
    std::cout << "  Orders Filled: " << final_stats.filled_orders << std::endl;
    std::cout << "  Orders Cancelled: " << final_stats.cancelled_orders << std::endl;
    std::cout << "  Orders Rejected: " << final_stats.rejected_orders << std::endl;
    
    if (final_stats.total_orders > 0) {
        double fill_rate = static_cast<double>(final_stats.filled_orders) / final_stats.total_orders * 100.0;
        std::cout << "  Fill Rate: " << std::fixed << std::setprecision(1) << fill_rate << "%" << std::endl;
    }
    
    // Print final position information  
    std::cout << "\n FINAL POSITION:" << std::endl;
    std::cout << "  Net Position: " << final_position.net_position << std::endl;
    std::cout << "  Realized P&L: $" << std::fixed << std::setprecision(2) << final_position.realized_pnl << std::endl;
    std::cout << "  Unrealized P&L: $" << final_position.unrealized_pnl << std::endl;
    std::cout << "  Daily Volume: " << final_position.daily_volume << std::endl;
    std::cout << "  Trade Count: " << final_position.trade_count << std::endl;
    
    // Calculate session duration
    auto session_duration = time_diff_us(session_start_time_, now());
    double session_seconds = to_microseconds(session_duration) / 1000000.0;
    std::cout << "\n  SESSION DURATION: " << std::fixed << std::setprecision(2) 
              << session_seconds << " seconds" << std::endl;
    
    // Risk warnings
    if (std::abs(final_position.net_position) > 0.01) {
        std::cout << "  WARNING: Ending session with non-zero position!" << std::endl;
    }
    
    if (final_position.realized_pnl < -risk_limits_.max_daily_loss * 0.8) {
        std::cout << "  WARNING: Significant daily losses detected!" << std::endl;
    }
    
    std::cout << "[ORDER MANAGER]  Shutdown complete." << std::endl;
}

// =============================================================================
// CORE ORDER OPERATIONS (CRITICAL PATH - STUDY THESE PATTERNS)
// =============================================================================

uint64_t OrderManager::create_order(Side side, price_t price, quantity_t quantity,
                                    price_t current_mid_price) {
    ScopedCoutSilencer silence_hot_path(!kEnableHotPathLogging);
    MEASURE_ORDER_LATENCY_FAST(latency_tracker_);
    
    std::cout << " DEBUG: Creating order - Side: " << (side == Side::BUY ? "BUY" : "SELL")
              << " Price: $" << price << " Qty: " << quantity 
              << " Mid: $" << current_mid_price << std::endl;
    
    // Fast path: check emergency shutdown and basic validation
    if (is_emergency_shutdown_.load(std::memory_order_relaxed)) {
        std::cout << " DEBUG: Emergency shutdown active - rejecting order" << std::endl;
        return 0;
    }
    
    // PERFORMANCE: Combined validation in single check
    if (quantity <= 0.0 || price <= 0.0) {
        std::cout << " DEBUG: Invalid order parameters - qty: " << quantity << " price: " << price << std::endl;
        return 0;  // Reject invalid parameters
    }
    
    // PERFORMANCE: Inline risk check for critical path
    RiskCheckResult risk_result = check_pre_trade_risk(side, quantity, price);
    if (risk_result != RiskCheckResult::APPROVED) {
        std::cout << " DEBUG: Risk check failed - result: " << static_cast<int>(risk_result) << std::endl;
        // PERFORMANCE: Remove string conversion and cout in critical path
        return 0;
    }
    
    std::cout << " DEBUG: Risk check passed" << std::endl;
    
    // PERFORMANCE: Lock-free ID generation
    uint64_t order_id = generate_order_id();
    std::cout << " DEBUG: Generated order ID: " << order_id << std::endl;
    
    // PERFORMANCE: Direct memory pool access
    Order* pooled_order = memory_manager_.order_pool().acquire_order();
    if (!pooled_order) {
        std::cout << " DEBUG: Memory pool exhausted - cannot create order" << std::endl;
        return 0;  // Pool exhausted
    }
    
    std::cout << " DEBUG: Acquired order from memory pool" << std::endl;
    
    // PERFORMANCE: Single timestamp capture
    timestamp_t creation_time = now();
    
    // PERFORMANCE: Initialize pooled order efficiently
    pooled_order->order_id = order_id;
    pooled_order->side = side;
    pooled_order->price = price;
    pooled_order->original_quantity = quantity;
    pooled_order->remaining_quantity = quantity;
    pooled_order->status = OrderStatus::PENDING;
    pooled_order->entry_time = creation_time;
    pooled_order->last_update_time = creation_time;
    pooled_order->mid_price_at_entry = current_mid_price;
    
    // PERFORMANCE: Avoid copy construction, direct initialization
    OrderInfo& order_info = orders_[order_id];  // Direct reference to avoid copy
    order_info.order = *pooled_order;
    order_info.creation_time = creation_time;
    order_info.mid_price_at_creation = current_mid_price;
    order_info.execution_state = ExecutionState::PENDING_SUBMISSION;
    order_info.filled_quantity = 0.0;
    order_info.average_fill_price = 0.0;
    order_info.slippage = 0.0;
    order_info.time_in_queue_ms = 0.0;
    order_info.is_aggressive = false;
    order_info.modification_count = 0;
    order_info.mid_price_at_fill = 0.0;
    order_info.market_impact_bps = 0.0;
    
    // PERFORMANCE: Batch container updates
    pending_orders_.insert(order_id);
    pooled_orders_[order_id] = pooled_order;

    std::cout << " DEBUG: Order created successfully - ID: " << order_id 
              << " Status: PENDING_SUBMISSION" << std::endl;

    // PERFORMANCE: Minimize locked section
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.total_orders++;
        
        // PERFORMANCE: Avoid division in critical path
        if (execution_stats_.total_orders > 0) {
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
    }

    // PERFORMANCE: Optional callback at end
    if (order_callback_) {
        order_callback_(order_info);
    }
     
    return order_id;
}

bool OrderManager::modify_order(uint64_t order_id, price_t new_price, quantity_t new_quantity,
                               ModificationType mod_type) {
    if (is_emergency_shutdown_.load()) return false;
    
    // Fast order lookup (O(1) hash table access)
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) {
        return false; // Order not found
    }
    
    // Validate order can be modified
    if (order_info->execution_state == ExecutionState::FILLED ||
        order_info->execution_state == ExecutionState::CANCELLED ||
        order_info->execution_state == ExecutionState::REJECTED ||
        order_info->execution_state == ExecutionState::EXPIRED) {
        return false;
    }
    
    // Fast validation: ensure new quantity doesn't exceed original
    if ((mod_type == ModificationType::QUANTITY_ONLY || mod_type == ModificationType::PRICE_AND_QUANTITY) &&
        new_quantity > order_info->order.original_quantity) {
        return false;
    }
    
    // **CRITICAL FIX: Modify order in OrderBookEngine first (if available)**
    if (orderbook_engine_) {
        price_t engine_price = (mod_type == ModificationType::PRICE_ONLY || mod_type == ModificationType::PRICE_AND_QUANTITY) 
                               ? new_price : order_info->order.price;
        quantity_t engine_quantity = (mod_type == ModificationType::QUANTITY_ONLY || mod_type == ModificationType::PRICE_AND_QUANTITY)
                                     ? new_quantity : order_info->order.remaining_quantity;
        
        if (!orderbook_engine_->modify_order(order_id, engine_price, engine_quantity)) {
            // Engine modify failed - could implement cancel+resubmit fallback here
            std::cout << " DEBUG: Failed to modify order in OrderBookEngine - ID: " << order_id << std::endl;
            return false;
        }
    } else {
        // For submitted/active orders, we require the engine to be present
        if (order_info->execution_state == ExecutionState::SUBMITTED ||
            order_info->execution_state == ExecutionState::ACKNOWLEDGED) {
            std::cout << " DEBUG: No OrderBookEngine available - cannot modify active order ID: " << order_id << std::endl;
            return false;
        }
        std::cout << " WARNING: Modifying order " << order_id << " locally only (no engine connected)" << std::endl;
    }
    
    timestamp_t modification_time = now();
    
    // Update local state only after successful engine modification
    switch (mod_type) {
        case ModificationType::PRICE_ONLY:
            order_info->order.price = new_price;
            break;
            
        case ModificationType::QUANTITY_ONLY:
            // Only allow quantity reduction for risk management
            if (new_quantity <= order_info->order.remaining_quantity) {
                order_info->order.remaining_quantity = new_quantity;
            } else {
                return false;
            }
            break;
            
        case ModificationType::PRICE_AND_QUANTITY:
            order_info->order.price = new_price;
            if (new_quantity <= order_info->order.remaining_quantity) {
                order_info->order.remaining_quantity = new_quantity;
            } else {
                return false;
            }
            break;
    }
    
    order_info->order.last_update_time = modification_time;
    order_info->modification_count++;

    if (order_callback_) {
        order_callback_(*order_info);
    }

    return true;
}

bool OrderManager::cancel_order(uint64_t order_id) {
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_CANCELLATION);
    
    // Don't block cancellation during emergency shutdown - we need to cancel orders!
    
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) {
        std::cout << " DEBUG: Order not found for cancellation - ID: " << order_id << std::endl;
        return false;
    }
    
    // FIXED: Check if order is already cancelled to prevent duplicate cancellations
    if (order_info->execution_state == ExecutionState::CANCELLED) {
        std::cout << " DEBUG: Order already cancelled - ID: " << order_id << std::endl;
        return false;
    }
    
    // Can only cancel orders that are pending or active
    if (order_info->execution_state == ExecutionState::FILLED ||
        order_info->execution_state == ExecutionState::REJECTED) {
        std::cout << " DEBUG: Cannot cancel order in state " << static_cast<int>(order_info->execution_state) 
                  << " - ID: " << order_id << std::endl;
        return false;
    }
    
    std::cout << " DEBUG: Attempting to cancel order ID: " << order_id << std::endl;
    
    bool engine_cancelled = true;

    // Cancel order in OrderBookEngine first (if available)
    if (orderbook_engine_) {
        engine_cancelled = orderbook_engine_->cancel_order(order_id);
        if (!engine_cancelled) {
            std::cout << " WARNING: Engine did not confirm cancel for order " << order_id
                      << "; applying local cancellation fallback" << std::endl;
        }
    } else {
        // If engine was previously connected but now disconnected, be strict about active orders
        if (engine_was_connected_ && 
            (order_info->execution_state == ExecutionState::SUBMITTED ||
             order_info->execution_state == ExecutionState::ACKNOWLEDGED)) {
            std::cout << " DEBUG: OrderBookEngine was disconnected - cannot cancel active order ID: " << order_id << std::endl;
            return false;
        }
        
        // Allow local-only cancellation for testing scenarios
        if (order_info->execution_state == ExecutionState::SUBMITTED ||
            order_info->execution_state == ExecutionState::ACKNOWLEDGED) {
            std::cout << " WARNING: Cancelling active order " << order_id 
                      << " locally only (no engine connected) - may cause inconsistency" << std::endl;
        } else {
            std::cout << " WARNING: Cancelling order " << order_id << " locally only (no engine connected)" << std::endl;
        }
        engine_cancelled = false;
    }
    
    // Always update local state. This prevents stale active orders from blocking requotes.
    // If engine cancellation failed, this behaves like a fail-open cleanup path.
    order_info->execution_state = ExecutionState::CANCELLED;
    order_info->order.status = OrderStatus::CANCELLED;
    order_info->order.last_update_time = now();
    order_info->completion_time = order_info->order.last_update_time;
    
    // **CRITICAL FIX: Release order back to memory pool when cancelled**
    auto pooled_it = pooled_orders_.find(order_id);
    if (pooled_it != pooled_orders_.end()) {
        memory_manager_.order_pool().release_order(pooled_it->second);
        pooled_orders_.erase(pooled_it);
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.cancelled_orders++;
        
        if (execution_stats_.total_orders > 0) {
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
    }
    
    // Remove from tracking
    pending_orders_.erase(order_id);
    active_orders_.erase(order_id);
    
    if (order_callback_) {
        order_callback_(*order_info);
    }
    
    if (engine_cancelled) {
        std::cout << " CANCELLED: " << (order_info->order.side == Side::BUY ? "BID" : "ASK")
                  << " Order ID: " << order_id << std::endl;
    } else {
        std::cout << " CANCELLED-LOCAL: " << (order_info->order.side == Side::BUY ? "BID" : "ASK")
                  << " Order ID: " << order_id << std::endl;
    }
    
    return true;
}

// =============================================================================
// INTEGRATION WITH ORDER BOOK ENGINE
// =============================================================================

void OrderManager::set_orderbook_engine(OrderBookEngine* orderbook_engine) {
    orderbook_engine_ = orderbook_engine;
    if (orderbook_engine) {
        engine_was_connected_ = true;
    }
    std::cout << "[ORDER MANAGER] Connected to OrderBookEngine" << std::endl;
}

bool OrderManager::submit_order(uint64_t order_id) {
    ScopedCoutSilencer silence_hot_path(!kEnableHotPathLogging);
    // Emergency shutdown check
    if (is_emergency_shutdown_.load()) {
        std::cout << " DEBUG: Emergency shutdown active - cannot submit order " << order_id << std::endl;
        return false;
    }

    std::cout << " DEBUG: Submitting order ID: " << order_id << std::endl;

    // Fast order lookup
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) {
        std::cout << " DEBUG: Order not found - ID: " << order_id << std::endl;
        return false;
    }

    std::cout << " DEBUG: Order found - Side: " << (order_info->order.side == Side::BUY ? "BUY" : "SELL")
              << " Price: $" << order_info->order.price << " Qty: " << order_info->order.remaining_quantity
              << " Status: " << static_cast<int>(order_info->execution_state) << std::endl;

    // State validation: Only submit if pending submission
    if (order_info->execution_state != ExecutionState::PENDING_SUBMISSION) {
        std::cout << " DEBUG: Order not in PENDING_SUBMISSION state - current: " 
                  << static_cast<int>(order_info->execution_state) << std::endl;
        return false;
    }

    // Final risk check (optional, but good practice)
    RiskCheckResult risk_result = check_pre_trade_risk(order_info->order.side,
                                                       order_info->order.remaining_quantity,
                                                       order_info->order.price);
    if (risk_result != RiskCheckResult::APPROVED) {
        std::cout << " DEBUG: Final risk check failed - result: " << static_cast<int>(risk_result) << std::endl;
        if (risk_callback_) {
            risk_callback_(RiskViolationType::ORDER_RATE_LIMIT, 
                "Order submission risk check failed: " + risk_check_result_to_string(risk_result));
        }
        return false;
    }

    std::cout << " DEBUG: Final risk check passed" << std::endl;

    // Order rate limiting
    if (!check_order_rate_limit()) {
        std::cout << " DEBUG: Order rate limit exceeded" << std::endl;
        if (risk_callback_) {
            risk_callback_(RiskViolationType::ORDER_RATE_LIMIT, "Order rate limit exceeded");
        }
        return false;
    }

    std::cout << " DEBUG: Rate limit check passed" << std::endl;

    // Update order state
    order_info->execution_state = ExecutionState::SUBMITTED;
    order_info->order.status = OrderStatus::ACTIVE;
    order_info->order.last_update_time = now();
    order_info->submission_time = order_info->order.last_update_time;

    std::cout << " DEBUG: Order state updated to SUBMITTED" << std::endl;

    // Track order submission for rate limiting
    {
        std::lock_guard<std::mutex> lock(rate_limit_mutex_);
        recent_orders_.push(order_info->submission_time);
        
        // Clean up old entries (older than 1 second) to prevent queue from growing indefinitely
        timestamp_t one_second_ago = order_info->submission_time - std::chrono::seconds(1);
        while (!recent_orders_.empty() && recent_orders_.front() < one_second_ago) {
            recent_orders_.pop();
        }
    }

    // Move order from pending to active
    pending_orders_.erase(order_id);
    active_orders_.insert(order_id);

    std::cout << " DEBUG: Order moved from pending to active" << std::endl;

    // **CRITICAL INTEGRATION:** Submit order to OrderBookEngine for execution
    if (orderbook_engine_) {
        std::cout << " DEBUG: Submitting to OrderBookEngine..." << std::endl;
        std::vector<TradeExecution> executions;
        MatchResult result = orderbook_engine_->submit_order_from_manager(order_info->order, executions);
        
        std::cout << " DEBUG: OrderBookEngine result: " << static_cast<int>(result) 
                  << " Executions: " << executions.size() << std::endl;
        
        // Process any immediate executions
        for (const auto& execution : executions) {
            std::cout << " DEBUG: Immediate execution - Qty: " << execution.quantity 
                      << " Price: $" << execution.price << std::endl;
            handle_fill(order_id, execution.quantity, execution.price, now(), 
                       execution.quantity >= order_info->order.remaining_quantity);
        }
        
        // Handle immediate execution results
        switch (result) {
            case MatchResult::FULL_FILL:
                std::cout << " DEBUG: Order fully executed immediately" << std::endl;
                break;
            case MatchResult::PARTIAL_FILL:
                std::cout << " DEBUG: Order partially executed, remainder in book" << std::endl;
                break;
            case MatchResult::NO_MATCH:
                std::cout << " DEBUG: Order placed in book, waiting for match" << std::endl;
                break;
            case MatchResult::REJECTED:
                std::cout << " DEBUG: Order rejected by OrderBookEngine" << std::endl;
                order_info->execution_state = ExecutionState::REJECTED;
                order_info->order.status = OrderStatus::REJECTED;
                active_orders_.erase(order_id);
                return false;
        }
    } else {
        std::cout << " WARNING: No OrderBookEngine connected - order submitted to memory only" << std::endl;
    }

    double latency_us = to_microseconds(order_info->submission_time - order_info->creation_time);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.avg_submission_latency_us =
            ((execution_stats_.avg_submission_latency_us * (execution_stats_.total_orders - 1)) + latency_us)
            / execution_stats_.total_orders;
    }

    std::cout << " DEBUG: Order submission complete - Total latency: " << latency_us << " us" << std::endl;

    if (order_callback_) {
        order_callback_(*order_info);
    }

    return true;
}

// =============================================================================
// ORDER LIFECYCLE HANDLERS
// =============================================================================

bool OrderManager::handle_order_ack(uint64_t order_id, timestamp_t ack_time) {
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;

    // Validate that order is in expected state
    if (order_info->execution_state != ExecutionState::SUBMITTED) {
        return false;
    }

    order_info->execution_state = ExecutionState::ACKNOWLEDGED;
    order_info->acknowledgment_time = ack_time;

    if (order_callback_) {
        order_callback_(*order_info);
    }

    return true;
}

bool OrderManager::handle_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price,
                              timestamp_t fill_time, bool is_final_fill) {
    ScopedCoutSilencer silence_hot_path(!kEnableHotPathLogging);
    // Handle order execution (partial or full)
    // LEARNING GOAL: This is where trades happen! Very important.
    
    std::cout << "\n DEBUG: Processing fill for order " << order_id << std::endl;
    std::cout << "   Fill Qty: " << fill_qty << " @ $" << fill_price << std::endl;
    std::cout << "   Is Final: " << (is_final_fill ? "YES" : "NO") << std::endl;
    
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) {
        std::cout << " DEBUG: Order not found for fill - ID: " << order_id << std::endl;
        return false;
    }

    std::cout << " DEBUG: Order found - Side: " << (order_info->order.side == Side::BUY ? "BUY" : "SELL")
              << " Original Qty: " << order_info->order.original_quantity
              << " Remaining: " << order_info->order.remaining_quantity << std::endl;

    // Calculate volume-weighted average fill price for accumulating fills
    quantity_t previous_filled = order_info->filled_quantity;
    quantity_t total_filled = previous_filled + fill_qty;
    
    std::cout << " DEBUG: Previous filled: " << previous_filled 
              << " New fill: " << fill_qty << " Total: " << total_filled << std::endl;
    
    if (previous_filled == 0) {
        // First fill - use fill price directly
        order_info->average_fill_price = fill_price;
        std::cout << " DEBUG: First fill - avg price set to: $" << fill_price << std::endl;
    } else {
        // Subsequent fill - calculate volume-weighted average
        order_info->average_fill_price = 
            ((order_info->average_fill_price * previous_filled) + (fill_price * fill_qty)) / total_filled;
        std::cout << " DEBUG: Subsequent fill - new avg price: $" << order_info->average_fill_price << std::endl;
    }
    
    // Accumulate filled quantity
    order_info->filled_quantity = total_filled;
    order_info->completion_time = fill_time;
    
    // Set execution state based on whether this is the final fill
    if (is_final_fill) {
        order_info->execution_state = ExecutionState::FILLED;
        // Remove filled order from active orders
        active_orders_.erase(order_id);
        std::cout << " DEBUG: Order completely filled - removed from active orders" << std::endl;
        
        // **CRITICAL FIX: Release order back to memory pool when completely filled**
        auto pooled_it = pooled_orders_.find(order_id);
        if (pooled_it != pooled_orders_.end()) {
            memory_manager_.order_pool().release_order(pooled_it->second);
            pooled_orders_.erase(pooled_it);
            std::cout << " DEBUG: Released order back to memory pool" << std::endl;
        }
    } else {
        order_info->execution_state = ExecutionState::PARTIALLY_FILLED;
        std::cout << " DEBUG: Order partially filled - remaining: " << order_info->order.remaining_quantity << std::endl;
    }
    
    // Update position
    update_position(fill_qty, fill_price, order_info->order.side);
    std::cout << " DEBUG: Position updated" << std::endl;

    // Update daily volume and trade count
    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        current_position_.daily_volume += fill_qty;
        if (is_final_fill) {
            current_position_.trade_count++;
        }
    }

    // Calculate execution quality metrics
    order_info->slippage = fill_price - order_info->order.price;
    order_info->market_impact_bps = calculate_market_impact(fill_qty, fill_price);
    
    std::cout << " DEBUG: Slippage: $" << order_info->slippage 
              << " Market Impact: " << order_info->market_impact_bps << " bps" << std::endl;

    // Update performance statistics
    if (is_final_fill) {
        update_execution_stats(*order_info);
        
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            execution_stats_.filled_orders++;
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
        
        std::cout << " DEBUG: Updated execution statistics" << std::endl;
    }

    // Notify fill event
    if (fill_callback_) {
        fill_callback_(*order_info, fill_qty, fill_price, is_final_fill);
    }

    std::cout << " DEBUG: Fill processing complete" << std::endl;
    return true;
}

bool OrderManager::handle_rejection(uint64_t order_id, const std::string& reason) {
    // Note: reason parameter kept for future logging/debugging but currently unused
    (void)reason;  // Suppress unused parameter warning
    
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;

    order_info->execution_state = ExecutionState::REJECTED;
    order_info->order.status = OrderStatus::REJECTED;
    order_info->order.last_update_time = now();
    order_info->completion_time = order_info->order.last_update_time;
    
    // **CRITICAL FIX: Release order back to memory pool when rejected**
    auto pooled_it = pooled_orders_.find(order_id);
    if (pooled_it != pooled_orders_.end()) {
        memory_manager_.order_pool().release_order(pooled_it->second);
        pooled_orders_.erase(pooled_it);
    }
    
    // Update rejection statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.rejected_orders++;
        
        if (execution_stats_.total_orders > 0) {
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
    }

    if (order_callback_) {
        order_callback_(*order_info);
    }

    pending_orders_.erase(order_id);
    active_orders_.erase(order_id);

    return true;
}

bool OrderManager::handle_cancel_confirmation(uint64_t order_id) {
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;

    order_info->execution_state = ExecutionState::CANCELLED;
    order_info->order.status = OrderStatus::CANCELLED;
    order_info->order.last_update_time = now();
    order_info->completion_time = order_info->order.last_update_time;

    pending_orders_.erase(order_id);
    active_orders_.erase(order_id);

    if (order_callback_) {
        order_callback_(*order_info);
    }

    return true;
}

// =============================================================================
// RISK MANAGEMENT (CRITICAL FOR PREVENTING LOSSES)
// =============================================================================

RiskCheckResult OrderManager::check_pre_trade_risk(Side side, quantity_t quantity, price_t price) const {
    // Note: price parameter reserved for future price-based risk checks
    (void)price;  // Suppress unused parameter warning
    
    // Check position limits
    if (!check_position_limit(side, quantity)) return RiskCheckResult::POSITION_LIMIT_EXCEEDED;
    
    // Check daily loss limits
    if (!check_daily_loss_limit()) return RiskCheckResult::DAILY_LOSS_LIMIT_EXCEEDED;
    
    // Note: Rate limiting is checked during order submission, not creation

    return RiskCheckResult::APPROVED;
}

void OrderManager::update_risk_limits(const RiskLimits& new_limits) {
    // Update risk limits
    std::lock_guard<std::mutex> lock(position_mutex_);
    risk_limits_ = new_limits;
    
    std::cout << "[RISK] Updated risk limits" << std::endl;
}

void OrderManager::emergency_shutdown(const std::string& reason) {
    // Set emergency flag
    is_emergency_shutdown_.store(true);
    
    // Cancel all active orders - copy set to avoid iterator invalidation
    std::vector<uint64_t> orders_to_cancel;
    orders_to_cancel.reserve(active_orders_.size());
    for (const auto& order_id : active_orders_) {
        orders_to_cancel.push_back(order_id);
    }
    
    // Cancel orders from the copy
    for (auto order_id : orders_to_cancel) {
        cancel_order(order_id);
    }

    // Notify risk callback
    if (risk_callback_) {
        risk_callback_(RiskViolationType::CRITICAL_BREACH, "Emergency shutdown triggered: " + reason);
    }
    
    // Log emergency event
    std::cout << "[EMERGENCY] Shutting down: " << reason << std::endl;
}

// =============================================================================
// POSITION AND P&L TRACKING
// =============================================================================

PositionInfo OrderManager::get_position() const {
    // Thread-safe position read
    std::lock_guard<std::mutex> lock(position_mutex_);
    return current_position_;
}

void OrderManager::update_position(quantity_t quantity, price_t price, Side side) {
    std::lock_guard<std::mutex> lock(position_mutex_);
    
    // Store old position before any updates
    quantity_t old_position = current_position_.net_position;
    price_t old_avg_price = current_position_.avg_price;
    
    std::cout << " DEBUG: Updating position - Qty: " << quantity 
              << " Price: $" << price 
              << " Side: " << (side == Side::BUY ? "BUY" : "SELL") << std::endl;
    std::cout << " DEBUG: Old position: " << old_position 
              << " Old avg price: $" << old_avg_price << std::endl;
    
    // Convert trade to signed quantity (positive for buy, negative for sell)
    quantity_t trade_qty = (side == Side::BUY) ? quantity : -quantity;
    quantity_t new_position = old_position + trade_qty;
    
    std::cout << " DEBUG: Trade qty: " << trade_qty 
              << " New position: " << new_position << std::endl;
    
    // Calculate realized P&L when reducing or closing position
    if ((old_position > 0 && trade_qty < 0) || (old_position < 0 && trade_qty > 0)) {
        // Reducing or potentially flipping position
        quantity_t reduction = std::min(std::abs(trade_qty), std::abs(old_position));
        double pnl = 0.0;
        
        if (old_position > 0) {
            // Reducing long position: PnL = (sell_price - avg_cost) * reduction
            pnl = (price - old_avg_price) * reduction;
            std::cout << " DEBUG: Reducing long position - PnL: $" << pnl 
                      << " = ($" << price << " - $" << old_avg_price << ") * " << reduction << std::endl;
        } else {
            // Reducing short position: PnL = (avg_short_price - cover_price) * reduction
            pnl = (old_avg_price - price) * reduction;
            std::cout << " DEBUG: Reducing short position - PnL: $" << pnl 
                      << " = ($" << old_avg_price << " - $" << price << ") * " << reduction << std::endl;
        }
        
        current_position_.realized_pnl += pnl;
        std::cout << " DEBUG: Updated realized PnL: $" << current_position_.realized_pnl << std::endl;
    }
    
    // Update net position
    current_position_.net_position = new_position;
    
    // Update average price based on position change type
    if (new_position == 0) {
        // Position closed - reset average price
        current_position_.avg_price = 0.0;
        std::cout << " DEBUG: Position closed - avg price reset to 0" << std::endl;
    } else if (old_position == 0) {
        // Opening new position - use trade price
        current_position_.avg_price = price;
        std::cout << " DEBUG: Opening new position - avg price set to $" << price << std::endl;
    } else if ((old_position > 0 && new_position > 0 && new_position > old_position) ||
               (old_position < 0 && new_position < 0 && std::abs(new_position) > std::abs(old_position))) {
        // Increasing position in same direction - calculate volume-weighted average
        quantity_t total_quantity = std::abs(old_position) + quantity;
        current_position_.avg_price = 
            ((old_avg_price * std::abs(old_position)) + (price * quantity)) / total_quantity;
        std::cout << " DEBUG: Increasing position - new avg price: $" << current_position_.avg_price << std::endl;
    } else if ((old_position > 0 && new_position < 0) || (old_position < 0 && new_position > 0)) {
        // Position flipped - set average price to trade price for the new position
        current_position_.avg_price = price;
        std::cout << " DEBUG: Position flipped - avg price set to $" << price << std::endl;
    }
    // For position reduction (but not flip), keep the original average price
    
    std::cout << " DEBUG: Final position: " << current_position_.net_position 
              << " Final avg price: $" << current_position_.avg_price << std::endl;
}

double OrderManager::calculate_unrealized_pnl(price_t current_mid_price) const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    
    // No unrealized PnL if no position
    if (current_position_.net_position == 0) {
        return 0.0;
    }
    
    // No unrealized PnL if no valid current price
    if (current_mid_price <= 0.0) {
        return 0.0;
    }
    
    // No unrealized PnL if no valid average price
    if (current_position_.avg_price <= 0.0) {
        return 0.0;
    }

    double unrealized_pnl = current_position_.net_position * (current_mid_price - current_position_.avg_price);
    
    return unrealized_pnl;
}

// =============================================================================
// PERFORMANCE MONITORING
// =============================================================================

ExecutionStats OrderManager::get_execution_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return execution_stats_;
}

void OrderManager::print_performance_report() const {
    // Print comprehensive performance report
    // LEARNING GOAL: What metrics matter for trading performance?
    
    auto stats = get_execution_stats();
    auto position = get_position();
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << " ORDER MANAGER PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    // Order Statistics Section
    std::cout << "\n ORDER STATISTICS:" << std::endl;
    std::cout << "  Total Orders:     " << std::setw(10) << stats.total_orders << std::endl;
    std::cout << "  Filled Orders:    " << std::setw(10) << stats.filled_orders << std::endl;
    std::cout << "  Cancelled Orders: " << std::setw(10) << stats.cancelled_orders << std::endl;
    std::cout << "  Rejected Orders:  " << std::setw(10) << stats.rejected_orders << std::endl;
    
    // Calculate success rates
    if (stats.total_orders > 0) {
        double fill_rate = (double)stats.filled_orders / stats.total_orders * 100.0;
        double cancel_rate = (double)stats.cancelled_orders / stats.total_orders * 100.0;
        double reject_rate = (double)stats.rejected_orders / stats.total_orders * 100.0;
        
        std::cout << "  Fill Rate:        " << std::fixed << std::setprecision(2) 
                  << std::setw(8) << fill_rate << "%" << std::endl;
        std::cout << "  Cancel Rate:      " << std::fixed << std::setprecision(2) 
                  << std::setw(8) << cancel_rate << "%" << std::endl;
        std::cout << "  Reject Rate:      " << std::fixed << std::setprecision(2) 
                  << std::setw(8) << reject_rate << "%" << std::endl;
    }
    
    // Performance Metrics Section
    std::cout << "\n PERFORMANCE METRICS:" << std::endl;
    std::cout << "  Avg Submission Latency: " << std::fixed << std::setprecision(3) 
              << std::setw(8) << stats.avg_submission_latency_us << " us" << std::endl;
    std::cout << "  Avg Fill Time:          " << std::fixed << std::setprecision(3) 
              << std::setw(8) << stats.avg_fill_time_ms << " ms" << std::endl;
    std::cout << "  Avg Cancel Time:        " << std::fixed << std::setprecision(3) 
              << std::setw(8) << stats.avg_cancel_time_ms << " ms" << std::endl;
    
    // Execution Quality Section
    std::cout << "\n EXECUTION QUALITY:" << std::endl;
    std::cout << "  Avg Slippage:      " << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.avg_slippage_bps << " bps" << std::endl;
    std::cout << "  Avg Market Impact: " << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.avg_market_impact_bps << " bps" << std::endl;
    
    // Position Information Section
    std::cout << "\n CURRENT POSITION:" << std::endl;
    std::cout << "  Net Position:      " << std::fixed << std::setprecision(0) 
              << std::setw(10) << position.net_position << std::endl;
    std::cout << "  Average Price:     $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << position.avg_price << std::endl;
    std::cout << "  Realized P&L:      $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << position.realized_pnl << std::endl;
    std::cout << "  Unrealized P&L:    $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << position.unrealized_pnl << std::endl;
    std::cout << "  Total P&L:         $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << (position.realized_pnl + position.unrealized_pnl) << std::endl;
    std::cout << "  Daily Volume:      " << std::fixed << std::setprecision(0) 
              << std::setw(10) << position.daily_volume << std::endl;
    std::cout << "  Trade Count:       " << std::setw(10) << position.trade_count << std::endl;
    
    // Risk Metrics Section
    std::cout << "\n  RISK METRICS:" << std::endl;
    std::cout << "  Risk Violations:   " << std::setw(10) << stats.risk_violations << std::endl;
    std::cout << "  Max Daily Loss:    $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.max_daily_loss << std::endl;
    std::cout << "  Current Drawdown:  $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.current_drawdown << std::endl;
    std::cout << "  Gross Exposure:    $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << position.gross_exposure << std::endl;
    
    // Active Orders Section
    auto active_orders = get_active_orders();
    std::cout << "\n ACTIVE ORDERS:" << std::endl;
    std::cout << "  Active Orders:     " << std::setw(10) << active_orders.size() << std::endl;
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "Report generated at: " << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << std::endl;
    std::cout << std::string(60, '=') << std::endl;
}

// =============================================================================
// PRIVATE HELPER FUNCTIONS
// =============================================================================

uint64_t OrderManager::generate_order_id() noexcept {
    // Lock-free order ID generation
    return next_order_id_.fetch_add(1);
}

OrderInfo* OrderManager::find_order(uint64_t order_id) noexcept {
    // Fast order lookup
    auto it = orders_.find(order_id);
    return (it != orders_.end()) ? &it->second : nullptr;
}

const OrderInfo* OrderManager::find_order(uint64_t order_id) const noexcept {
    // Const version of find_order
    auto it = orders_.find(order_id);
    return (it != orders_.end()) ? &it->second : nullptr;
}

bool OrderManager::check_position_limit(Side side, quantity_t quantity) const noexcept {
    // PRODUCTION FIX: Proper position limit validation
    std::lock_guard<std::mutex> lock(position_mutex_);
    
    // Calculate what the new position would be if this order were filled
    quantity_t hypothetical_position = current_position_.net_position;
    if (side == Side::BUY) {
        hypothetical_position += quantity;
    } else {
        hypothetical_position -= quantity;
    }
    
    // Reject if the absolute position would exceed the limit
    bool within_limit = std::abs(hypothetical_position) <= risk_limits_.max_position;
    
    if (!within_limit) {
        std::cout << " Position limit check failed: "
                  << "Current: " << current_position_.net_position 
                  << ", Proposed: " << hypothetical_position 
                  << ", Limit: " << risk_limits_.max_position << std::endl;
    }
    
    return within_limit;
}

bool OrderManager::check_daily_loss_limit() const noexcept {
    // Check if daily losses exceed limit
    return current_position_.realized_pnl > -risk_limits_.max_daily_loss;
}

bool OrderManager::check_order_rate_limit() const noexcept {
    // Check order rate limiting using sliding window approach
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    
    timestamp_t current_time = now();
    timestamp_t one_second_ago = current_time - std::chrono::seconds(1);
    
    // Remove timestamps older than 1 second (sliding window cleanup)
    while (!recent_orders_.empty() && recent_orders_.front() < one_second_ago) {
        recent_orders_.pop();
    }
    
    // Check if current rate would exceed limit (including the order we're about to submit)
    return recent_orders_.size() < risk_limits_.max_orders_per_second;
}

std::vector<uint64_t> OrderManager::get_active_orders() const {
    std::vector<uint64_t> result;
    result.reserve(active_orders_.size());
    for (const auto& order_id : active_orders_) {
        result.push_back(order_id);
    }
    return result;
}

const OrderInfo* OrderManager::get_order_info(uint64_t order_id) const {
    return find_order(order_id);
}

void OrderManager::reset_daily_stats() {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    std::lock_guard<std::mutex> position_lock(position_mutex_);
    
    // Reset execution statistics
    execution_stats_.total_orders = 0;
    execution_stats_.filled_orders = 0;
    execution_stats_.cancelled_orders = 0;
    execution_stats_.rejected_orders = 0;
    execution_stats_.avg_submission_latency_us = 0.0;
    execution_stats_.avg_fill_time_ms = 0.0;
    execution_stats_.avg_cancel_time_ms = 0.0;
    execution_stats_.fill_rate = 0.0;
    execution_stats_.avg_slippage_bps = 0.0;
    execution_stats_.avg_market_impact_bps = 0.0;
    execution_stats_.risk_violations = 0;
    execution_stats_.current_drawdown = 0.0;
    
    // Reset position daily stats
    current_position_.daily_volume = 0.0;
    current_position_.trade_count = 0;
    
    // Reset session start time
    session_start_time_ = now();
    
    std::cout << "[ORDER MANAGER] Daily statistics reset" << std::endl;
}

void OrderManager::set_order_callback(OrderCallback callback) {
    order_callback_ = callback;
}

void OrderManager::set_fill_callback(FillCallback callback) {
    fill_callback_ = callback;
}

void OrderManager::set_risk_callback(RiskCallback callback) {
    risk_callback_ = callback;
}

double OrderManager::calculate_market_impact(quantity_t quantity, price_t price) const {
    // Note: price parameter reserved for future price-dependent impact models
    (void)price;  // Suppress unused parameter warning
    
    // Simple market impact model: impact = quantity * impact_factor
    // In reality, this would be much more sophisticated
    double impact_factor = 0.01; // 1 bps per 1000 shares
    return (quantity / 1000.0) * impact_factor;
}

size_t OrderManager::get_active_order_count() const {
    return active_orders_.size();
}

size_t OrderManager::get_pending_order_count() const {
    return pending_orders_.size();
}

bool OrderManager::is_healthy() const {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    std::lock_guard<std::mutex> position_lock(position_mutex_);
    
    return !is_emergency_shutdown_.load() && 
           execution_stats_.risk_violations < 10 &&
           std::abs(current_position_.realized_pnl) < risk_limits_.max_daily_loss;
}

void OrderManager::update_execution_stats(const OrderInfo& order_info) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // Update latency statistics
    if (order_info.completion_time != timestamp_t{} && order_info.submission_time != timestamp_t{}) {
        double fill_time_ms = to_microseconds(order_info.completion_time - order_info.submission_time) / 1000.0;
        
        if (execution_stats_.avg_fill_time_ms == 0.0) {
            execution_stats_.avg_fill_time_ms = fill_time_ms;
        } else {
            // Running average
            execution_stats_.avg_fill_time_ms = 
                (execution_stats_.avg_fill_time_ms * (execution_stats_.filled_orders - 1) + fill_time_ms) / 
                execution_stats_.filled_orders;
        }
    }
    
    // Update slippage statistics
    if (order_info.slippage != 0.0) {
        double slippage_bps = std::abs(order_info.slippage) / order_info.order.price * 10000.0;
        
        if (execution_stats_.avg_slippage_bps == 0.0) {
            execution_stats_.avg_slippage_bps = slippage_bps;
        } else {
            execution_stats_.avg_slippage_bps = 
                (execution_stats_.avg_slippage_bps * (execution_stats_.filled_orders - 1) + slippage_bps) / 
                execution_stats_.filled_orders;
        }
    }
    
    // Update market impact statistics
    if (order_info.market_impact_bps != 0.0) {
        if (execution_stats_.avg_market_impact_bps == 0.0) {
            execution_stats_.avg_market_impact_bps = order_info.market_impact_bps;
        } else {
            execution_stats_.avg_market_impact_bps = 
                (execution_stats_.avg_market_impact_bps * (execution_stats_.filled_orders - 1) + order_info.market_impact_bps) / 
                execution_stats_.filled_orders;
        }
    }
}

void OrderManager::track_latency(const OrderInfo& order_info) {
    // Track various latency metrics in the latency tracker
    if (order_info.submission_time != timestamp_t{} && order_info.creation_time != timestamp_t{}) {
        auto submission_latency = time_diff_us(order_info.creation_time, order_info.submission_time);
        latency_tracker_.add_latency(LatencyType::ORDER_PLACEMENT, submission_latency);
    }
    
    if (order_info.completion_time != timestamp_t{} && order_info.submission_time != timestamp_t{}) {
        auto fill_latency = time_diff_us(order_info.submission_time, order_info.completion_time);
        latency_tracker_.add_latency(LatencyType::TRADE_EXECUTION_PROCESSING, fill_latency);
    }
    
    if (order_info.completion_time != timestamp_t{} && order_info.creation_time != timestamp_t{}) {
        auto total_latency = time_diff_us(order_info.creation_time, order_info.completion_time);
        latency_tracker_.add_latency(LatencyType::ORDER_PLACEMENT, total_latency);
    }
}

void OrderManager::notify_order_event(const OrderInfo& order_info) {
    if (order_callback_) {
        order_callback_(order_info);
    }
}

void OrderManager::notify_fill_event(const OrderInfo& order_info, quantity_t fill_qty, price_t fill_price) {
    if (fill_callback_) {
        fill_callback_(order_info, fill_qty, fill_price, order_info.execution_state == ExecutionState::FILLED);
    }
}

void OrderManager::notify_risk_violation(RiskViolationType violation, const std::string& message) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    execution_stats_.risk_violations++;
    
    if (risk_callback_) {
        risk_callback_(violation, message);
    }
}

void OrderManager::print_debug_info() const {
    std::cout << "\n=== ORDER MANAGER DEBUG INFO ===" << std::endl;
    std::cout << "Orders in memory: " << orders_.size() << std::endl;
    std::cout << "Active orders: " << active_orders_.size() << std::endl;
    std::cout << "Pending orders: " << pending_orders_.size() << std::endl;
    std::cout << "Emergency shutdown: " << is_emergency_shutdown_.load() << std::endl;
    std::cout << "=================================" << std::endl;
}

bool OrderManager::force_cancel_order_during_shutdown(uint64_t order_id) {
    // Force cancel order during shutdown - bypasses engine to avoid circular calls and deadlocks
    
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) {
        std::cout << " DEBUG: Order not found for force cancellation - ID: " << order_id << std::endl;
        return false;
    }
    
    // Skip if already cancelled
    if (order_info->execution_state == ExecutionState::CANCELLED) {
        std::cout << " DEBUG: Order already cancelled - ID: " << order_id << std::endl;
        return true;
    }
    
    std::cout << " DEBUG: Force cancelling order ID: " << order_id << " during shutdown" << std::endl;
    
    // During shutdown, skip engine cancellation to avoid circular calls and deadlocks
    // The engine will be destroyed anyway, so we only need to clean up local state
    std::cout << " DEBUG: Bypassing engine cancel during shutdown for order " << order_id 
              << " - performing local cleanup only" << std::endl;
    
    // Force update local state regardless of engine result
    order_info->execution_state = ExecutionState::CANCELLED;
    order_info->order.status = OrderStatus::CANCELLED;
    order_info->order.last_update_time = now();
    order_info->completion_time = order_info->order.last_update_time;
    
    // Release order back to memory pool
    auto pooled_it = pooled_orders_.find(order_id);
    if (pooled_it != pooled_orders_.end()) {
        memory_manager_.order_pool().release_order(pooled_it->second);
        pooled_orders_.erase(pooled_it);
        std::cout << " DEBUG: Released order back to memory pool - ID: " << order_id << std::endl;
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.cancelled_orders++;
        
        if (execution_stats_.total_orders > 0) {
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
    }
    
    // Remove from tracking (force cleanup)
    pending_orders_.erase(order_id);
    active_orders_.erase(order_id);
    
    std::cout << " FORCE CANCELLED: " << (order_info->order.side == Side::BUY ? "BID" : "ASK") 
              << " Order ID: " << order_id << " during shutdown" << std::endl;
    
    return true;
}

} // namespace hft
