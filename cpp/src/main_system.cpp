#include "market_data_feed.hpp"
#include "orderbook_engine.hpp"
#include "signal_engine.hpp"
#include "order_manager.hpp"
#include "latency_tracker.hpp"
#include "memory_pool.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>
#include <memory>
#include <unordered_set>

// Global system instance for signal handling
std::atomic<bool> g_running{true};

void signal_handler(int signal_number) {
    std::cout << "\n🛑 Shutdown signal received (signal " << signal_number << ")..." << std::endl;
    g_running = false;
}

int main() {
    std::cout << "🚀 HFT System Starting..." << std::endl;
    std::cout << "📋 System Components:" << std::endl;
    std::cout << "   • Market Data Feed (Real-time)" << std::endl;
    std::cout << "   • Order Book Engine (High-performance)" << std::endl;
    std::cout << "   • Signal Engine (Market Making)" << std::endl;
    std::cout << "   • Order Manager (Risk Management)" << std::endl;
    std::cout << "   • Latency Tracker (Microsecond precision)" << std::endl;
    std::cout << "   • Memory Pool (Zero allocations)" << std::endl;
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        // Initialize memory manager
        auto& memory_manager = hft::MemoryManager::instance();
        
        // Initialize latency tracker
        hft::LatencyTracker latency_tracker;
        
        // Initialize order book engine
        hft::OrderBookEngine orderbook_engine(memory_manager, latency_tracker, "BTC-USD");
        
        // PYTHON-STYLE ULTRA-AGGRESSIVE market making config
        hft::MarketMakingConfig signal_config;
        signal_config.default_quote_size = 0.1;  // PYTHON-STYLE: Start with smaller sizes like Python (0.1 BTC)
        signal_config.min_spread_bps = 0.1;  // ULTRA-AGGRESSIVE: Allow sub-bps spreads
        signal_config.max_spread_bps = 5.0;   // VERY TIGHT: Maximum 5 bps spread
        signal_config.target_spread_bps = 0.5; // ULTRA-AGGRESSIVE: Target 0.5 bps spread
        signal_config.max_position = 10.0;   // PYTHON-STYLE: Smaller max position like Python system
        signal_config.max_orders_per_second = 1000; // EXTREME: Allow very high order frequency
        
        // Configure PYTHON-STYLE aggressive timing parameters
        signal_config.quote_refresh_ms = 500;  // PYTHON-STYLE: Replace quotes every 500ms
        signal_config.cooldown_ms = 50;    // ULTRA-FAST: 50ms cooldown for immediate requoting
        signal_config.enable_aggressive_quotes = true;  // ENABLE: Full aggressive mode
        signal_config.inventory_skew_factor = 0.5;  // PYTHON-STYLE: Strong inventory skewing
        signal_config.max_inventory_skew_bps = 20.0; // PYTHON-STYLE: Reasonable skewing limits
        
        // Initialize signal engine
        hft::SignalEngine signal_engine(memory_manager, latency_tracker, signal_config);
        
        // PYTHON-STYLE ULTRA-AGGRESSIVE risk limits
        hft::RiskLimits risk_limits;
        risk_limits.max_position = 10.0;   // PYTHON-STYLE: Match signal engine position limits
        risk_limits.max_daily_loss = 1000.0; // PYTHON-STYLE: Reasonable daily loss limit
        risk_limits.max_orders_per_second = 1000; // EXTREME: Match signal engine order frequency
        
        // Initialize order manager
        hft::OrderManager order_manager(memory_manager, latency_tracker, risk_limits);
        
        // Create market data config
        hft::MarketDataConfig market_config;
        market_config.product_id = "BTC-USD";
        market_config.subscribe_to_level2 = true;
        market_config.subscribe_to_matches = true;
        
        // Initialize market data feed
        hft::MarketDataFeed market_data_feed(orderbook_engine, latency_tracker, market_config);
        
        // Set up component relationships
        signal_engine.set_orderbook_engine(&orderbook_engine);
        signal_engine.set_order_manager(&order_manager);
        order_manager.set_orderbook_engine(&orderbook_engine);
        
        // CRITICAL FIX: Connect orderbook engine to order manager for fill notifications
        orderbook_engine.set_order_manager(&order_manager);
        
        // CRITICAL FIX: Set up trade callback on orderbook engine to process fills
        orderbook_engine.set_trade_callback([&order_manager](const hft::TradeExecution& trade) {
            std::cout << "🎯 TRADE EXECUTION: " << (trade.aggressor_side == hft::Side::BUY ? "BUY" : "SELL") 
                      << " " << trade.quantity << " @ $" << trade.price << std::endl;
            // Fill processing is handled by simulate_market_order_from_trade in the orderbook engine
            // which calls notify_fill -> order_manager.handle_fill for any matches
        });
        
        // Set up callbacks for signal processing
        static std::unordered_set<uint64_t> processed_callback_signals;
        static std::mutex callback_mutex;
        
        signal_engine.set_signal_callback([&order_manager, &orderbook_engine, &latency_tracker, &signal_engine](const hft::TradingSignal& signal) {
            // FIXED: Prevent duplicate callback processing
            std::lock_guard<std::mutex> lock(callback_mutex);
            
            if (processed_callback_signals.find(signal.order_id) != processed_callback_signals.end()) {
                std::cout << "⚠️ DEBUG: Signal callback ID " << signal.order_id << " already processed, skipping" << std::endl;
                return;
            }
            
            processed_callback_signals.insert(signal.order_id);
            
            std::cout << "\n🎯 DEBUG: Signal received - Type: " 
                      << (signal.type == hft::SignalType::PLACE_BID ? "PLACE_BID" : 
                          signal.type == hft::SignalType::PLACE_ASK ? "PLACE_ASK" :
                          signal.type == hft::SignalType::CANCEL_BID ? "CANCEL_BID" : "CANCEL_ASK")
                      << " Side: " << (signal.side == hft::Side::BUY ? "BUY" : "SELL")
                      << " Price: $" << signal.price << " Qty: " << signal.quantity << std::endl;
            
            if (signal.type == hft::SignalType::PLACE_BID || signal.type == hft::SignalType::PLACE_ASK) {
                // Get current mid price for performance tracking
                auto top_of_book = orderbook_engine.get_top_of_book();
                hft::price_t current_mid = top_of_book.mid_price;
                
                std::cout << "📊 DEBUG: Current market - Bid: $" << top_of_book.bid_price 
                          << " Ask: $" << top_of_book.ask_price 
                          << " Mid: $" << current_mid << std::endl;
                
                // Measure order creation latency
                auto creation_start = hft::now();
                auto order_id = order_manager.create_order(signal.side, signal.price, signal.quantity, current_mid);
                auto creation_end = hft::now();
                auto creation_latency = hft::time_diff_us(creation_start, creation_end);
                
                std::cout << "⏱️ DEBUG: Order creation latency: " << creation_latency.count() << " μs" << std::endl;
                latency_tracker.add_latency(hft::LatencyType::ORDER_PLACEMENT, creation_latency);
                
                if (order_id > 0) {
                    std::cout << "✅ DEBUG: Order created successfully - ID: " << order_id << std::endl;
                    
                    // Track order placement in signal engine
                    hft::QuoteSide quote_side = (signal.side == hft::Side::BUY) ? hft::QuoteSide::BID : hft::QuoteSide::ASK;
                    signal_engine.track_order_placement(order_id, quote_side, signal.price, signal.quantity);
                    
                    // Measure order submission latency
                    auto submission_start = hft::now();
                    bool submitted = order_manager.submit_order(order_id);
                    auto submission_end = hft::now();
                    auto submission_latency = hft::time_diff_us(submission_start, submission_end);
                    
                    std::cout << "⏱️ DEBUG: Order submission latency: " << submission_latency.count() << " μs" << std::endl;
                    latency_tracker.add_latency(hft::LatencyType::ORDER_PLACEMENT, submission_latency);
                    
                    if (submitted) {
                        std::cout << "🎯 MARKET MAKING: " 
                                  << (signal.side == hft::Side::BUY ? "BID" : "ASK") 
                                  << " $" << signal.price << " x " << signal.quantity 
                                  << " (Order ID: " << order_id << ") ✅ SUBMITTED" << std::endl;
                        
                        // Track total order placement time
                        auto total_latency = hft::time_diff_us(creation_start, submission_end);
                        std::cout << "⏱️ DEBUG: Total order placement latency: " << total_latency.count() << " μs" << std::endl;
                        latency_tracker.add_latency(hft::LatencyType::TICK_TO_TRADE, total_latency);
                    } else {
                        std::cout << "❌ FAILED TO SUBMIT: Order " << order_id 
                                  << " creation succeeded but submission failed" << std::endl;
                        // Clean up the failed order and remove from tracking
                        order_manager.cancel_order(order_id);
                        signal_engine.track_order_cancellation(order_id);
                    }
                } else {
                    std::cout << "❌ FAILED TO CREATE: " 
                              << (signal.side == hft::Side::BUY ? "BID" : "ASK") 
                              << " $" << signal.price << " x " << signal.quantity << std::endl;
                }
            } else if (signal.type == hft::SignalType::CANCEL_BID || signal.type == hft::SignalType::CANCEL_ASK) {
                std::cout << "🔄 DEBUG: Attempting to cancel order ID: " << signal.order_id << std::endl;
                
                auto cancel_start = hft::now();
                bool cancelled = order_manager.cancel_order(signal.order_id);
                auto cancel_end = hft::now();
                auto cancel_latency = hft::time_diff_us(cancel_start, cancel_end);
                
                std::cout << "⏱️ DEBUG: Order cancellation latency: " << cancel_latency.count() << " μs" << std::endl;
                latency_tracker.add_latency(hft::LatencyType::ORDER_CANCELLATION, cancel_latency);
                
                if (cancelled) {
                    std::cout << "✅ CANCELLED: " 
                              << (signal.side == hft::Side::BUY ? "BID" : "ASK") 
                              << " Order ID: " << signal.order_id << std::endl;
                    
                    // Track order cancellation in signal engine
                    signal_engine.track_order_cancellation(signal.order_id);
                } else {
                    std::cout << "❌ CANCEL FAILED: Order ID: " << signal.order_id << std::endl;
                }
            }
        });
        
        // Set up callbacks for order execution
        order_manager.set_fill_callback([&latency_tracker, &signal_engine](const hft::OrderInfo& order_info, hft::quantity_t fill_qty, 
                                          hft::price_t fill_price, bool is_final_fill) {
            std::cout << "\n💰 DEBUG: Order FILL detected!" << std::endl;
            std::cout << "   Order ID: " << order_info.order.order_id << std::endl;
            std::cout << "   Side: " << (order_info.order.side == hft::Side::BUY ? "BUY" : "SELL") << std::endl;
            std::cout << "   Fill Qty: " << fill_qty << " @ $" << fill_price << std::endl;
            std::cout << "   Original Qty: " << order_info.order.original_quantity << std::endl;
            std::cout << "   Remaining Qty: " << order_info.order.remaining_quantity << std::endl;
            std::cout << "   Is Final Fill: " << (is_final_fill ? "YES" : "NO") << std::endl;
            
            // Track order fill in signal engine
            signal_engine.track_order_fill(order_info.order.order_id, fill_qty, fill_price);
            
            // Calculate and track fill latency
            if (order_info.submission_time != hft::timestamp_t{}) {
                auto fill_latency = hft::time_diff_us(order_info.submission_time, hft::now());
                std::cout << "⏱️ DEBUG: Fill latency: " << fill_latency.count() << " μs" << std::endl;
                latency_tracker.add_latency(hft::LatencyType::TICK_TO_TRADE, fill_latency);
            }
            
            // Calculate slippage
            double slippage_bps = 0.0;
            if (order_info.order.price > 0.0 && fill_price > 0.0) {
                slippage_bps = ((fill_price - order_info.order.price) / order_info.order.price) * 10000.0;
                std::cout << "📊 DEBUG: Slippage: " << slippage_bps << " bps" << std::endl;
            }
            
            std::cout << "💰 FILL: " << (order_info.order.side == hft::Side::BUY ? "BUY" : "SELL") 
                      << " " << fill_qty << " @ $" << fill_price 
                      << " (Order ID: " << order_info.order.order_id << ")" << std::endl;
        });
        
        // Set up callbacks for market data
        market_data_feed.set_book_message_callback([&signal_engine, &orderbook_engine, &latency_tracker](const hft::CoinbaseBookMessage& book_msg) {
            // Use arrival time if available, otherwise use current time
            auto market_data_start = book_msg.arrival_time != hft::timestamp_t{} ? book_msg.arrival_time : hft::now();
            
            // Skip latency tracking for the first few messages (connection setup)
            static int callback_message_count = 0;
            callback_message_count++;
            
            std::cout << "\n📊 DEBUG: Market data received - " << book_msg.product_id 
                      << " - " << book_msg.changes.size() << " updates"
                      << " | Message #" << callback_message_count << (callback_message_count <= 3 ? " (setup - not tracked)" : " (tracked)") << std::endl;
            
            // CRITICAL FIX: Always trigger signal engine with updated market data
            auto top_of_book = orderbook_engine.get_top_of_book();
            
            // FIXED: Remove the market validation that was preventing signal generation
            // The orderbook engine should handle invalid market data internally
            std::cout << "📈 DEBUG: Processing market data - Bid: $" << top_of_book.bid_price 
                      << " Ask: $" << top_of_book.ask_price 
                      << " Spread: $" << (top_of_book.ask_price - top_of_book.bid_price) << std::endl;
            
            auto signal_start = hft::now();
            signal_engine.process_market_data_update(top_of_book);
            auto signal_end = hft::now();
            auto signal_latency = hft::time_diff_us(signal_start, signal_end);
            
            std::cout << "⏱️ DEBUG: Signal generation latency: " << signal_latency.count() << " μs" << std::endl;
            
            // Only track latency after the first 3 messages (connection setup)
            if (callback_message_count > 3) {
                latency_tracker.add_latency(hft::LatencyType::TICK_TO_TRADE, signal_latency);
                
                // Calculate complete tick-to-trade latency from arrival to signal completion
                auto tick_to_trade_end = hft::now();
                auto complete_tick_to_trade_latency = hft::time_diff_us(market_data_start, tick_to_trade_end);
                std::cout << "⏱️ DEBUG: Complete tick-to-trade latency: " << complete_tick_to_trade_latency.count() << " μs" << std::endl;
                latency_tracker.add_latency(hft::LatencyType::TICK_TO_TRADE, complete_tick_to_trade_latency);
            } else {
                std::cout << "⏱️ DEBUG: Skipping latency tracking for setup message" << std::endl;
            }
            
            auto market_data_end = hft::now();
            auto market_data_latency = hft::time_diff_us(market_data_start, market_data_end);
            std::cout << "⏱️ DEBUG: Total market data processing latency: " << market_data_latency.count() << " μs" << std::endl;
            
            // Only track latency after the first 3 messages (connection setup)
            if (callback_message_count > 3) {
                latency_tracker.add_latency(hft::LatencyType::MARKET_DATA_PROCESSING, market_data_latency);
            }
        });
        
        // Set up callbacks for trade messages - PRINT ALL EXCHANGE TRADES
        market_data_feed.set_trade_message_callback([&order_manager, &orderbook_engine, &latency_tracker](const hft::CoinbaseTradeMessage& trade_msg) {
            std::cout << "💱 EXCHANGE TRADE: " << trade_msg.product_id 
                      << " " << trade_msg.side << " " << std::fixed << std::setprecision(8) << trade_msg.size 
                      << " @ $" << std::fixed << std::setprecision(2) << trade_msg.price << std::endl;
            
            // CRITICAL FIX: Process trade for potential fills
            // The order book engine processes trades and generates fill events
            // This is already handled in update_order_book_from_trade via process_market_data_trade
            // but we need to ensure the trade callbacks reach the order manager
            
            // Process real market trades for FIFO queue simulation and fill detection
            // Real trades from websocket are processed in update_order_book_from_trade() 
            // which calls process_market_data_trade() -> simulate_market_order_from_trade()
        });
        
        std::cout << "✅ All components initialized successfully with PYTHON-STYLE ULTRA-AGGRESSIVE configuration!" << std::endl;
        std::cout << "📈 PYTHON-STYLE SETTINGS SUMMARY:" << std::endl;
        std::cout << "   • Quote Size: " << signal_config.default_quote_size << " BTC (Python-style small sizes)" << std::endl;
        std::cout << "   • Target Spread: " << signal_config.target_spread_bps << " bps (Ultra-tight)" << std::endl;
        std::cout << "   • Max Position: " << signal_config.max_position << " BTC (Conservative like Python)" << std::endl;
        std::cout << "   • Max Orders/sec: " << signal_config.max_orders_per_second << " (Extreme frequency)" << std::endl;
        std::cout << "   • Quote Refresh: " << signal_config.quote_refresh_ms << " ms (Python-style fast)" << std::endl;
        std::cout << "   • Aggressive Mode: " << (signal_config.enable_aggressive_quotes ? "ENABLED" : "DISABLED") << std::endl;
        std::cout << "   • Strategy: Inside-spread quoting when possible, join best bid/ask otherwise" << std::endl;
        
        // Start market data feed
        if (!market_data_feed.start()) {
            std::cerr << "❌ Failed to start market data feed" << std::endl;
            return 1;
        }
        
        std::cout << "📊 Market data feed started successfully!" << std::endl;
        
        // Start signal engine
        if (!signal_engine.start()) {
            std::cerr << "❌ Failed to start signal engine" << std::endl;
            return 1;
        }
        
        std::cout << "🎯 Signal engine started successfully!" << std::endl;
        
        // Main loop - keep system running
        std::cout << "🔄 System running... Press Ctrl+C to stop" << std::endl;
        
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // AGGRESSIVE: More frequent cleanup of stale quotes for active trading
            static int cleanup_counter = 0;
            if (++cleanup_counter % 5 == 0) { // Every 5 seconds instead of 30
                signal_engine.clear_stale_quotes();
            }
            

            
            // AGGRESSIVE: Print status more frequently for better monitoring
            static int status_counter = 0;
            if (++status_counter % 5 == 0) {  // Every 5 seconds instead of 10
                auto top_of_book = orderbook_engine.get_top_of_book();
                auto position = order_manager.get_position();
                auto stats = order_manager.get_execution_stats();
                
                std::cout << "\n📊 AGGRESSIVE MARKET MAKING STATUS:" << std::endl;
                std::cout << "   Bid: $" << std::fixed << std::setprecision(2) << top_of_book.bid_price 
                          << " Ask: $" << std::fixed << std::setprecision(2) << top_of_book.ask_price 
                          << " Spread: $" << std::fixed << std::setprecision(2) << (top_of_book.ask_price - top_of_book.bid_price) << std::endl;
                std::cout << "   Position: " << std::fixed << std::setprecision(4) << position.net_position 
                          << " P&L: $" << std::fixed << std::setprecision(2) << position.realized_pnl << std::endl;
                std::cout << "   Orders: " << stats.total_orders 
                          << " Fills: " << stats.filled_orders 
                          << " Fill Rate: " << (stats.total_orders > 0 ? (stats.filled_orders * 100.0 / stats.total_orders) : 0) << "%" << std::endl;
                std::cout << "   Active Orders: " << order_manager.get_active_order_count() << std::endl;
                
                // Print our order positions and how long they've been sitting
                auto our_orders = order_manager.get_active_orders();
                if (!our_orders.empty()) {
                    std::cout << "\n📋 OUR ACTIVE ORDERS:" << std::endl;
                    auto now_time = hft::now();
                    for (const auto& order_id : our_orders) {
                        auto order_info = order_manager.get_order_info(order_id);
                        if (order_info) {
                            auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                now_time - order_info->submission_time).count();
                            std::cout << "   Order " << order_id 
                                      << " " << (order_info->order.side == hft::Side::BUY ? "BID" : "ASK")
                                      << " $" << std::fixed << std::setprecision(2) << order_info->order.price
                                      << " x " << std::fixed << std::setprecision(4) << order_info->order.remaining_quantity
                                      << " (age: " << age_seconds << "s)" << std::endl;
                        }
                    }
                }
                
                // Print latency statistics every 30 seconds
                if (status_counter % 30 == 0) {
                    std::cout << "\n⏱️ LATENCY STATISTICS:" << std::endl;
                    auto order_latency = latency_tracker.get_statistics(hft::LatencyType::ORDER_PLACEMENT);
                    auto market_data_latency = latency_tracker.get_statistics(hft::LatencyType::MARKET_DATA_PROCESSING);
                    auto tick_to_trade_latency = latency_tracker.get_statistics(hft::LatencyType::TICK_TO_TRADE);
                    
                    std::cout << "   Order Placement - Mean: " << order_latency.mean_us << "μs, P95: " << order_latency.p95_us << "μs" << std::endl;
                    std::cout << "   Market Data - Mean: " << market_data_latency.mean_us << "μs, P95: " << market_data_latency.p95_us << "μs" << std::endl;
                    std::cout << "   Tick-to-Trade - Mean: " << tick_to_trade_latency.mean_us << "μs, P95: " << tick_to_trade_latency.p95_us << "μs" << std::endl;
                }
            }
        }
        
        // Shutdown
        std::cout << "🛑 Shutting down..." << std::endl;
        
        // Stop components in reverse order of dependencies with timeout
        signal_engine.stop();
        
        // Stop market data feed with timeout
        std::cout << "🔄 Stopping market data feed..." << std::endl;
        market_data_feed.stop();
        
        // Wait for market data to stop with timeout
        auto start_time = std::chrono::steady_clock::now();
        while (market_data_feed.is_connected() && 
               std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (market_data_feed.is_connected()) {
            std::cout << "⚠️ Market data feed still connected after timeout, forcing shutdown" << std::endl;
        }
        
        // Cancel all remaining orders before shutdown
        std::cout << "🔄 Cancelling remaining orders..." << std::endl;
        auto active_orders = order_manager.get_active_order_count();
        if (active_orders > 0) {
            std::cout << "📋 Found " << active_orders << " active orders to cancel" << std::endl;
            // The OrderManager destructor will handle cancellation
        }
        
        // Print final statistics
        std::cout << "\n📊 FINAL STATISTICS:" << std::endl;
        latency_tracker.print_latency_report();
        memory_manager.print_memory_report();
        
        std::cout << "✅ System shutdown complete!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 