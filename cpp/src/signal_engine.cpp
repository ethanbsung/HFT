#include "signal_engine.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace hft {

// =========================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =========================================================================

SignalEngine::SignalEngine(MemoryManager& memory_manager,
                         LatencyTracker& latency_tracker,
                         const MarketMakingConfig& config)
    : memory_manager_(memory_manager),
      latency_tracker_(latency_tracker),
      config_(config),
      orderbook_engine_(nullptr),
      order_manager_(nullptr),
      market_data_feed_(nullptr),
      is_running_(false),
      should_stop_(false),
      session_start_time_(now()),
      last_mid_price_(0.0),
      last_spread_bps_(0.0),
      current_position_(0.0),
      current_pnl_(0.0),
      peak_equity_(1000.0),  // Initial capital
      max_drawdown_(0.0) {
    
    // Validate configuration
    if (!validate_config(config_)) {
        throw std::invalid_argument("Invalid market making configuration");
    }
    
    // Initialize market making state
    initialize_market_making_state();
    
    std::cout << "🚀 Signal Engine initialized with market making configuration" << std::endl;
    std::cout << "   Default quote size: " << config_.default_quote_size << std::endl;
    std::cout << "   Target spread: " << config_.target_spread_bps << " bps" << std::endl;
    std::cout << "   Max position: " << config_.max_position << std::endl;
    std::cout << "   Quote refresh: " << config_.quote_refresh_ms << " ms" << std::endl;
    std::cout << "   Cooldown: " << config_.cooldown_ms << " ms" << std::endl;
}

SignalEngine::~SignalEngine() {
    stop();
    std::cout << "🛑 Signal Engine destroyed" << std::endl;
}

// =========================================================================
// CORE SIGNAL ENGINE OPERATIONS
// =========================================================================

void SignalEngine::process_market_data_update(const TopOfBook& top_of_book) {
    // TODO: Implement market data processing
    // - Update current top of book
    // - Calculate mid price and spread
    // - Update quote state
    // - Generate trading signals
}

void SignalEngine::process_trade_execution(const TradeExecution& trade) {
    // TODO: Implement trade processing
    // - Update position tracking
    // - Calculate P&L impact
    // - Update quote state
    // - Check for risk violations
}

void SignalEngine::process_market_depth_update(const MarketDepth& depth) {
    // TODO: Implement market depth processing
    // - Update current market depth
    // - Analyze order flow
    // - Optimize quote placement
    // - Adjust spread based on depth
}

std::vector<TradingSignal> SignalEngine::generate_trading_signals() {
    // TODO: Implement signal generation
    // - Calculate optimal quotes
    // - Apply inventory skewing
    // - Check risk constraints
    // - Generate order signals
    return std::vector<TradingSignal>();
}

// =========================================================================
// MARKET MAKING STRATEGY OPERATIONS
// =========================================================================

void SignalEngine::calculate_optimal_quotes(price_t& bid_price, price_t& ask_price,
                                          quantity_t& bid_size, quantity_t& ask_size) {
    // TODO: Implement optimal quote calculation
    // - Get current market state
    // - Calculate mid price
    // - Apply target spread
    // - Set quote sizes
    // - Apply inventory skewing
}

void SignalEngine::apply_inventory_skew(price_t& bid_price, price_t& ask_price) {
    // TODO: Implement inventory skewing
    // - Get current position
    // - Calculate skew amount
    // - Apply asymmetric adjustments
    // - Respect maximum skew limits
}

bool SignalEngine::should_place_quote(QuoteSide side, price_t price, quantity_t size) {
    // TODO: Implement quote placement logic
    // - Check risk constraints
    // - Validate price levels
    // - Check position limits
    // - Consider market conditions
    return false;
}

bool SignalEngine::should_replace_quote(QuoteSide side, price_t current_price, price_t new_price) {
    // TODO: Implement quote replacement logic
    // - Compare price improvements
    // - Check cooldown periods
    // - Consider market impact
    // - Validate spread constraints
    return false;
}

// =========================================================================
// RISK MANAGEMENT OPERATIONS
// =========================================================================

bool SignalEngine::check_pre_trade_risk(const TradingSignal& signal) {
    // TODO: Implement pre-trade risk checks
    // - Position limit validation
    // - Daily loss limit check
    // - Drawdown monitoring
    // - Order rate limiting
    return true;
}

void SignalEngine::update_risk_metrics(const TopOfBook& market_data) {
    // TODO: Implement risk metrics update
    // - Update position tracking
    // - Calculate P&L metrics
    // - Monitor drawdown
    // - Track risk violations
}

bool SignalEngine::check_position_limits(Side side, quantity_t quantity) {
    // TODO: Implement position limit checks
    // - Calculate new position
    // - Check against limits
    // - Consider current exposure
    return true;
}

// =========================================================================
// CONFIGURATION AND CONTROL
// =========================================================================

bool SignalEngine::start() {
    if (is_running_.load()) {
        std::cout << "⚠️ Signal Engine is already running" << std::endl;
        return false;
    }
    
    // Validate component references
    if (!orderbook_engine_) {
        std::cerr << "❌ Cannot start: OrderBookEngine not set" << std::endl;
        return false;
    }
    
    if (!order_manager_) {
        std::cerr << "❌ Cannot start: OrderManager not set" << std::endl;
        return false;
    }
    
    // Reset session state
    session_start_time_ = now();
    should_stop_.store(false);
    is_running_.store(true);
    
    // Initialize market making state
    initialize_market_making_state();
    
    std::cout << "🚀 Signal Engine started successfully" << std::endl;
    std::cout << "   Session start: " << std::chrono::duration_cast<std::chrono::seconds>(
        session_start_time_.time_since_epoch()).count() << std::endl;
    
    return true;
}

void SignalEngine::stop() {
    if (!is_running_.load()) {
        std::cout << "⚠️ Signal Engine is not running" << std::endl;
        return;
    }
    
    should_stop_.store(true);
    is_running_.store(false);
    
    // Cancel all active quotes
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (auto& [side, quote] : active_quotes_) {
            if (quote.state == QuoteState::ACTIVE) {
                quote.state = QuoteState::CANCELLING;
                std::cout << "🛑 Cancelling quote: " << quote_side_to_string(side) 
                          << " @ " << quote.price << std::endl;
            }
        }
    }
    
    // Calculate session statistics
    auto session_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now() - session_start_time_).count();
    
    std::cout << "🛑 Signal Engine stopped" << std::endl;
    std::cout << "   Session duration: " << session_duration << " seconds" << std::endl;
    std::cout << "   Final position: " << current_position_.load() << std::endl;
    std::cout << "   Final P&L: " << current_pnl_.load() << std::endl;
}

void SignalEngine::update_config(const MarketMakingConfig& config) {
    // Validate new configuration
    if (!validate_config(config)) {
        std::cerr << "❌ Invalid configuration update rejected" << std::endl;
        return;
    }
    
    // Update configuration
    config_ = config;
    
    std::cout << "⚙️ Signal Engine configuration updated" << std::endl;
    std::cout << "   New target spread: " << config_.target_spread_bps << " bps" << std::endl;
    std::cout << "   New max position: " << config_.max_position << std::endl;
    std::cout << "   New quote refresh: " << config_.quote_refresh_ms << " ms" << std::endl;
}

void SignalEngine::set_orderbook_engine(OrderBookEngine* orderbook_engine) {
    orderbook_engine_ = orderbook_engine;
    if (orderbook_engine_) {
        std::cout << "🔗 OrderBookEngine connected to Signal Engine" << std::endl;
    }
}

void SignalEngine::set_order_manager(OrderManager* order_manager) {
    order_manager_ = order_manager;
    if (order_manager_) {
        std::cout << "🔗 OrderManager connected to Signal Engine" << std::endl;
    }
}

void SignalEngine::set_market_data_feed(MarketDataFeed* market_data_feed) {
    market_data_feed_ = market_data_feed;
    if (market_data_feed_) {
        std::cout << "🔗 MarketDataFeed connected to Signal Engine" << std::endl;
    }
}

// =========================================================================
// CALLBACK MANAGEMENT
// =========================================================================

void SignalEngine::set_signal_callback(SignalCallback callback) {
    signal_callback_ = callback;
    std::cout << "🔗 Signal callback registered" << std::endl;
}

void SignalEngine::set_quote_update_callback(QuoteUpdateCallback callback) {
    quote_update_callback_ = callback;
    std::cout << "🔗 Quote update callback registered" << std::endl;
}

void SignalEngine::set_risk_alert_callback(RiskAlertCallback callback) {
    risk_alert_callback_ = callback;
    std::cout << "🔗 Risk alert callback registered" << std::endl;
}

// =========================================================================
// MONITORING AND STATISTICS
// =========================================================================

MarketMakingStats SignalEngine::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return statistics_;
}

std::vector<MarketMakingQuote> SignalEngine::get_active_quotes() const {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    std::vector<MarketMakingQuote> quotes;
    quotes.reserve(active_quotes_.size());
    
    for (const auto& [side, quote] : active_quotes_) {
        quotes.push_back(quote);
    }
    
    return quotes;
}

PositionInfo SignalEngine::get_position_info() const {
    PositionInfo info;
    info.size = current_position_.load();
    info.average_price = 0.0; // TODO: Calculate from trade history
    info.unrealized_pnl = current_pnl_.load();
    info.realized_pnl = 0.0; // TODO: Calculate from closed positions
    return info;
}

void SignalEngine::print_performance_report() const {
    std::cout << "\n📊 Signal Engine Performance Report" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    // Session information
    auto session_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now() - session_start_time_).count();
    std::cout << "Session Duration: " << session_duration << " seconds" << std::endl;
    std::cout << "Running: " << (is_running_.load() ? "Yes" : "No") << std::endl;
    
    // Position and P&L
    std::cout << "Current Position: " << current_position_.load() << std::endl;
    std::cout << "Current P&L: " << current_pnl_.load() << std::endl;
    std::cout << "Peak Equity: " << peak_equity_.load() << std::endl;
    std::cout << "Max Drawdown: " << max_drawdown_.load() << std::endl;
    
    // Quote statistics
    auto stats = get_statistics();
    std::cout << "Quotes Placed: " << stats.total_quotes_placed << std::endl;
    std::cout << "Quotes Filled: " << stats.total_quotes_filled << std::endl;
    std::cout << "Quotes Cancelled: " << stats.total_quotes_cancelled << std::endl;
    std::cout << "Fill Rate: " << (stats.fill_rate * 100.0) << "%" << std::endl;
    std::cout << "Avg Spread Captured: " << stats.avg_spread_captured_bps << " bps" << std::endl;
    
    // Risk metrics
    std::cout << "Risk Violations: " << stats.risk_violations << std::endl;
    std::cout << "Position Limit Utilization: " << (stats.position_limit_utilization * 100.0) << "%" << std::endl;
    
    // Performance metrics
    std::cout << "Avg Signal Generation Latency: " << stats.avg_signal_generation_latency_us << " μs" << std::endl;
    std::cout << "Avg Quote Placement Latency: " << stats.avg_quote_placement_latency_us << " μs" << std::endl;
    
    std::cout << "=====================================\n" << std::endl;
}

void SignalEngine::reset_daily_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // Reset daily counters
    statistics_.total_quotes_placed = 0;
    statistics_.total_quotes_filled = 0;
    statistics_.total_quotes_cancelled = 0;
    statistics_.risk_violations = 0;
    
    // Reset performance metrics
    statistics_.avg_signal_generation_latency_us = 0.0;
    statistics_.avg_quote_placement_latency_us = 0.0;
    
    std::cout << "🔄 Daily statistics reset" << std::endl;
}

bool SignalEngine::is_healthy() const {
    // Check if engine is running
    if (!is_running_.load()) {
        return false;
    }
    
    // Check component connections
    if (!orderbook_engine_ || !order_manager_) {
        return false;
    }
    
    // Check for excessive risk violations
    auto stats = get_statistics();
    if (stats.risk_violations > 10) {
        return false;
    }
    
    // Check for excessive drawdown
    if (max_drawdown_.load() > config_.max_drawdown) {
        return false;
    }
    
    return true;
}

LatencyStatistics SignalEngine::get_signal_generation_latency() const {
    return latency_tracker_.get_statistics(LatencyType::TICK_TO_TRADE);
}

// =========================================================================
// PRIVATE HELPER METHODS
// =========================================================================

void SignalEngine::initialize_market_making_state() {
    // Clear active quotes
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        active_quotes_.clear();
    }
    
    // Initialize statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        statistics_ = MarketMakingStats();
    }
    
    // Reset position and P&L tracking
    current_position_.store(0.0);
    current_pnl_.store(0.0);
    peak_equity_.store(1000.0);
    max_drawdown_.store(0.0);
    
    // Clear performance tracking
    {
        std::lock_guard<std::mutex> lock(signal_rate_mutex_);
        while (!recent_signals_.empty()) {
            recent_signals_.pop();
        }
    }
    
    std::cout << "🔄 Market making state initialized" << std::endl;
}

void SignalEngine::update_quote_state(const TopOfBook& top_of_book) {
    // TODO: Implement quote state update
    // - Update current quotes
    // - Check quote validity
    // - Adjust quote parameters
    // - Handle quote lifecycle
}

void SignalEngine::calculate_spread_adjustments(price_t& bid_price, price_t& ask_price) {
    // TODO: Implement spread adjustments
    // - Calculate optimal spread
    // - Apply market conditions
    // - Consider volatility
    // - Adjust for competition
}

void SignalEngine::track_signal_generation_latency(timestamp_t start_time) {
    auto end_time = now();
    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time).count();
    
    // Track latency in latency tracker
    latency_tracker_.add_latency(LatencyType::TICK_TO_TRADE, latency_us);
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        statistics_.avg_signal_generation_latency_us = 
            (statistics_.avg_signal_generation_latency_us + latency_us) / 2.0;
    }
}

void SignalEngine::update_statistics(const TradingSignal& signal) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // Update signal counters based on signal type
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
    
    // Update fill rate
    if (statistics_.total_quotes_placed > 0) {
        statistics_.fill_rate = static_cast<double>(statistics_.total_quotes_filled) / 
                               statistics_.total_quotes_placed;
    }
}

void SignalEngine::notify_signal_generated(const TradingSignal& signal) {
    if (signal_callback_) {
        signal_callback_(signal);
    }
}

void SignalEngine::notify_quote_updated(const MarketMakingQuote& quote) {
    if (quote_update_callback_) {
        quote_update_callback_(quote);
    }
}

void SignalEngine::notify_risk_alert(const std::string& alert, double value) {
    if (risk_alert_callback_) {
        risk_alert_callback_(alert, value);
    }
}

bool SignalEngine::validate_config(const MarketMakingConfig& config) const {
    // Check quote parameters
    if (config.default_quote_size <= 0.0) {
        std::cerr << "❌ Invalid default quote size: " << config.default_quote_size << std::endl;
        return false;
    }
    
    if (config.min_spread_bps < 0.0 || config.max_spread_bps < config.min_spread_bps) {
        std::cerr << "❌ Invalid spread configuration: min=" << config.min_spread_bps 
                  << ", max=" << config.max_spread_bps << std::endl;
        return false;
    }
    
    if (config.target_spread_bps < config.min_spread_bps || 
        config.target_spread_bps > config.max_spread_bps) {
        std::cerr << "❌ Invalid target spread: " << config.target_spread_bps << std::endl;
        return false;
    }
    
    // Check inventory management
    if (config.max_position <= 0.0) {
        std::cerr << "❌ Invalid max position: " << config.max_position << std::endl;
        return false;
    }
    
    if (config.inventory_skew_factor < 0.0 || config.inventory_skew_factor > 1.0) {
        std::cerr << "❌ Invalid inventory skew factor: " << config.inventory_skew_factor << std::endl;
        return false;
    }
    
    // Check risk management
    if (config.max_daily_loss <= 0.0) {
        std::cerr << "❌ Invalid max daily loss: " << config.max_daily_loss << std::endl;
        return false;
    }
    
    if (config.max_drawdown <= 0.0 || config.max_drawdown > 1.0) {
        std::cerr << "❌ Invalid max drawdown: " << config.max_drawdown << std::endl;
        return false;
    }
    
    if (config.max_orders_per_second <= 0) {
        std::cerr << "❌ Invalid max orders per second: " << config.max_orders_per_second << std::endl;
        return false;
    }
    
    // Check performance settings
    if (config.quote_refresh_ms <= 0) {
        std::cerr << "❌ Invalid quote refresh: " << config.quote_refresh_ms << " ms" << std::endl;
        return false;
    }
    
    if (config.cooldown_ms <= 0) {
        std::cerr << "❌ Invalid cooldown: " << config.cooldown_ms << " ms" << std::endl;
        return false;
    }
    
    return true;
}

bool SignalEngine::is_market_suitable_for_quoting() const {
    // TODO: Implement market suitability check
    // - Check spread conditions
    // - Validate market depth
    // - Consider volatility
    // - Assess market quality
    return true;
}

double SignalEngine::calculate_market_volatility() const {
    // TODO: Implement volatility calculation
    // - Calculate price volatility
    // - Consider recent trades
    // - Analyze spread movement
    // - Return volatility metric
    return 0.0;
}

void SignalEngine::update_position_tracking(const TradeExecution& trade) {
    // TODO: Implement position tracking update
    // - Update position size
    // - Calculate P&L impact
    // - Track trade history
    // - Update risk metrics
}

void SignalEngine::calculate_pnl_metrics() {
    // TODO: Implement P&L calculation
    // - Calculate realized P&L
    // - Calculate unrealized P&L
    // - Update peak equity
    // - Monitor drawdown
}

bool SignalEngine::check_risk_violations() {
    // TODO: Implement risk violation checks
    // - Check position limits
    // - Monitor daily loss
    // - Track drawdown
    // - Validate risk constraints
    return false;
}

void SignalEngine::emergency_shutdown(const std::string& reason) {
    std::cerr << "🚨 EMERGENCY SHUTDOWN: " << reason << std::endl;
    
    // Stop the engine
    stop();
    
    // Cancel all quotes immediately
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (auto& [side, quote] : active_quotes_) {
            quote.state = QuoteState::CANCELLING;
        }
    }
    
    // Notify risk alert
    notify_risk_alert("EMERGENCY_SHUTDOWN", 1.0);
    
    std::cerr << "🛑 Emergency shutdown completed" << std::endl;
}

// =========================================================================
// UTILITY FUNCTIONS
// =========================================================================

MarketMakingConfig create_default_market_making_config() {
    MarketMakingConfig config;
    // Default values are already set in constructor
    return config;
}

MarketMakingConfig create_aggressive_market_making_config() {
    MarketMakingConfig config;
    // TODO: Set aggressive configuration values
    config.enable_aggressive_quotes = true;
    config.target_spread_bps = 10.0;
    config.max_orders_per_second = 200;
    return config;
}

MarketMakingConfig create_conservative_market_making_config() {
    MarketMakingConfig config;
    // TODO: Set conservative configuration values
    config.target_spread_bps = 25.0;
    config.max_position = 50.0;
    config.max_orders_per_second = 50;
    return config;
}

bool validate_trading_signal(const TradingSignal& signal) {
    // TODO: Implement signal validation
    // - Check signal type
    // - Validate parameters
    // - Ensure consistency
    // - Return validation result
    return true;
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
