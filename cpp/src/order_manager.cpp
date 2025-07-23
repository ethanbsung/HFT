#include "order_manager.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace hft {

// =============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =============================================================================

OrderManager::OrderManager(MemoryManager& memory_manager,
                          LatencyTracker& latency_tracker,
                          const RiskLimits& risk_limits)
    : memory_manager_(memory_manager)
    , latency_tracker_(latency_tracker)
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
    
    OrderInfo order_info;
    order_info.order.order_id = order_id;
    order_info.order.side = side;
    order_info.order.price = price;
    order_info.order.original_quantity = quantity;
    order_info.order.remaining_quantity = quantity;
    order_info.order.status = OrderStatus::PENDING;
    order_info.order.entry_time = now();
    order_info.order.last_update_time = order_info.order.entry_time;
    order_info.order.mid_price_at_entry = current_mid_price;
    
    order_info.creation_time = order_info.order.entry_time;
    order_info.mid_price_at_creation = current_mid_price;
    order_info.execution_state = ExecutionState::PENDING_SUBMISSION;
    
    orders_[order_id] = order_info; pending_orders_.insert(order_id);

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
        order_callback_(*order_info);
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
    // Emergency shutdown check
    if (is_emergency_shutdown_.load()) return false;

    // Fast order lookup
    OrderInfo* order_info = find_order(order_id);
    if (!order_info) return false;

    // State validation: Only cancel if not already filled/cancelled/rejected/expired
    if (order_info->execution_state == ExecutionState::FILLED ||
        order_info->execution_state == ExecutionState::CANCELLED ||
        order_info->execution_state == ExecutionState::REJECTED ||
        order_info->execution_state == ExecutionState::EXPIRED) {
        return false;
    }

    // Update order state
    order_info->execution_state = ExecutionState::CANCELLED;
    order_info->order.status = OrderStatus::CANCELLED;
    order_info->order.last_update_time = now();
    order_info->completion_time = order_info->order.last_update_time;

    // Remove from active/pending sets
    active_orders_.erase(order_id);
    pending_orders_.erase(order_id);

    // Update statistics (thread-safe)
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        execution_stats_.cancelled_orders++;
        // Update avg_cancel_time_ms, etc.
        if (execution_stats_.total_orders > 0) {
            execution_stats_.fill_rate = static_cast<double>(execution_stats_.filled_orders) /
                                        execution_stats_.total_orders;
        }
    }

    if (order_callback_) {
        order_callback_(*order_info);
    }

    return true;
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

    // Move order from pending to active
    pending_orders_.erase(order_id);
    active_orders_.insert(order_id);

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
// ORDER LIFECYCLE HANDLERS (STUDY THESE FOR MARKET INTEGRATION)
// =============================================================================

bool OrderManager::handle_order_ack(uint64_t order_id, timestamp_t ack_time) {
    // TODO: Handle exchange acknowledgment
    // LEARNING GOAL: What happens when exchange confirms order?
    
    // Pattern: External event → Update internal state → Notify callbacks
    
    return false; // TODO: Implement
}

bool OrderManager::handle_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price,
                              timestamp_t fill_time, bool is_final_fill) {
    // TODO: Handle order execution (partial or full)
    // LEARNING GOAL: This is where trades happen! Very important.
    
    // Key responsibilities:
    // 1. Update order state (filled_quantity, average_fill_price)
    // 2. Update position (net_position, realized_pnl)
    // 3. Calculate execution quality metrics (slippage, market_impact)
    // 4. Notify callbacks
    // 5. Update performance statistics
    
    return false; // TODO: Implement
}

bool OrderManager::handle_rejection(uint64_t order_id, const std::string& reason) {
    // TODO: Handle order rejection
    // LEARNING GOAL: What do you do when exchange says "no"?
    
    return false; // TODO: Implement
}

bool OrderManager::handle_cancel_confirmation(uint64_t order_id) {
    // TODO: Handle cancellation confirmation
    // LEARNING GOAL: Difference between requesting cancel vs confirmed cancel
    
    return false; // TODO: Implement
}

// =============================================================================
// RISK MANAGEMENT (CRITICAL FOR PREVENTING LOSSES)
// =============================================================================

RiskCheckResult OrderManager::check_pre_trade_risk(Side side, quantity_t quantity, price_t price) const {
    // TODO: Ultra-fast risk validation (< 100ns target)
    // LEARNING CHALLENGE: How do you make risk checks this fast?
    
    // Fast checks (in order of speed):
    // 1. Position limits (simple arithmetic)
    // 2. Daily loss limits (cached value)
    // 3. Order rate limits (queue size check)
    
    // TODO: Implement each check
    // if (!check_position_limit(side, quantity)) return RiskCheckResult::POSITION_LIMIT_EXCEEDED;
    // if (!check_daily_loss_limit()) return RiskCheckResult::DAILY_LOSS_LIMIT_EXCEEDED;
    // if (!check_order_rate_limit()) return RiskCheckResult::ORDER_RATE_LIMIT_EXCEEDED;
    
    return RiskCheckResult::APPROVED;
}

void OrderManager::update_risk_limits(const RiskLimits& new_limits) {
    // TODO: Thread-safe update of risk limits
    // LEARNING GOAL: How to update shared data safely
    
    // HINT: Use mutex to protect risk_limits_
    std::lock_guard<std::mutex> lock(position_mutex_);
    risk_limits_ = new_limits;
    
    std::cout << "[RISK] Updated risk limits" << std::endl;
}

void OrderManager::emergency_shutdown(const std::string& reason) {
    // TODO: Emergency shutdown implementation
    // LEARNING GOAL: How to safely shut down a trading system
    
    std::cout << "[EMERGENCY] Shutting down: " << reason << std::endl;
    
    // TODO: Set emergency flag (atomic)
    // TODO: Cancel all active orders
    // TODO: Notify risk callback
    // TODO: Log emergency event
}

// =============================================================================
// POSITION AND P&L TRACKING
// =============================================================================

PositionInfo OrderManager::get_position() const {
    // TODO: Thread-safe position read
    // LEARNING GOAL: How to provide fast, consistent reads
    
    std::lock_guard<std::mutex> lock(position_mutex_);
    return current_position_;
}

void OrderManager::update_position(quantity_t quantity, price_t price, Side side) {
    // TODO: Update position from trade
    // LEARNING GOAL: How to calculate volume-weighted average prices
    
    // This is complex! You need to:
    // 1. Update net position (+/- based on side)
    // 2. Recalculate average price (volume-weighted)
    // 3. Calculate realized P&L if reducing position
    // 4. Update risk metrics
    
    std::lock_guard<std::mutex> lock(position_mutex_);
    // TODO: Implement position math
}

double OrderManager::calculate_unrealized_pnl(price_t current_mid_price) const {
    // TODO: Calculate mark-to-market P&L
    // LEARNING GOAL: How to calculate unrealized profits/losses
    
    // Formula: position_size * (current_price - average_price)
    // Positive = profit, Negative = loss
    
    std::lock_guard<std::mutex> lock(position_mutex_);
    return current_position_.net_position * (current_mid_price - current_position_.avg_price);
}

// =============================================================================
// PERFORMANCE MONITORING
// =============================================================================

ExecutionStats OrderManager::get_execution_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return execution_stats_;
}

void OrderManager::print_performance_report() const {
    // TODO: Print comprehensive performance report
    // LEARNING GOAL: What metrics matter for trading performance?
    
    auto stats = get_execution_stats();
    auto position = get_position();
    
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "📊 ORDER MANAGER PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    
    // TODO: Print detailed statistics
    // Hint: Look at latency_tracker.cpp for formatting examples
}

// =============================================================================
// PRIVATE HELPER FUNCTIONS (STUDY THESE FOR IMPLEMENTATION PATTERNS)
// =============================================================================

uint64_t OrderManager::generate_order_id() noexcept {
    // TODO: Lock-free order ID generation
    // LEARNING GOAL: Why use atomic operations?
    
    return next_order_id_.fetch_add(1);
}

OrderInfo* OrderManager::find_order(uint64_t order_id) noexcept {
    // TODO: Fast order lookup
    // LEARNING GOAL: Hash table performance patterns
    
    auto it = orders_.find(order_id);
    return (it != orders_.end()) ? &it->second : nullptr;
}

const OrderInfo* OrderManager::find_order(uint64_t order_id) const noexcept {
    // TODO: Const version of find_order
    auto it = orders_.find(order_id);
    return (it != orders_.end()) ? &it->second : nullptr;
}

bool OrderManager::check_position_limit(Side side, quantity_t quantity) const noexcept {
    // TODO: Fast position limit check
    // LEARNING GOAL: How to do arithmetic risk checks quickly
    
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
    // TODO: Check if daily losses exceed limit
    return current_position_.realized_pnl > -risk_limits_.max_daily_loss;
}

bool OrderManager::check_order_rate_limit() const noexcept {
    // TODO: Check order rate limiting
    // LEARNING GOAL: How to implement sliding window rate limiting
    
    // This is tricky! You need to count orders in last second
    // Hint: Use recent_orders_ queue and current timestamp
    
    return true; // TODO: Implement actual check
}

// =============================================================================
// LEARNING EXERCISES FOR YOU TO COMPLETE
// =============================================================================

/*
NEXT STEPS FOR YOUR LEARNING:

1. COMPLETE THE TODOS:
   - Start with the simple ones (constructor initialization)
   - Move to the fast-path functions (create_order, cancel_order)
   - Finish with complex ones (position management, statistics)

2. STUDY QUESTIONS:
   - Why are some functions marked 'noexcept'?
   - Why use atomic variables vs mutexes?
   - How does hash table lookup achieve O(1) performance?
   - What makes pre-trade risk checks fast vs slow?

3. PERFORMANCE CHALLENGES:
   - Can you make create_order() under 1 microsecond?
   - Can you make cancel_order() under 300 nanoseconds?
   - How would you optimize the position calculation?

4. REAL-WORLD SCENARIOS:
   - What happens during a market crash?
   - How do you handle network outages?
   - What if memory allocation fails?

5. INTEGRATION EXERCISES:
   - How does this connect to your MemoryPool?
   - How does this use your LatencyTracker?
   - What metrics would you add?

Focus on understanding WHY each design decision was made.
Speed in HFT isn't just about fast code - it's about smart architecture!
*/

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

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
