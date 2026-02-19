#include "signal_engine.hpp"
#include "order_manager.hpp"
#include "orderbook_engine.hpp"
#include "log_control.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace hft {

// =============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =============================================================================

SignalEngine::SignalEngine(LatencyTracker& latency_tracker,
                          const MarketMakingConfig& config)
    : latency_tracker_(&latency_tracker)
    , config_(config)
    , orderbook_engine_(nullptr)
    , order_manager_(nullptr)
    , is_running_(false)
    , should_stop_(false)
    , is_destroying_(false)
    , session_start_time_(now())
    , next_signal_id_(1) {}

SignalEngine::~SignalEngine() {
    // Set destruction flag to prevent any further operations
    is_destroying_.store(true);
    
    // Stop the engine first
    should_stop_.store(true);
    is_running_.store(false);
    
    // Clear callbacks first to prevent them from being called after destruction
    signal_callback_ = nullptr;
    quote_update_callback_ = nullptr;
    risk_alert_callback_ = nullptr;
    
    // Clear external component references to prevent accessing destroyed objects
    orderbook_engine_ = nullptr;
    order_manager_ = nullptr;
    latency_tracker_ = nullptr;  // Clear latency tracker pointer
    
    // Clear shared data structures with proper locking
    {
        std::lock_guard<std::mutex> lock(signal_rate_mutex_);
        while (!recent_signals_.empty()) {
            recent_signals_.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        active_quotes_.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        // Reset statistics to prevent any lingering references
        statistics_ = MarketMakingStats();
    }
}

// =============================================================================
// CORE SIGNAL GENERATION (PRIMARY RESPONSIBILITY)
// =============================================================================

std::vector<TradingSignal> SignalEngine::generate_trading_signals() {
    // Check destruction flag first.
    if (is_destroying_.load()) {
        return {};
    }
    
    if (!is_running_.load() || should_stop_.load()) {
        return {};
    }
    
    std::vector<TradingSignal> signals;
    
    if (!orderbook_engine_) {
        return {};
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    if (top_of_book.bid_price <= 0.0 && top_of_book.ask_price <= 0.0) {
        return {};
    }
    
    price_t bid_price = 0.0;
    price_t ask_price = 0.0;
    quantity_t bid_size = 0.0;
    quantity_t ask_size = 0.0;
    
    calculate_optimal_quotes(bid_price, ask_price, bid_size, ask_size);
    
    // Generate quote signals
    generate_quote_signals(signals, bid_price, ask_price, bid_size, ask_size);
    
    // Apply rate limiting only when signals exist.
    if (!signals.empty()) {
        apply_rate_limiting(signals);
    }
    
    return signals;
}

void SignalEngine::calculate_optimal_quotes(price_t& bid_price, price_t& ask_price,
                                          quantity_t& bid_size, quantity_t& ask_size) {
    if (!orderbook_engine_) {
        return;
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    price_t mid_price = top_of_book.mid_price;
    
    // Use a fallback mid price if top-of-book mid is unavailable.
    if (mid_price <= 0.0 || mid_price < 0.001) {
        if (top_of_book.bid_price > 0.0) {
            mid_price = top_of_book.bid_price;
        } else if (top_of_book.ask_price > 0.0) {
            mid_price = top_of_book.ask_price;
        } else {
            mid_price = 118000.0;
        }
    }
    
    // Aggressive quoting: stay at/near touch depending on spread.
    double target_spread_bps = config_.target_spread_bps;
    double current_spread_bps = orderbook_engine_->get_spread_bps();
    
    if (current_spread_bps > 5.0) {
        target_spread_bps = std::max(1.0, current_spread_bps * 0.1);
    } else if (current_spread_bps > 2.0) {
        target_spread_bps = std::max(0.5, current_spread_bps * 0.2);
    } else {
        target_spread_bps = std::max(0.1, current_spread_bps * 0.5);
    }
    
    // Competitive pricing: quote inside or at the touch.
    if (top_of_book.bid_price > 0.0 && top_of_book.ask_price > 0.0) {
        double tick_size = 0.01;
        
        if (current_spread_bps > 5.0) {
            bid_price = top_of_book.bid_price + tick_size;
            ask_price = top_of_book.ask_price - tick_size;
        } else {
            bid_price = top_of_book.bid_price;
            ask_price = top_of_book.ask_price;
        }
    } else {
        bid_price = mid_price * (1.0 - target_spread_bps / 10000.0);
        ask_price = mid_price * (1.0 + target_spread_bps / 10000.0);
    }
    
    // Apply inventory skew
    apply_inventory_skew(bid_price, ask_price);
    
    // Safety check: keep bid below ask.
    if (bid_price >= ask_price) {
        double min_spread_bps = 1.0;
        bid_price = mid_price * (1.0 - min_spread_bps / 10000.0);
        ask_price = mid_price * (1.0 + min_spread_bps / 10000.0);
    }
    
    // Calculate sizes
    bid_size = calculate_position_adjusted_size(config_.default_quote_size, QuoteSide::BID);
    ask_size = calculate_position_adjusted_size(config_.default_quote_size, QuoteSide::ASK);
    
}

void SignalEngine::apply_inventory_skew(price_t& bid_price, price_t& ask_price) {
    if (!order_manager_) {
        return;
    }
    
    auto position = order_manager_->get_position();
    position_t current_position = position.net_position;
    
    if (std::abs(current_position) < config_.max_position * 0.1) {
        return; // No significant position to skew
    }
    
    // Calculate skew based on position
    double position_ratio = current_position / config_.max_position;
    double skew_bps = position_ratio * config_.max_inventory_skew_bps;
    
    // Apply skew: if long position, make it harder to buy (higher bid, higher ask)
    // if short position, make it harder to sell (lower bid, lower ask)
    if (current_position > 0) {
        // Long position: increase both prices to discourage buying
        bid_price *= (1.0 + skew_bps / 10000.0);
        ask_price *= (1.0 + skew_bps / 10000.0);
    } else {
        // Short position: decrease both prices to discourage selling
        bid_price *= (1.0 - std::abs(skew_bps) / 10000.0);
        ask_price *= (1.0 - std::abs(skew_bps) / 10000.0);
    }
}

bool SignalEngine::should_place_quote(QuoteSide side, price_t price, quantity_t size) {
    if (!order_manager_) {
        return false;
    }
    
    // Validating quote
    
    // Basic validation
    if (price <= 0.0 || size <= 0.0) {
        return false;
    }
    
    auto position = order_manager_->get_position();
    position_t current_position = position.net_position;

    if (side == QuoteSide::BID && current_position >= config_.max_position) {
        return false;
    }
    
    if (side == QuoteSide::ASK && current_position <= -config_.max_position) {
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(signal_rate_mutex_);
        auto now_time = now();
        while (!recent_signals_.empty() && 
               to_microseconds(time_diff_us(recent_signals_.front(), now_time)) > 1000000) {
            recent_signals_.pop();
        }

        if (recent_signals_.size() >= config_.max_orders_per_second * 3) {
            return false;
        }
    }
    
    return true;
}

bool SignalEngine::should_replace_quote(QuoteSide side, price_t current_price, price_t new_price) {
    if (!orderbook_engine_) {
        return false;
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    price_t mid_price = top_of_book.mid_price;
    
    if (mid_price <= 0.0) {
        return false;
    }
    
    // AGGRESSIVE: Reduce price improvement threshold for more frequent requoting
    double improvement_threshold = config_.enable_aggressive_quotes ? 
        mid_price * 0.00005 : mid_price * 0.0001;  // 0.5 bps for aggressive mode vs 1 bps for normal
    
    if (side == QuoteSide::BID) {
        return new_price > current_price + improvement_threshold;
    } else {
        return new_price < current_price - improvement_threshold;
    }
}

// =============================================================================
// SIGNAL GENERATION HELPERS
// =============================================================================

void SignalEngine::generate_quote_signals(std::vector<TradingSignal>& signals,
                                        price_t bid_price, price_t ask_price,
                                        quantity_t bid_size, quantity_t ask_size) {
    if (bid_price <= 0.0 || ask_price <= 0.0 || bid_size <= 0.0 || ask_size <= 0.0) {
        return;
    }
    
    bool should_replace_quotes = false;
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        
        if (!active_quotes_.empty()) {
            
            // Check each quote's age - replace if any quote is older than configured refresh interval
            auto now_time = now();
            for (const auto& [order_id, quote] : active_quotes_) {
                auto quote_age_us = std::chrono::duration_cast<std::chrono::microseconds>(now_time - quote.creation_time).count();
                auto quote_age_ms = quote_age_us / 1000;
                
                if (quote_age_ms > static_cast<int64_t>(config_.quote_refresh_ms)) {
                    should_replace_quotes = true;
                    break;
                }
            }
            
            // Replace if market midpoint has moved materially.
            static price_t last_market_mid_price = 0.0;
            auto top_of_book = orderbook_engine_->get_top_of_book();
            price_t current_market_mid = top_of_book.mid_price;
            
            if (last_market_mid_price > 0.0 && current_market_mid > 0.0) {
                double price_change_bps = std::abs(current_market_mid - last_market_mid_price) / last_market_mid_price * 10000.0;
                if (price_change_bps > 0.5) {
                    should_replace_quotes = true;
                }
            }
            if (current_market_mid > 0.0) {
                last_market_mid_price = current_market_mid;
            }
            
            // Check if our quotes are no longer competitive (not at best bid/offer)
            if (top_of_book.bid_price > 0.0 && top_of_book.ask_price > 0.0) {
                for (const auto& [order_id, quote] : active_quotes_) {
                    bool is_competitive = false;
                    if (quote.side == QuoteSide::BID) {
                        is_competitive = (quote.price >= top_of_book.bid_price - 0.01);
                    } else {
                        is_competitive = (quote.price <= top_of_book.ask_price + 0.01);
                    }
                    
                    if (!is_competitive) {
                        should_replace_quotes = true;
                        break;
                    }
                }
            }
            
        } else {
            should_replace_quotes = true;
        }
    }
    
    if (should_replace_quotes) {
        bool has_existing_quotes = false;
        {
            std::lock_guard<std::mutex> lock(quotes_mutex_);
            has_existing_quotes = !active_quotes_.empty();
        }
        
        bool should_place_bid = should_place_quote(QuoteSide::BID, bid_price, bid_size);
        bool should_place_ask = should_place_quote(QuoteSide::ASK, ask_price, ask_size);
        
        if (has_existing_quotes) {
            if (should_place_bid) {
                generate_targeted_cancellation_signals(signals, QuoteSide::BID);
            }
            if (should_place_ask) {
                generate_targeted_cancellation_signals(signals, QuoteSide::ASK);
            }
        }
        
        if (should_place_bid) {
            TradingSignal signal;
            signal.type = SignalType::PLACE_BID;
            signal.side = Side::BUY;
            signal.price = bid_price;
            signal.quantity = bid_size;
            signal.timestamp = now();
            signal.reason = "Market making bid";
            signal.order_id = next_signal_id_++;
            signals.push_back(signal);
        }
        
        if (should_place_ask) {
            TradingSignal signal;
            signal.type = SignalType::PLACE_ASK;
            signal.side = Side::SELL;
            signal.price = ask_price;
            signal.quantity = ask_size;
            signal.timestamp = now();
            signal.reason = "Market making ask";
            signal.order_id = next_signal_id_++;
            signals.push_back(signal);
        }
    }
}

void SignalEngine::generate_cancellation_signals(std::vector<TradingSignal>& signals) {
    if (!orderbook_engine_) {
        return;
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    price_t mid_price = top_of_book.mid_price;
    
    if (mid_price <= 0.0) {
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (const auto& [order_id, quote] : active_quotes_) {
            TradingSignal signal;
            signal.type = (quote.side == QuoteSide::BID) ? SignalType::CANCEL_BID : SignalType::CANCEL_ASK;
            signal.order_id = order_id;
            signal.timestamp = now();
            signal.reason = "Replacing quote with new market making quote";
            signals.push_back(signal);
        }
        
        // `active_quotes_` is updated by order callbacks.
    }
}

void SignalEngine::generate_targeted_cancellation_signals(std::vector<TradingSignal>& signals, QuoteSide side) {
    if (!orderbook_engine_) {
        return;
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    price_t mid_price = top_of_book.mid_price;
    
    if (mid_price <= 0.0) {
        return;
    }
    
    // Only cancel quotes on the specified side
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (const auto& [order_id, quote] : active_quotes_) {
            if (quote.side == side) {
                TradingSignal signal;
                signal.type = (quote.side == QuoteSide::BID) ? SignalType::CANCEL_BID : SignalType::CANCEL_ASK;
                signal.order_id = order_id;
                signal.timestamp = now();
                signal.reason = "Replacing " + std::string(side == QuoteSide::BID ? "bid" : "ask") + " quote with new market making quote";
                signals.push_back(signal);
            }
        }
    }
}

void SignalEngine::apply_rate_limiting(std::vector<TradingSignal>& signals) {
    if (signals.empty()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(signal_rate_mutex_);
    
    // Remove old signals from tracking
    auto now_time = now();
    while (!recent_signals_.empty() && 
           to_microseconds(time_diff_us(recent_signals_.front(), now_time)) > 1000000) {
        recent_signals_.pop();
    }
    
    if (recent_signals_.size() > config_.max_orders_per_second * 2) {
        while (!recent_signals_.empty()) {
            recent_signals_.pop();
        }
    }
    
    // Limit signals based on rate
    size_t max_signals = config_.max_orders_per_second - recent_signals_.size();
    if (signals.size() > max_signals) {
        signals.resize(max_signals);
    }
    
    for (size_t i = 0; i < signals.size(); ++i) {
        recent_signals_.push(now_time);
    }
}

// =============================================================================
// MARKET ANALYSIS (SIGNAL ENGINE SPECIFIC)
// =============================================================================

DepthMetrics SignalEngine::analyze_market_depth(const MarketDepth& depth) {
    DepthMetrics metrics;
    
    if (depth.bids.empty() || depth.asks.empty()) {
        return metrics;
    }
    
    price_t mid_price = (depth.bids[0].price + depth.asks[0].price) / 2.0;
    if (mid_price <= 0.0) {
        return metrics;
    }
    
    // Calculate liquidity
    metrics.bid_liquidity_bps = calculate_liquidity_bps(depth.bids, mid_price, Side::BUY);
    metrics.ask_liquidity_bps = calculate_liquidity_bps(depth.asks, mid_price, Side::SELL);
    
    // Calculate imbalance
    metrics.bid_ask_imbalance = metrics.bid_liquidity_bps / metrics.ask_liquidity_bps;
    
    // Calculate market pressure
    metrics.market_pressure = calculate_market_pressure(depth);
    
    // Calculate spread impact
    metrics.spread_impact = calculate_spread_impact(depth, mid_price);
    
    // Detect significant changes
    metrics.significant_change = detect_significant_depth_change(depth);
    
    return metrics;
}

double SignalEngine::calculate_liquidity_bps(const std::vector<PriceLevel>& levels, 
                                          price_t mid_price, Side side) {
    (void)side; // Suppress unused parameter warning
    if (levels.empty() || mid_price <= 0.0) {
        return 0.0;
    }
    
    double total_liquidity = 0.0;
    double total_value = 0.0;
    
    for (const auto& level : levels) {
        double price_diff = std::abs(level.price - mid_price);
        double liquidity_bps = (price_diff / mid_price) * 10000.0;
        
        total_liquidity += level.quantity * liquidity_bps;
        total_value += level.quantity * level.price;
    }
    
    return total_value > 0.0 ? total_liquidity / total_value : 0.0;
}

double SignalEngine::calculate_market_pressure(const MarketDepth& depth) {
    if (depth.bids.empty() || depth.asks.empty()) {
        return 0.0;
    }
    
    // Calculate volume-weighted average prices
    double bid_vwap = 0.0, ask_vwap = 0.0;
    double bid_volume = 0.0, ask_volume = 0.0;
    
    for (const auto& level : depth.bids) {
        bid_vwap += level.price * level.quantity;
        bid_volume += level.quantity;
    }
    
    for (const auto& level : depth.asks) {
        ask_vwap += level.price * level.quantity;
        ask_volume += level.quantity;
    }
    
    if (bid_volume <= 0.0 || ask_volume <= 0.0) {
        return 0.0;
    }
    
    bid_vwap /= bid_volume;
    ask_vwap /= ask_volume;
    
    price_t mid_price = (depth.bids[0].price + depth.asks[0].price) / 2.0;
    if (mid_price <= 0.0) {
        return 0.0;
    }
    
    // Market pressure: positive = bullish, negative = bearish
    double pressure = ((bid_vwap - mid_price) - (ask_vwap - mid_price)) / mid_price;
    return std::clamp(pressure, -1.0, 1.0);
}

double SignalEngine::calculate_spread_impact(const MarketDepth& depth, price_t mid_price) {
    if (depth.bids.empty() || depth.asks.empty() || mid_price <= 0.0) {
        return 0.0;
    }
    
    price_t best_bid = depth.bids[0].price;
    price_t best_ask = depth.asks[0].price;
    
    double spread_bps = ((best_ask - best_bid) / mid_price) * 10000.0;
    return spread_bps;
}

bool SignalEngine::detect_significant_depth_change(const MarketDepth& depth) {
    // Placeholder heuristic: any populated book is treated as significant.
    return !depth.bids.empty() && !depth.asks.empty();
}

// =============================================================================
// QUOTE MANAGEMENT METHODS
// =============================================================================

void SignalEngine::track_order_placement(uint64_t order_id, QuoteSide side, price_t price, quantity_t quantity) {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    
    MarketMakingQuote quote;
    quote.side = side;
    quote.price = price;
    quote.quantity = quantity;
    quote.state = QuoteState::ACTIVE;
    quote.order_id = order_id;
    quote.creation_time = now();
    quote.last_update_time = now();
    quote.filled_quantity = 0.0;
    
    active_quotes_[order_id] = quote;
}

void SignalEngine::track_order_cancellation(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    
    auto it = active_quotes_.find(order_id);
    if (it != active_quotes_.end()) {
        active_quotes_.erase(it);
    }
}

void SignalEngine::track_order_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price) {
    (void)fill_price;
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    
    auto it = active_quotes_.find(order_id);
    if (it != active_quotes_.end()) {
        it->second.filled_quantity += fill_qty;
        it->second.last_update_time = now();
        
        // If order is completely filled, remove it from active quotes
        if (it->second.filled_quantity >= it->second.quantity) {
            active_quotes_.erase(it);
        }
    }
}

void SignalEngine::clear_stale_quotes() {
    std::vector<uint64_t> stale_order_ids;

    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);

        auto now_time = now();
        const int64_t stale_threshold_us = std::max<int64_t>(
            30000000,
            static_cast<int64_t>(config_.quote_refresh_ms) * 1000 * 10);
        for (const auto& [order_id, quote] : active_quotes_) {
            // Identify quotes older than the forced cleanup threshold.
            auto age = time_diff_us(quote.creation_time, now_time);
            if (to_microseconds(age) > stale_threshold_us) {
                stale_order_ids.push_back(order_id);
            }
        }
    }

    for (uint64_t order_id : stale_order_ids) {
        bool cancelled = false;
        if (order_manager_) {
            cancelled = order_manager_->cancel_order(order_id);
        }

        if (cancelled || !order_manager_ || order_manager_->get_order_info(order_id) == nullptr) {
            track_order_cancellation(order_id);
        }
    }
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

inline quantity_t SignalEngine::calculate_position_adjusted_size(quantity_t base_size, QuoteSide side) const {
    if (!order_manager_) {
        return base_size;
    }
    
    auto position = order_manager_->get_position();
    position_t current_position = position.net_position;
    
    // Adjust size based on position
    double adjustment = 1.0;
    if (side == QuoteSide::BID && current_position > 0) {
        // Long position: reduce bid size
        adjustment = 1.0 - (current_position / config_.max_position) * 0.5;
    } else if (side == QuoteSide::BID && current_position < 0) {
        // Short position: increase bid size
        adjustment = 1.0 + (std::abs(current_position) / config_.max_position) * 0.5;
    } else if (side == QuoteSide::ASK && current_position > 0) {
        // Long position: increase ask size
        adjustment = 1.0 + (current_position / config_.max_position) * 0.5;
    } else if (side == QuoteSide::ASK && current_position < 0) {
        // Short position: reduce ask size
        adjustment = 1.0 - (std::abs(current_position) / config_.max_position) * 0.5;
    }
    
    return base_size * adjustment;
}

bool SignalEngine::should_cancel_quote(const MarketMakingQuote& quote, price_t mid_price) const {
    if (mid_price <= 0.0) {
        return false;
    }
    
    // Cancel if quote is too far from mid price
    double price_diff = std::abs(quote.price - mid_price);
    double threshold = mid_price * 0.01; // 100 bps threshold (1%)
    
    return price_diff > threshold;
}

void SignalEngine::update_statistics(const TradingSignal& signal) {
    if (is_destroying_.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    switch (signal.type) {
        case SignalType::PLACE_BID:
        case SignalType::PLACE_ASK:
            statistics_.total_quotes_placed++;
            break;
        case SignalType::CANCEL_BID:
        case SignalType::CANCEL_ASK:
            statistics_.total_quotes_cancelled++;
            break;
        default:
            break;
    }
}

void SignalEngine::notify_signal_generated(const TradingSignal& signal) {
    if (!is_destroying_.load() && signal_callback_) {
        signal_callback_(signal);
    }
}

void SignalEngine::notify_risk_alert(const std::string& alert, double value) {
    if (!is_destroying_.load() && risk_alert_callback_) {
        risk_alert_callback_(alert, value);
    }
}

// =============================================================================
// CONFIGURATION AND CONTROL
// =============================================================================

bool SignalEngine::start() {
    if (is_running_.load()) {
        return false;
    }
    
    is_running_.store(true);
    should_stop_.store(false);
    session_start_time_ = now();
    
    std::cout << "[SIGNAL ENGINE] Started successfully." << std::endl;
    return true;
}

void SignalEngine::stop() {
    should_stop_.store(true);
    is_running_.store(false);
    std::cout << "[SIGNAL ENGINE] Stopped." << std::endl;
}

void SignalEngine::update_config(const MarketMakingConfig& config) {
    config_ = config;
    std::cout << "[SIGNAL ENGINE] Configuration updated." << std::endl;
}

void SignalEngine::process_market_data_update(const TopOfBook& top_of_book) {
    ScopedCoutSilencer silence_hot_path(!kEnableHotPathLogging);
    
    if (!is_running_.load() || should_stop_.load() || is_destroying_.load()) {
        return;
    }
    
    // Update current market state
    current_top_of_book_ = top_of_book;

    // Generate trading signals based on market data
    auto signals = generate_trading_signals();
    
    for (const auto& signal : signals) {
        update_statistics(signal);
        notify_signal_generated(signal);
    }
}

// =============================================================================
// SETTERS FOR EXTERNAL COMPONENTS
// =============================================================================

void SignalEngine::set_orderbook_engine(OrderBookEngine* orderbook_engine) {
    orderbook_engine_ = orderbook_engine;
}

void SignalEngine::set_order_manager(OrderManager* order_manager) {
    order_manager_ = order_manager;
}

void SignalEngine::set_signal_callback(SignalCallback callback) {
    signal_callback_ = callback;
}

void SignalEngine::set_quote_update_callback(QuoteUpdateCallback callback) {
    quote_update_callback_ = callback;
}

void SignalEngine::set_risk_alert_callback(RiskAlertCallback callback) {
    risk_alert_callback_ = callback;
}

void SignalEngine::clear_all_callbacks() {
    signal_callback_ = nullptr;
    quote_update_callback_ = nullptr;
    risk_alert_callback_ = nullptr;
}

// =============================================================================
// STATISTICS AND REPORTING
// =============================================================================

MarketMakingStats SignalEngine::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return statistics_;
}

std::vector<MarketMakingQuote> SignalEngine::get_active_quotes() const {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    std::vector<MarketMakingQuote> quotes;
    quotes.reserve(active_quotes_.size());
    
    for (const auto& [order_id, quote] : active_quotes_) {
        quotes.push_back(quote);
    }
    
    return quotes;
}

void SignalEngine::print_performance_report() const {
    std::cout << "\n Signal Engine Performance Report" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    auto session_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now() - session_start_time_).count();
    std::cout << "Session Duration: " << session_duration << " seconds" << std::endl;
    std::cout << "Running: " << (is_running_.load() ? "Yes" : "No") << std::endl;
    
    // Quote statistics
    auto stats = get_statistics();
    std::cout << "Quotes Placed: " << stats.total_quotes_placed << std::endl;
    std::cout << "Quotes Filled: " << stats.total_quotes_filled << std::endl;
    std::cout << "Quotes Cancelled: " << stats.total_quotes_cancelled << std::endl;
    std::cout << "Fill Rate: " << (stats.fill_rate * 100.0) << "%" << std::endl;
    std::cout << "Avg Spread Captured: " << stats.avg_spread_captured_bps << " bps" << std::endl;
    
    // Performance metrics
    auto latency_stats = get_signal_generation_latency();
    std::cout << "Signal Generation Latency (us):" << std::endl;
    std::cout << "  Mean: " << latency_stats.mean_us << std::endl;
    std::cout << "  P95: " << latency_stats.p95_us << std::endl;
    std::cout << "  P99: " << latency_stats.p99_us << std::endl;
    
    std::cout << "=====================================\n" << std::endl;
}

LatencyStatistics SignalEngine::get_signal_generation_latency() const {
    if (!latency_tracker_) {
        return LatencyStatistics{};  // Return empty statistics if tracker is null
    }
    return latency_tracker_->get_statistics(LatencyType::TICK_TO_TRADE);
}

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

MarketMakingConfig create_default_market_making_config() {
    return MarketMakingConfig();
}

MarketMakingConfig create_aggressive_market_making_config() {
    MarketMakingConfig config;
    config.default_quote_size = 20.0;
    config.target_spread_bps = 10.0;
    config.max_orders_per_second = 200;
    config.enable_aggressive_quotes = true;
    return config;
}

MarketMakingConfig create_conservative_market_making_config() {
    MarketMakingConfig config;
    config.default_quote_size = 5.0;
    config.target_spread_bps = 25.0;
    config.max_orders_per_second = 50;
    config.enable_aggressive_quotes = false;
    return config;
}

bool validate_trading_signal(const TradingSignal& signal) {
    if (signal.price <= 0.0 || signal.quantity <= 0.0) {
        return false;
    }
    
    switch (signal.type) {
        case SignalType::PLACE_BID:
        case SignalType::PLACE_ASK:
        case SignalType::CANCEL_BID:
        case SignalType::CANCEL_ASK:
        case SignalType::MODIFY_BID:
        case SignalType::MODIFY_ASK:
        case SignalType::HOLD:
        case SignalType::EMERGENCY_CANCEL:
            return true;
        default:
            return false;
    }
}

std::string signal_type_to_string(SignalType type) {
    switch (type) {
        case SignalType::PLACE_BID: return "PLACE_BID";
        case SignalType::PLACE_ASK: return "PLACE_ASK";
        case SignalType::CANCEL_BID: return "CANCEL_BID";
        case SignalType::CANCEL_ASK: return "CANCEL_ASK";
        case SignalType::MODIFY_BID: return "MODIFY_BID";
        case SignalType::MODIFY_ASK: return "MODIFY_ASK";
        case SignalType::HOLD: return "HOLD";
        case SignalType::EMERGENCY_CANCEL: return "EMERGENCY_CANCEL";
        default: return "UNKNOWN";
    }
}

std::string quote_side_to_string(QuoteSide side) {
    switch (side) {
        case QuoteSide::BID: return "BID";
        case QuoteSide::ASK: return "ASK";
        case QuoteSide::BOTH: return "BOTH";
        default: return "UNKNOWN";
    }
}

} // namespace hft
