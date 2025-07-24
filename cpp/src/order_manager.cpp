#include "order_manager.hpp"
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
    
    // Get final statistics before cleanup
    auto final_stats = get_execution_stats();
    auto final_position = get_position();
    
    // Check for any remaining orders and handle them
    size_t active_count = active_orders_.size();
    size_t pending_count = pending_orders_.size();
    
    if (active_count > 0 || pending_count > 0) {
        std::cout << "⚠️  WARNING: Found remaining orders during shutdown:" << std::endl;
        std::cout << "  Active orders: " << active_count << std::endl;
        std::cout << "  Pending orders: " << pending_count << std::endl;
        
        // In a real system, you'd want to cancel these properly
        // For learning, we'll just warn about them
        std::cout << "  (In production, these would be emergency cancelled)" << std::endl;
    }
    
    // Print final execution statistics
    std::cout << "\n📊 FINAL SESSION STATISTICS:" << std::endl;
    std::cout << "  Orders Sent: " << final_stats.total_orders << std::endl;
    std::cout << "  Orders Filled: " << final_stats.filled_orders << std::endl;
    std::cout << "  Orders Cancelled: " << final_stats.cancelled_orders << std::endl;
    std::cout << "  Orders Rejected: " << final_stats.rejected_orders << std::endl;
    
    if (final_stats.total_orders > 0) {
        double fill_rate = static_cast<double>(final_stats.filled_orders) / final_stats.total_orders * 100.0;
        std::cout << "  Fill Rate: " << std::fixed << std::setprecision(1) << fill_rate << "%" << std::endl;
    }
    
    // Print final position information  
    std::cout << "\n💰 FINAL POSITION:" << std::endl;
    std::cout << "  Net Position: " << final_position.net_position << std::endl;
    std::cout << "  Realized P&L: $" << std::fixed << std::setprecision(2) << final_position.realized_pnl << std::endl;
    std::cout << "  Unrealized P&L: $" << final_position.unrealized_pnl << std::endl;
    std::cout << "  Daily Volume: " << final_position.daily_volume << std::endl;
    std::cout << "  Trade Count: " << final_position.trade_count << std::endl;
    
    // Calculate session duration
    auto session_duration = time_diff_us(session_start_time_, now());
    double session_seconds = to_microseconds(session_duration) / 1000000.0;
    std::cout << "\n⏱️  SESSION DURATION: " << std::fixed << std::setprecision(2) 
              << session_seconds << " seconds" << std::endl;
    
    // Risk warnings
    if (std::abs(final_position.net_position) > 0.01) {
        std::cout << "⚠️  WARNING: Ending session with non-zero position!" << std::endl;
    }
    
    if (final_position.realized_pnl < -risk_limits_.max_daily_loss * 0.8) {
        std::cout << "⚠️  WARNING: Significant daily losses detected!" << std::endl;
    }
    
    std::cout << "[ORDER MANAGER] ✅ Shutdown complete." << std::endl;
}

// =============================================================================
// CORE ORDER OPERATIONS (CRITICAL PATH - STUDY THESE PATTERNS)
// =============================================================================

uint64_t OrderManager::create_order(Side side, price_t price, quantity_t quantity,
                                    price_t current_mid_price) {
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_PLACEMENT);
    
    if (is_emergency_shutdown_.load()) return 0;
    
    RiskCheckResult risk_result = check_pre_trade_risk(side, quantity, price);
    
    if (risk_result != RiskCheckResult::APPROVED) {
        std::cout << "Order not approved: " << risk_check_result_to_string(risk_result) << std::endl;
        return 0;
    }
    
    uint64_t order_id = generate_order_id();
    
    // **CRITICAL FIX: Use memory pool for Order allocation**
    Order* pooled_order = memory_manager_.order_pool().acquire_order();
    if (!pooled_order) {
        std::cout << "❌ Memory pool exhausted - cannot create order!" << std::endl;
        return 0;
    }
    
    // Initialize the pooled order
    pooled_order->order_id = order_id;
    pooled_order->side = side;
    pooled_order->price = price;
    pooled_order->original_quantity = quantity;
    pooled_order->remaining_quantity = quantity;
    pooled_order->status = OrderStatus::PENDING;
    pooled_order->entry_time = now();
    pooled_order->last_update_time = pooled_order->entry_time;
    pooled_order->mid_price_at_entry = current_mid_price;
    
    // Create OrderInfo with pointer to pooled order
    OrderInfo order_info;
    order_info.order = *pooled_order;  // Copy for now - we'll optimize this further
    order_info.creation_time = pooled_order->entry_time;
    order_info.mid_price_at_creation = current_mid_price;
    order_info.execution_state = ExecutionState::PENDING_SUBMISSION;
    
    // Store in orders map
    orders_[order_id] = order_info; 
    pending_orders_.insert(order_id);
    
    // **CRITICAL: Track pooled order for proper cleanup**
    pooled_orders_[order_id] = pooled_order;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.total_orders++;
        
        // Update fill rate calculation (safe division)
        if (execution_stats_.total_orders > 0) {
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
    }

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
    
    timestamp_t modification_time = now();
    
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
    // TODO: Cancel specific order
    // LEARNING GOAL: How to cancel orders efficiently while maintaining state
    
    MEASURE_LATENCY(latency_tracker_, LatencyType::ORDER_CANCELLATION);
    
    if (is_emergency_shutdown_.load()) return false;
    
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;
    
    // Can only cancel orders that are pending or active
    if (order_info->execution_state == ExecutionState::FILLED ||
        order_info->execution_state == ExecutionState::CANCELLED ||
        order_info->execution_state == ExecutionState::REJECTED) {
        return false;
    }
    
    // Update order state
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
    
    return true;
}

// =============================================================================
// INTEGRATION WITH ORDER BOOK ENGINE
// =============================================================================

void OrderManager::set_orderbook_engine(OrderBookEngine* orderbook_engine) {
    orderbook_engine_ = orderbook_engine;
    std::cout << "[ORDER MANAGER] Connected to OrderBookEngine" << std::endl;
}

bool OrderManager::submit_order(uint64_t order_id) {
    // Emergency shutdown check
    if (is_emergency_shutdown_.load()) return false;

    // Fast order lookup
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;

    // State validation: Only submit if pending submission
    if (order_info->execution_state != ExecutionState::PENDING_SUBMISSION) {
        return false;
    }

    // Final risk check (optional, but good practice)
    RiskCheckResult risk_result = check_pre_trade_risk(order_info->order.side,
                                                       order_info->order.remaining_quantity,
                                                       order_info->order.price);
    if (risk_result != RiskCheckResult::APPROVED) {
        if (risk_callback_) {
            risk_callback_(RiskViolationType::ORDER_RATE_LIMIT, 
                "Order submission risk check failed: " + risk_check_result_to_string(risk_result));
        }
        return false;
    }

    // Order rate limiting
    if (!check_order_rate_limit()) {
        if (risk_callback_) {
            risk_callback_(RiskViolationType::ORDER_RATE_LIMIT, "Order rate limit exceeded");
        }
        return false;
    }

    // Update order state
    order_info->execution_state = ExecutionState::SUBMITTED;
    order_info->order.status = OrderStatus::ACTIVE;
    order_info->order.last_update_time = now();
    order_info->submission_time = order_info->order.last_update_time;

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

    // **CRITICAL INTEGRATION:** Submit order to OrderBookEngine for execution
    if (orderbook_engine_) {
        std::vector<TradeExecution> executions;
        MatchResult result = orderbook_engine_->submit_order_from_manager(order_info->order, executions);
        
        // Handle immediate execution results
        switch (result) {
            case MatchResult::FULL_FILL:
                // Order was fully executed immediately - fills will come via notify_fill callback
                break;
            case MatchResult::PARTIAL_FILL:
                // Order was partially executed, remainder is in book - fills will come via callback
                break;
            case MatchResult::NO_MATCH:
                // Order was placed in book, waiting for match
                break;
            case MatchResult::REJECTED:
                // Order was rejected by book engine
                order_info->execution_state = ExecutionState::REJECTED;
                order_info->order.status = OrderStatus::REJECTED;
                active_orders_.erase(order_id);
                return false;
        }
    } else {
        std::cout << "⚠️ WARNING: No OrderBookEngine connected - order submitted to memory only" << std::endl;
    }

    double latency_us = to_microseconds(order_info->submission_time - order_info->creation_time);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.avg_submission_latency_us =
            ((execution_stats_.avg_submission_latency_us * (execution_stats_.total_orders - 1)) + latency_us)
            / execution_stats_.total_orders;
    }

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
    // Handle order execution (partial or full)
    // LEARNING GOAL: This is where trades happen! Very important.
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;

    // Calculate volume-weighted average fill price for accumulating fills
    quantity_t previous_filled = order_info->filled_quantity;
    quantity_t total_filled = previous_filled + fill_qty;
    
    if (previous_filled == 0) {
        // First fill - use fill price directly
        order_info->average_fill_price = fill_price;
    } else {
        // Subsequent fill - calculate volume-weighted average
        order_info->average_fill_price = 
            ((order_info->average_fill_price * previous_filled) + (fill_price * fill_qty)) / total_filled;
    }
    
    // Accumulate filled quantity
    order_info->filled_quantity = total_filled;
    order_info->completion_time = fill_time;
    
    // Set execution state based on whether this is the final fill
    if (is_final_fill) {
        order_info->execution_state = ExecutionState::FILLED;
        // Remove filled order from active orders
        active_orders_.erase(order_id);
        
        // **CRITICAL FIX: Release order back to memory pool when completely filled**
        auto pooled_it = pooled_orders_.find(order_id);
        if (pooled_it != pooled_orders_.end()) {
            memory_manager_.order_pool().release_order(pooled_it->second);
            pooled_orders_.erase(pooled_it);
        }
    } else {
        order_info->execution_state = ExecutionState::PARTIALLY_FILLED;
    }
    
    // Update position
    update_position(fill_qty, fill_price, order_info->order.side);

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

    // Update performance statistics
    if (is_final_fill) {
        update_execution_stats(*order_info);
        
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            execution_stats_.filled_orders++;
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) / 
                                        execution_stats_.total_orders;
        }
    }

    // Notify fill event
    if (fill_callback_) {
        fill_callback_(*order_info, fill_qty, fill_price, is_final_fill);
    }

    return true;
}

bool OrderManager::handle_rejection(uint64_t order_id, const std::string& reason) {
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
    // TODO: Ultra-fast risk validation (< 100ns target)
    // LEARNING CHALLENGE: How do you make risk checks this fast?

    // Check position limits
    if (!check_position_limit(side, quantity)) return RiskCheckResult::POSITION_LIMIT_EXCEEDED;
    
    // Check daily loss limits
    if (!check_daily_loss_limit()) return RiskCheckResult::DAILY_LOSS_LIMIT_EXCEEDED;
    
    // Check order rate limits
    if (!check_order_rate_limit()) return RiskCheckResult::ORDER_RATE_LIMIT_EXCEEDED;

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
    
    // Convert trade to signed quantity (positive for buy, negative for sell)
    quantity_t trade_qty = (side == Side::BUY) ? quantity : -quantity;
    quantity_t new_position = old_position + trade_qty;
    
    // Calculate realized P&L when reducing or closing position
    if ((old_position > 0 && trade_qty < 0) || (old_position < 0 && trade_qty > 0)) {
        // Reducing or potentially flipping position
        quantity_t reduction = std::min(std::abs(trade_qty), std::abs(old_position));
        double pnl = 0.0;
        
        if (old_position > 0) {
            // Reducing long position: PnL = (sell_price - avg_cost) * reduction
            pnl = (price - old_avg_price) * reduction;
        } else {
            // Reducing short position: PnL = (avg_short_price - cover_price) * reduction
            pnl = (old_avg_price - price) * reduction;
        }
        
        current_position_.realized_pnl += pnl;
    }
    
    // Update net position
    current_position_.net_position = new_position;
    
    // Update average price based on position change type
    if (new_position == 0) {
        // Position closed - reset average price
        current_position_.avg_price = 0.0;
    } else if (old_position == 0) {
        // Opening new position - use trade price
        current_position_.avg_price = price;
    } else if ((old_position > 0 && new_position > 0 && new_position > old_position) ||
               (old_position < 0 && new_position < 0 && std::abs(new_position) > std::abs(old_position))) {
        // Increasing position in same direction - calculate volume-weighted average
        quantity_t total_quantity = std::abs(old_position) + quantity;
        current_position_.avg_price = 
            ((old_avg_price * std::abs(old_position)) + (price * quantity)) / total_quantity;
    } else if ((old_position > 0 && new_position < 0) || (old_position < 0 && new_position > 0)) {
        // Position flipped - set average price to trade price for the new position
        current_position_.avg_price = price;
    }
    // For position reduction (but not flip), keep the original average price
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
    std::cout << "📊 ORDER MANAGER PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    // Order Statistics Section
    std::cout << "\n📈 ORDER STATISTICS:" << std::endl;
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
    std::cout << "\n⚡ PERFORMANCE METRICS:" << std::endl;
    std::cout << "  Avg Submission Latency: " << std::fixed << std::setprecision(3) 
              << std::setw(8) << stats.avg_submission_latency_us << " μs" << std::endl;
    std::cout << "  Avg Fill Time:          " << std::fixed << std::setprecision(3) 
              << std::setw(8) << stats.avg_fill_time_ms << " ms" << std::endl;
    std::cout << "  Avg Cancel Time:        " << std::fixed << std::setprecision(3) 
              << std::setw(8) << stats.avg_cancel_time_ms << " ms" << std::endl;
    
    // Execution Quality Section
    std::cout << "\n💰 EXECUTION QUALITY:" << std::endl;
    std::cout << "  Avg Slippage:      " << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.avg_slippage_bps << " bps" << std::endl;
    std::cout << "  Avg Market Impact: " << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.avg_market_impact_bps << " bps" << std::endl;
    
    // Position Information Section
    std::cout << "\n📍 CURRENT POSITION:" << std::endl;
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
    std::cout << "\n⚠️  RISK METRICS:" << std::endl;
    std::cout << "  Risk Violations:   " << std::setw(10) << stats.risk_violations << std::endl;
    std::cout << "  Max Daily Loss:    $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.max_daily_loss << std::endl;
    std::cout << "  Current Drawdown:  $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.current_drawdown << std::endl;
    std::cout << "  Gross Exposure:    $" << std::fixed << std::setprecision(2) 
              << std::setw(8) << position.gross_exposure << std::endl;
    
    // Active Orders Section
    auto active_orders = get_active_orders();
    std::cout << "\n🔄 ACTIVE ORDERS:" << std::endl;
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
    // Fast position limit check
    // Calculate what position would be after this trade
    quantity_t new_position = current_position_.net_position;
    if (side == Side::BUY) {
        new_position += quantity;
    } else {
        new_position -= quantity;
    }
    
    // Check against limits
    return std::abs(new_position) <= risk_limits_.max_position;
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

void OrderManager::print_debug_info() const {
    std::cout << "\n=== ORDER MANAGER DEBUG INFO ===" << std::endl;
    std::cout << "Orders in memory: " << orders_.size() << std::endl;
    std::cout << "Active orders: " << active_orders_.size() << std::endl;
    std::cout << "Pending orders: " << pending_orders_.size() << std::endl;
    std::cout << "Emergency shutdown: " << is_emergency_shutdown_.load() << std::endl;
    std::cout << "=================================" << std::endl;
}

std::string risk_check_result_to_string(RiskCheckResult result) {
    switch (result) {
        case RiskCheckResult::APPROVED:
            return "APPROVED";
        case RiskCheckResult::POSITION_LIMIT_EXCEEDED:
            return "POSITION_LIMIT_EXCEEDED";
        case RiskCheckResult::DAILY_LOSS_LIMIT_EXCEEDED:
            return "DAILY_LOSS_LIMIT_EXCEEDED";
        case RiskCheckResult::DRAWDOWN_LIMIT_EXCEEDED:
            return "DRAWDOWN_LIMIT_EXCEEDED";
        case RiskCheckResult::CONCENTRATION_RISK:
            return "CONCENTRATION_RISK";
        case RiskCheckResult::VAR_LIMIT_EXCEEDED:
            return "VAR_LIMIT_EXCEEDED";
        case RiskCheckResult::ORDER_RATE_LIMIT_EXCEEDED:
            return "ORDER_RATE_LIMIT_EXCEEDED";
        case RiskCheckResult::LATENCY_LIMIT_EXCEEDED:
            return "LATENCY_LIMIT_EXCEEDED";
        case RiskCheckResult::CRITICAL_BREACH:
            return "CRITICAL_BREACH";
        default:
            return "UNKNOWN_RISK_RESULT";
    }
}

} // namespace hft
