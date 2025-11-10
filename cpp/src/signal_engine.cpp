#include "signal_engine.hpp"
#include "order_manager.hpp"
#include "orderbook_engine.hpp"
#include "market_data_feed.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <unordered_set>

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
    , session_start_time_(now())
    , next_signal_id_(1) {
    
    std::cout << "[SIGNAL ENGINE] Initialized with AGGRESSIVE config:" << std::endl;
    std::cout << "  Default Quote Size: " << config_.default_quote_size << std::endl;
    std::cout << "  Target Spread: " << config_.target_spread_bps << " bps" << std::endl;
    std::cout << "  Min/Max Spread: " << config_.min_spread_bps << "/" << config_.max_spread_bps << " bps" << std::endl;
    std::cout << "  Max Position: " << config_.max_position << std::endl;
    std::cout << "  Max Orders/sec: " << config_.max_orders_per_second << std::endl;
    std::cout << "  Quote Refresh: " << config_.quote_refresh_ms << " ms" << std::endl;
    std::cout << "  Cooldown: " << config_.cooldown_ms << " ms" << std::endl;
    std::cout << "  Aggressive Quotes: " << (config_.enable_aggressive_quotes ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "  Inventory Skew Factor: " << config_.inventory_skew_factor << std::endl;
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
    // Generate trading signals based on market data
    
    // Check destruction flag first - if we're destroying, don't do anything
    if (is_destroying_.load()) {
        // Engine shutting down
        return {};
    }
    
    if (!is_running_.load() || should_stop_.load()) {
        // Engine stopped
        return {};
    }
    
    // Calculating optimal quotes based on market conditions
    
    // TEMPORARILY DISABLED: ScopedLatencyMeasurement to isolate segmentation fault
    // std::unique_ptr<ScopedLatencyMeasurement> latency_measurement;
    // if (latency_tracker_ && !is_destroying_.load()) {
    //     latency_measurement = std::make_unique<ScopedLatencyMeasurement>(*latency_tracker_, LatencyType::TICK_TO_TRADE);
    // }
    
    std::vector<TradingSignal> signals;
    
    // Get current market state from OrderBookEngine - FIXED: Add null check
    if (!orderbook_engine_) {
        // No OrderBookEngine available
        return {};
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    // Market: Bid=$" << top_of_book.bid_price << " Ask=$" << top_of_book.ask_price
    
    // FIXED: Relax validation to allow signal generation even with imperfect market data
    if (top_of_book.bid_price <= 0.0 && top_of_book.ask_price <= 0.0) {
        // No valid market data
        return {};
    }
    
    // FIXED: Allow crossed markets temporarily - the orderbook will handle this
    if (top_of_book.bid_price > 0.0 && top_of_book.ask_price > 0.0 && top_of_book.bid_price >= top_of_book.ask_price) {
//         std::cout << "⚠️ DEBUG: Crossed market detected but proceeding with signal generation" << std::endl;
    }
    
    // Calculating optimal quotes
    
    // Calculate optimal quotes
    price_t bid_price, ask_price;
    quantity_t bid_size, ask_size;
    
    calculate_optimal_quotes(bid_price, ask_price, bid_size, ask_size);
    
//     std::cout << "📈 DEBUG: Optimal quotes calculated:" << std::endl;
    std::cout << "   Bid: $" << bid_price << " x " << bid_size << std::endl;
    std::cout << "   Ask: $" << ask_price << " x " << ask_size << std::endl;
    std::cout << "   Market Bid: $" << top_of_book.bid_price << " Market Ask: $" << top_of_book.ask_price << std::endl;
    std::cout << "   Our Spread: $" << (ask_price - bid_price) << " Market Spread: $" << (top_of_book.ask_price - top_of_book.bid_price) << std::endl;
    
    // Generate quote signals
    generate_quote_signals(signals, bid_price, ask_price, bid_size, ask_size);
    
    // Only generate cancellation signals if we're placing new quotes
    if (!signals.empty()) {
        generate_cancellation_signals(signals);
    }
    
    std::cout << "📋 DEBUG: Generated " << signals.size() << " signals" << std::endl;
    
    // Apply rate limiting - FIXED: Only if we have signals
    if (!signals.empty()) {
        apply_rate_limiting(signals);
            // Rate limited: " << signals.size() << " signals
    }
    
    // Track signal generation - FIXED: Add destruction check
    if (!is_destroying_.load()) {
        for (const auto& signal : signals) {
            update_statistics(signal);
            notify_signal_generated(signal);
        }
    }
    
    // Signal generation complete
    return signals;
}

void SignalEngine::calculate_optimal_quotes(price_t& bid_price, price_t& ask_price,
                                          quantity_t& bid_size, quantity_t& ask_size) {
    if (!orderbook_engine_) {
//         std::cout << "❌ DEBUG: No OrderBookEngine for quote calculation" << std::endl;
        return;
    }
    
    auto top_of_book = orderbook_engine_->get_top_of_book();
    price_t mid_price = top_of_book.mid_price;
    double spread_bps = orderbook_engine_->get_spread_bps();
    
//     std::cout << "📊 DEBUG: Quote calculation inputs:" << std::endl;
    std::cout << "   Mid Price: $" << mid_price << std::endl;
    std::cout << "   Current Spread: " << spread_bps << " bps" << std::endl;
    std::cout << "   Target Spread: " << config_.target_spread_bps << " bps" << std::endl;
    
    // FIXED: Use a fallback mid price if the calculated one is invalid
    if (mid_price <= 0.0 || mid_price < 0.001) {
        // Use the best available price as fallback
        if (top_of_book.bid_price > 0.0) {
            mid_price = top_of_book.bid_price;
        } else if (top_of_book.ask_price > 0.0) {
            mid_price = top_of_book.ask_price;
        } else {
            // Use a reasonable default price for BTC
            mid_price = 118000.0; // Default BTC price
        }
//         std::cout << "⚠️ DEBUG: Using fallback mid price: $" << mid_price << std::endl;
    }
    
    // FIXED: Remove crossed market check that was preventing quote generation
    if (top_of_book.bid_price > 0.0 && top_of_book.ask_price > 0.0 && top_of_book.bid_price >= top_of_book.ask_price) {
//         std::cout << "⚠️ DEBUG: Crossed market detected but proceeding with quote calculation" << std::endl;
    }
    
    // PYTHON-STYLE AGGRESSIVE QUOTING: Place orders very close to market
    // Based on your Python system that quotes within 1-2 ticks of best bid/ask
    double target_spread_bps = config_.target_spread_bps;
    double current_spread_bps = orderbook_engine_->get_spread_bps();
    
    // AGGRESSIVE STRATEGY: Quote inside the spread when possible
    if (current_spread_bps > 5.0) { // If spread > 5 bps, we can be aggressive
        // Quote 1 tick inside best bid/ask (like your Python system)
        target_spread_bps = std::max(1.0, current_spread_bps * 0.1); // 1 bps minimum
//         std::cout << "📊 DEBUG: INSIDE SPREAD quoting: " << target_spread_bps << " bps (market: " << current_spread_bps << " bps)" << std::endl;
    } else if (current_spread_bps > 2.0) {
        // Tight spread - join the best bid/ask
        target_spread_bps = std::max(0.5, current_spread_bps * 0.2);
//         std::cout << "📊 DEBUG: JOIN BEST quoting: " << target_spread_bps << " bps (market: " << current_spread_bps << " bps)" << std::endl;
    } else {
        // Very tight market - use minimal spread
        target_spread_bps = std::max(0.1, current_spread_bps * 0.5);
//         std::cout << "📊 DEBUG: MINIMAL SPREAD quoting: " << target_spread_bps << " bps (market: " << current_spread_bps << " bps)" << std::endl;
    }
    
    // FIXED: Remove minimum spread check that was preventing quote generation
    if (current_spread_bps > 0.0 && current_spread_bps < 0.5) { // If spread is extremely tight (< 0.5 bps)
//         std::cout << "⚠️ DEBUG: Market spread tight (" << current_spread_bps << " bps) but proceeding with quotes" << std::endl;
    }
    
    // PYTHON-STYLE COMPETITIVE PRICING: Quote at or inside best bid/ask
    // This matches your Python system's aggressive quoting strategy
    
    if (top_of_book.bid_price > 0.0 && top_of_book.ask_price > 0.0) {
        // Calculate tick size (assume 1 cent for BTC-USD)
        double tick_size = 0.01;
        
        if (current_spread_bps > 5.0) {
            // Wide spread: Quote 1 tick inside best bid/ask (improve the market)
            bid_price = top_of_book.bid_price + tick_size;
            ask_price = top_of_book.ask_price - tick_size;
//             std::cout << "📊 DEBUG: INSIDE SPREAD quotes - Bid: $" << bid_price << " Ask: $" << ask_price << std::endl;
        } else {
            // Tight spread: JOIN the best bid/ask for maximum fill probability
            // This is much more aggressive than staying away from the market
            bid_price = top_of_book.bid_price;
            ask_price = top_of_book.ask_price;
//             std::cout << "📊 DEBUG: JOIN BEST quotes - Bid: $" << bid_price << " Ask: $" << ask_price << std::endl;
        }
    } else {
        // Fallback to spread-based pricing
        bid_price = mid_price * (1.0 - target_spread_bps / 10000.0);
        ask_price = mid_price * (1.0 + target_spread_bps / 10000.0);
//         std::cout << "📊 DEBUG: FALLBACK spread-based quotes - Bid: $" << bid_price << " Ask: $" << ask_price << std::endl;
    }
    
//     std::cout << "📈 DEBUG: Initial quotes:" << std::endl;
    std::cout << "   Bid: $" << bid_price << std::endl;
    std::cout << "   Ask: $" << ask_price << std::endl;
    
    // Apply inventory skew
    apply_inventory_skew(bid_price, ask_price);
    
//     std::cout << "📊 DEBUG: After inventory skew:" << std::endl;
    std::cout << "   Bid: $" << bid_price << std::endl;
    std::cout << "   Ask: $" << ask_price << std::endl;
    
    // FIXED: Safety check to ensure bid < ask after all adjustments
    if (bid_price >= ask_price) {
        // If prices are crossed, adjust them to maintain a minimum spread
        double min_spread_bps = 1.0; // Minimum 1 bps spread
        bid_price = mid_price * (1.0 - min_spread_bps / 10000.0);
        ask_price = mid_price * (1.0 + min_spread_bps / 10000.0);
//         std::cout << "⚠️ DEBUG: Prices crossed - adjusted to minimum spread" << std::endl;
    }
    
    // Calculate sizes
    bid_size = calculate_position_adjusted_size(config_.default_quote_size, QuoteSide::BID);
    ask_size = calculate_position_adjusted_size(config_.default_quote_size, QuoteSide::ASK);
    
//     std::cout << "📊 DEBUG: Final quote sizes:" << std::endl;
    std::cout << "   Bid Size: " << bid_size << std::endl;
    std::cout << "   Ask Size: " << ask_size << std::endl;
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
//         std::cout << "❌ DEBUG: No OrderManager available for quote validation" << std::endl;
        return false;
    }
    
    // Validating quote
    
    // Basic validation
    if (price <= 0.0 || size <= 0.0) {
//         std::cout << "❌ DEBUG: Invalid price or size - price: $" << price << " size: " << size << std::endl;
        return false;
    }
    
    // FIXED: Relax position limits to allow more trading
    auto position = order_manager_->get_position();
    position_t current_position = position.net_position;
    
    // Position check: " << current_position
    
    // AGGRESSIVE: Allow even more position building - only warn at 90% instead of 80%
    if (side == QuoteSide::BID && current_position >= config_.max_position * 0.9) {
//         std::cout << "⚠️ DEBUG: AGGRESSIVE - Approaching long limit - position: " << current_position << " but allowing quote" << std::endl;
        // Allow quotes with full size for aggressive trading
    }
    
    if (side == QuoteSide::ASK && current_position <= -config_.max_position * 0.9) {
//         std::cout << "⚠️ DEBUG: AGGRESSIVE - Approaching short limit - position: " << current_position << " but allowing quote" << std::endl;
        // Allow quotes with full size for aggressive trading
    }
    
    // FIXED: Remove strict position limits that were preventing trading
    if (side == QuoteSide::BID && current_position >= config_.max_position) {
//         std::cout << "❌ DEBUG: Too long to place more bids - position: " << current_position << std::endl;
        return false; // Too long to place more bids
    }
    
    if (side == QuoteSide::ASK && current_position <= -config_.max_position) {
//         std::cout << "❌ DEBUG: Too short to place more asks - position: " << current_position << std::endl;
        return false; // Too short to place more asks
    }
    
    // FIXED: Relax rate limits to allow more trading
    {
        std::lock_guard<std::mutex> lock(signal_rate_mutex_);
        // Remove old signals from tracking first
        auto now_time = now();
        while (!recent_signals_.empty() && 
               to_microseconds(time_diff_us(recent_signals_.front(), now_time)) > 1000000) { // 1 second
            recent_signals_.pop();
        }
        
        // Rate limit check: " << recent_signals_.size() << " signals"
        
        // FIXED: Allow more signals per second
        if (recent_signals_.size() >= config_.max_orders_per_second * 2) {
//             std::cout << "⚠️ DEBUG: Rate limit approaching but allowing quote" << std::endl;
            // Allow the quote but log the warning
        }
        
        if (recent_signals_.size() >= config_.max_orders_per_second * 3) {
//             std::cout << "❌ DEBUG: Rate limit exceeded" << std::endl;
            return false;
        }
    }
    
//     std::cout << "✅ DEBUG: Quote validation passed" << std::endl;
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
    // FIXED: Add validation for input parameters
    if (bid_price <= 0.0 || ask_price <= 0.0 || bid_size <= 0.0 || ask_size <= 0.0) {
//         std::cout << "❌ DEBUG: Invalid quote parameters - no signals generated" << std::endl;
        return;
    }
    
//     std::cout << "🎯 DEBUG: Generating quote signals..." << std::endl;
    
    // SIMPLIFIED QUOTE REPLACEMENT: Always replace quotes that are too old
    bool should_replace_quotes = false;
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
//         std::cout << "🔍 DEBUG: active_quotes_.size() = " << active_quotes_.size() << std::endl;
        
        if (!active_quotes_.empty()) {
//             std::cout << "🔍 DEBUG: Checking existing quotes for replacement..." << std::endl;
            
            // Check each quote's age - replace if any quote is older than 500ms
            auto now_time = now();
            for (const auto& [order_id, quote] : active_quotes_) {
                auto quote_age_us = std::chrono::duration_cast<std::chrono::microseconds>(now_time - quote.creation_time).count();
                auto quote_age_ms = quote_age_us / 1000;
                
                std::cout << "🕒 DEBUG: Quote ID " << order_id << " (" << (quote.side == QuoteSide::BID ? "BID" : "ASK") 
                          << " $" << quote.price << ") age: " << quote_age_ms << "ms (limit: 500ms)" << std::endl;
                
                if (quote_age_ms > 500) {
                    should_replace_quotes = true;
                    std::cout << "🕒 DEBUG: REPLACE QUOTE - Quote ID " << order_id << " aged " << quote_age_ms 
                              << "ms, replacing all quotes" << std::endl;
                    break; // Replace all quotes if any one is too old
                }
            }
            
            // Also check for significant price improvement - USE MARKET MID PRICE, NOT OUR QUOTES!
            static price_t last_market_mid_price = 0.0;
            // Get actual market mid price from orderbook engine, not our calculated quotes
            auto top_of_book = orderbook_engine_->get_top_of_book();
            price_t current_market_mid = top_of_book.mid_price;
            
            if (last_market_mid_price > 0.0 && current_market_mid > 0.0) {
                double price_change_bps = std::abs(current_market_mid - last_market_mid_price) / last_market_mid_price * 10000.0;
                if (price_change_bps > 0.5) { // Market moved more than 0.5 bps (more aggressive)
                    should_replace_quotes = true;
                    std::cout << "📈 DEBUG: MARKET MOVEMENT - Market mid moved " << price_change_bps << " bps, replacing quotes" << std::endl;
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
                        // Our bid should be at or very close to the best bid
                        is_competitive = (quote.price >= top_of_book.bid_price - 0.01);
                    } else {
                        // Our ask should be at or very close to the best ask  
                        is_competitive = (quote.price <= top_of_book.ask_price + 0.01);
                    }
                    
                    if (!is_competitive) {
                        should_replace_quotes = true;
                        std::cout << "💰 DEBUG: UNCOMPETITIVE QUOTE - " << (quote.side == QuoteSide::BID ? "BID" : "ASK") 
                                  << " $" << quote.price << " not competitive with market " 
                                  << (quote.side == QuoteSide::BID ? "bid $" : "ask $") 
                                  << (quote.side == QuoteSide::BID ? top_of_book.bid_price : top_of_book.ask_price) 
                                  << ", replacing quotes" << std::endl;
                        break;
                    }
                }
            }
            
        } else {
            // No existing quotes, should place new ones
            should_replace_quotes = true;
//             std::cout << "🔍 DEBUG: No existing quotes, placing new quotes" << std::endl;
        }
    }
    
//     std::cout << "🔍 DEBUG: should_replace_quotes = " << (should_replace_quotes ? "true" : "false") << std::endl;
    
        if (should_replace_quotes) {
        // Check if we have existing quotes to cancel
        bool has_existing_quotes = false;
        {
            std::lock_guard<std::mutex> lock(quotes_mutex_);
            has_existing_quotes = !active_quotes_.empty();
        }
        
        // **CRITICAL FIX: Targeted replacement instead of blanket cancellation**
        // Only cancel and replace the sides we intend to place new quotes for
        
        bool should_place_bid = should_place_quote(QuoteSide::BID, bid_price, bid_size);
        bool should_place_ask = should_place_quote(QuoteSide::ASK, ask_price, ask_size);
        
        if (has_existing_quotes) {
            // Cancel only the sides we're going to replace
            if (should_place_bid) {
                generate_targeted_cancellation_signals(signals, QuoteSide::BID);
                std::cout << "🔄 DEBUG: Cancelling existing BID quotes for replacement" << std::endl;
            }
            if (should_place_ask) {
                generate_targeted_cancellation_signals(signals, QuoteSide::ASK);
                std::cout << "🔄 DEBUG: Cancelling existing ASK quotes for replacement" << std::endl;
            }
        }
        
        // Generate new quote signals (regardless of whether we have existing quotes)
        if (should_place_bid) {
            TradingSignal signal;
            signal.type = SignalType::PLACE_BID;
            signal.side = Side::BUY;
            signal.price = bid_price;
            signal.quantity = bid_size;
            signal.timestamp = now();
            signal.reason = "Market making bid";
            signal.order_id = next_signal_id_++;  // Add unique signal ID
            signals.push_back(signal);
            std::cout << "✅ DEBUG: Generated BID signal ID " << signal.order_id << " - $" << bid_price << " x " << bid_size << std::endl;
        }
        
        if (should_place_ask) {
            TradingSignal signal;
            signal.type = SignalType::PLACE_ASK;
            signal.side = Side::SELL;
            signal.price = ask_price;
            signal.quantity = ask_size;
            signal.timestamp = now();
            signal.reason = "Market making ask";
            signal.order_id = next_signal_id_++;  // Add unique signal ID
            signals.push_back(signal);
            std::cout << "✅ DEBUG: Generated ASK signal ID " << signal.order_id << " - $" << ask_price << " x " << ask_size << std::endl;
        }
    } else {
//         std::cout << "📊 DEBUG: Existing quotes are competitive, no replacement needed" << std::endl;
    }
    
//     std::cout << "📊 DEBUG: Quote signal generation complete - " << signals.size() << " signals" << std::endl;
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
    
    // FIXED: Only cancel quotes that are actually active and not already cancelled
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (const auto& [order_id, quote] : active_quotes_) {
            // We track our own quotes, so if it's in active_quotes_ it's ours
            TradingSignal signal;
            signal.type = (quote.side == QuoteSide::BID) ? SignalType::CANCEL_BID : SignalType::CANCEL_ASK;
            signal.order_id = order_id;
            signal.timestamp = now();
            signal.reason = "Replacing quote with new market making quote";
            signals.push_back(signal);
            std::cout << "🔄 DEBUG: Cancelling existing quote - Order ID: " << order_id 
                      << " Side: " << (quote.side == QuoteSide::BID ? "BID" : "ASK") << std::endl;
        }
        
        // Don't clear active_quotes_ here - let the order manager handle the cancellations
        // active_quotes_ will be updated when order callbacks are received
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
                std::cout << "🔄 DEBUG: Cancelling " << (side == QuoteSide::BID ? "BID" : "ASK") 
                          << " quote - Order ID: " << order_id << " Price: $" << quote.price << std::endl;
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
    
    std::cout << "📝 DEBUG: Tracked order placement - ID: " << order_id 
              << " Side: " << (side == QuoteSide::BID ? "BID" : "ASK")
              << " Price: $" << price << " Qty: " << quantity << std::endl;
}

void SignalEngine::track_order_cancellation(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    
    auto it = active_quotes_.find(order_id);
    if (it != active_quotes_.end()) {
        std::cout << "📝 DEBUG: Tracked order cancellation - ID: " << order_id 
                  << " Side: " << (it->second.side == QuoteSide::BID ? "BID" : "ASK") << std::endl;
        active_quotes_.erase(it);
    }
}

void SignalEngine::track_order_fill(uint64_t order_id, quantity_t fill_qty, price_t fill_price) {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    
    auto it = active_quotes_.find(order_id);
    if (it != active_quotes_.end()) {
        it->second.filled_quantity += fill_qty;
        it->second.last_update_time = now();
        
        std::cout << "📝 DEBUG: Tracked order fill - ID: " << order_id 
                  << " Fill Qty: " << fill_qty << " @ $" << fill_price 
                  << " Total Filled: " << it->second.filled_quantity << std::endl;
        
        // If order is completely filled, remove it from active quotes
        if (it->second.filled_quantity >= it->second.quantity) {
//             std::cout << "✅ DEBUG: Order completely filled - removing from active quotes" << std::endl;
            active_quotes_.erase(it);
        }
    }
}

void SignalEngine::clear_stale_quotes() {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
    
    auto now_time = now();
    auto it = active_quotes_.begin();
    while (it != active_quotes_.end()) {
        // Remove quotes older than 30 seconds
        auto age = time_diff_us(it->second.creation_time, now_time);
        if (to_microseconds(age) > 30000000) { // 30 seconds
            std::cout << "🧹 DEBUG: Removing stale quote - ID: " << it->second.order_id << std::endl;
            it = active_quotes_.erase(it);
        } else {
            ++it;
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
//     std::cout << "🎯 DEBUG: process_market_data_update called" << std::endl;
    
    if (!is_running_.load() || should_stop_.load() || is_destroying_.load()) {
//         std::cout << "❌ DEBUG: Signal engine not running or stopping" << std::endl;
        return;
    }
    
//     std::cout << "✅ DEBUG: Signal engine is running, processing market data" << std::endl;
    
    // Update current market state
    current_top_of_book_ = top_of_book;
    
    // REAL MARKET DATA PROCESSING: Market-based fills handled in process_trade() method
    // No need for simulation - using actual Coinbase trade data
    
    // Generate trading signals based on market data
    auto signals = generate_trading_signals();
    
    // Log signal generation
    if (!signals.empty()) {
        std::cout << "🎯 SIGNAL ENGINE: Generated " << signals.size() << " trading signals" << std::endl;
    } else {
//         std::cout << "📊 DEBUG: No signals generated" << std::endl;
    }
    
    // Process generated signals
    std::cout << "🔄 DEBUG: Processing " << signals.size() << " signals..." << std::endl;
    
    // FIXED: Prevent duplicate signal processing by tracking processed signal IDs
    static std::unordered_set<uint64_t> processed_signal_ids;
    
    for (size_t i = 0; i < signals.size(); ++i) {
        const auto& signal = signals[i];
        std::cout << "🔄 DEBUG: Processing signal " << (i+1) << "/" << signals.size() 
                  << " - Type: " << (signal.type == SignalType::PLACE_BID ? "PLACE_BID" : 
                                   signal.type == SignalType::PLACE_ASK ? "PLACE_ASK" : 
                                   signal.type == SignalType::CANCEL_BID ? "CANCEL_BID" : "CANCEL_ASK")
                  << " ID: " << signal.order_id
                  << " Price: $" << signal.price << " Qty: " << signal.quantity << std::endl;
        
        // Check if this signal has already been processed
        if (processed_signal_ids.find(signal.order_id) != processed_signal_ids.end()) {
//             std::cout << "⚠️ DEBUG: Signal ID " << signal.order_id << " already processed, skipping" << std::endl;
            continue;
        }
        
        // Mark signal as processed
        processed_signal_ids.insert(signal.order_id);
        
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
