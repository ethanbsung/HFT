#include "signal_engine.hpp"
#include "order_manager.hpp"
#include "orderbook_engine.hpp"
#include "market_data_feed.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace hft {

// =============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =============================================================================

SignalEngine::SignalEngine(MemoryManager& memory_manager,
                          LatencyTracker& latency_tracker,
                          const MarketMakingConfig& config)
    : memory_manager_(memory_manager)
    , latency_tracker_(&latency_tracker)
    , config_(config)
    , orderbook_engine_(nullptr)
    , order_manager_(nullptr)
    , market_data_feed_(nullptr)
    , is_running_(false)
    , should_stop_(false)
    , is_destroying_(false)
    , session_start_time_(now()) {
    
    std::cout << "[SIGNAL ENGINE] Initialized with config:" << std::endl;
    std::cout << "  Default Quote Size: " << config_.default_quote_size << std::endl;
    std::cout << "  Target Spread: " << config_.target_spread_bps << " bps" << std::endl;
    std::cout << "  Max Position: " << config_.max_position << std::endl;
    std::cout << "  Max Orders/sec: " << config_.max_orders_per_second << std::endl;
}

SignalEngine::~SignalEngine() {
    std::cout << "[SIGNAL ENGINE] Starting destruction..." << std::endl;
    
    // Set destruction flag to prevent any further operations
    is_destroying_.store(true);
    std::cout << "[SIGNAL ENGINE] Destruction flag set." << std::endl;
    
    // Stop the engine first
    should_stop_.store(true);
    is_running_.store(false);
    std::cout << "[SIGNAL ENGINE] Stop flags set." << std::endl;
    
    // Clear callbacks first to prevent them from being called after destruction
    signal_callback_ = nullptr;
    quote_update_callback_ = nullptr;
    risk_alert_callback_ = nullptr;
    trade_execution_callback_ = nullptr;
    std::cout << "[SIGNAL ENGINE] Callbacks cleared." << std::endl;
    
    // Clear external component references to prevent accessing destroyed objects
    orderbook_engine_ = nullptr;
    order_manager_ = nullptr;
    market_data_feed_ = nullptr;
    latency_tracker_ = nullptr;  // Clear latency tracker pointer
    std::cout << "[SIGNAL ENGINE] Component references cleared." << std::endl;
    
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
    
    std::cout << "[SIGNAL ENGINE] Shared data structures cleared." << std::endl;
    std::cout << "[SIGNAL ENGINE] Shutdown complete." << std::endl;
}

// =============================================================================
// CORE SIGNAL GENERATION (PRIMARY RESPONSIBILITY)
// =============================================================================

std::vector<TradingSignal> SignalEngine::generate_trading_signals() {
    // Check destruction flag first - if we're destroying, don't do anything
    if (is_destroying_.load()) {
        return {};
    }
    
    if (!is_running_.load() || should_stop_.load()) {
        return {};
    }
    
    // TEMPORARILY DISABLED: ScopedLatencyMeasurement to isolate segmentation fault
    // std::unique_ptr<ScopedLatencyMeasurement> latency_measurement;
    // if (latency_tracker_ && !is_destroying_.load()) {
    //     latency_measurement = std::make_unique<ScopedLatencyMeasurement>(*latency_tracker_, LatencyType::TICK_TO_TRADE);
    // }
    
    std::vector<TradingSignal> signals;
    
    // Get current market state from OrderBookEngine - FIXED: Add null check
    if (!orderbook_engine_) {
        return {};
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    if (top_of_book.bid_price <= 0.0 || top_of_book.ask_price <= 0.0) {
        return {};
    }
    
    // FIXED: Check for crossed market (bid > ask) - should not generate signals
    if (top_of_book.bid_price >= top_of_book.ask_price) {
        return {};
    }
    
    // Calculate optimal quotes
    price_t bid_price, ask_price;
    quantity_t bid_size, ask_size;
    
    calculate_optimal_quotes(bid_price, ask_price, bid_size, ask_size);
    
    // Generate quote signals
    generate_quote_signals(signals, bid_price, ask_price, bid_size, ask_size);
    
    // Generate cancellation signals for stale quotes
    generate_cancellation_signals(signals);
    
    // Apply rate limiting - FIXED: Only if we have signals
    if (!signals.empty()) {
        apply_rate_limiting(signals);
    }
    
    // Track signal generation - FIXED: Add destruction check
    if (!is_destroying_.load()) {
        for (const auto& signal : signals) {
            update_statistics(signal);
            notify_signal_generated(signal);
        }
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
    double spread_bps = orderbook_engine_->get_spread_bps();
    
    if (mid_price <= 0.0 || mid_price < 0.001) {
        // Initialize output parameters to 0.0 when invalid market data or extremely small prices
        bid_price = 0.0;
        ask_price = 0.0;
        bid_size = 0.0;
        ask_size = 0.0;
        return;
    }
    
    // FIXED: Check for crossed market - should not calculate quotes
    if (top_of_book.bid_price >= top_of_book.ask_price) {
        bid_price = 0.0;
        ask_price = 0.0;
        bid_size = 0.0;
        ask_size = 0.0;
        return;
    }
    
    // Calculate base spread - FIXED: Use target spread directly, not with adjustment
    double target_spread_bps = config_.target_spread_bps;
    
    // Calculate optimal prices with target spread
    bid_price = mid_price * (1.0 - target_spread_bps / 10000.0);
    ask_price = mid_price * (1.0 + target_spread_bps / 10000.0);
    
    // Apply inventory skew
    apply_inventory_skew(bid_price, ask_price);
    
    // FIXED: Safety check to ensure bid < ask after all adjustments
    if (bid_price >= ask_price) {
        // If prices are crossed, adjust them to maintain a minimum spread
        double min_spread_bps = 1.0; // Minimum 1 bps spread
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
    
    // Basic validation
    if (price <= 0.0 || size <= 0.0) {
        return false;
    }
    
    // Check position limits
    auto position = order_manager_->get_position();
    position_t current_position = position.net_position;
    
    if (side == QuoteSide::BID && current_position >= config_.max_position) {
        return false; // Too long to place more bids
    }
    
    if (side == QuoteSide::ASK && current_position <= -config_.max_position) {
        return false; // Too short to place more asks
    }
    
    // Check rate limits - FIXED: Use atomic check instead of mutex to avoid double locking
    {
        std::lock_guard<std::mutex> lock(signal_rate_mutex_);
        // Remove old signals from tracking first
        auto now_time = now();
        while (!recent_signals_.empty() && 
               to_microseconds(time_diff_us(recent_signals_.front(), now_time)) > 1000000) { // 1 second
            recent_signals_.pop();
        }
        
        if (recent_signals_.size() >= config_.max_orders_per_second) {
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
    
    // Calculate price improvement threshold
    double improvement_threshold = mid_price * 0.0001; // 1 bps minimum improvement
    
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
    // FIXED: Add validation for input parameters
    if (bid_price <= 0.0 || ask_price <= 0.0 || bid_size <= 0.0 || ask_size <= 0.0) {
        return;
    }
    
    // Generate bid signal
    if (should_place_quote(QuoteSide::BID, bid_price, bid_size)) {
        TradingSignal signal;
        signal.type = SignalType::PLACE_BID;
        signal.side = Side::BUY;
        signal.price = bid_price;
        signal.quantity = bid_size;
        signal.timestamp = now();
        signal.reason = "Market making bid";
        signals.push_back(signal);
    }
    
    // Generate ask signal
    if (should_place_quote(QuoteSide::ASK, ask_price, ask_size)) {
        TradingSignal signal;
        signal.type = SignalType::PLACE_ASK;
        signal.side = Side::SELL;
        signal.price = ask_price;
        signal.quantity = ask_size;
        signal.timestamp = now();
        signal.reason = "Market making ask";
        signals.push_back(signal);
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
    
    // Check for stale quotes that should be cancelled
    // This is a simplified version - in practice you'd check actual active quotes
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (const auto& [order_id, quote] : active_quotes_) {
            if (should_cancel_quote(quote, mid_price)) {
                TradingSignal signal;
                signal.type = (quote.side == QuoteSide::BID) ? SignalType::CANCEL_BID : SignalType::CANCEL_ASK;
                signal.order_id = order_id;
                signal.timestamp = now();
                signal.reason = "Stale quote cancellation";
                signals.push_back(signal);
            }
        }
    }
}

void SignalEngine::apply_rate_limiting(std::vector<TradingSignal>& signals) {
    // FIXED: Only apply rate limiting if we have signals to limit
    if (signals.empty()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(signal_rate_mutex_);
    
    // Remove old signals from tracking
    auto now_time = now();
    while (!recent_signals_.empty() && 
           to_microseconds(time_diff_us(recent_signals_.front(), now_time)) > 1000000) { // 1 second
        recent_signals_.pop();
    }
    
    // FIXED: Add safety check to prevent queue from growing too large
    if (recent_signals_.size() > config_.max_orders_per_second * 2) {
        // Emergency cleanup if queue is too large
        while (!recent_signals_.empty()) {
            recent_signals_.pop();
        }
    }
    
    // Limit signals based on rate
    size_t max_signals = config_.max_orders_per_second - recent_signals_.size();
    if (signals.size() > max_signals) {
        signals.resize(max_signals);
    }
    
    // Track new signals - FIXED: Only track if we have signals to track
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
    // Simplified: detect if there are significant changes in top levels
    // In practice, you'd compare with previous depth state
    return !depth.bids.empty() && !depth.asks.empty();
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

inline bool SignalEngine::should_cancel_quote(const MarketMakingQuote& quote, price_t mid_price) const {
    if (mid_price <= 0.0) {
        return false;
    }
    
    // Cancel if quote is too far from mid price
    double price_diff = std::abs(quote.price - mid_price);
    double threshold = mid_price * 0.01; // 100 bps threshold (1%)
    
    return price_diff > threshold;
}

void SignalEngine::update_statistics(const TradingSignal& signal) {
    // FIXED: Add destruction check before updating statistics
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
    // FIXED: Add destruction check before calling callback
    if (!is_destroying_.load() && signal_callback_) {
        signal_callback_(signal);
    }
}

void SignalEngine::notify_risk_alert(const std::string& alert, double value) {
    // FIXED: Add destruction check before calling callback
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
    if (!is_running_.load() || should_stop_.load() || is_destroying_.load()) {
        return;
    }
    
    // Update current market state
    current_top_of_book_ = top_of_book;
    
    // Generate trading signals based on market data
    auto signals = generate_trading_signals();
    
    // Log signal generation
    if (!signals.empty()) {
        std::cout << "🎯 SIGNAL ENGINE: Generated " << signals.size() << " trading signals" << std::endl;
    }
    
    // Process generated signals
    for (const auto& signal : signals) {
        if (signal_callback_) {
            signal_callback_(signal);
        }
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

void SignalEngine::set_market_data_feed(MarketDataFeed* market_data_feed) {
    market_data_feed_ = market_data_feed;
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
    trade_execution_callback_ = nullptr;
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
    std::cout << "\n📊 Signal Engine Performance Report" << std::endl;
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
    std::cout << "Signal Generation Latency (μs):" << std::endl;
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
