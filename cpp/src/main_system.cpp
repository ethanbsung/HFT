#include "market_data_feed.hpp"
#include "orderbook_engine.hpp"
#include "signal_engine.hpp"
#include "order_manager.hpp"
#include "latency_tracker.hpp"
#include "memory_pool.hpp"
#include "log_control.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>
#include <memory>
#include <mutex>
#include <iomanip>

// Global system instance for signal handling
std::atomic<bool> g_running{true};

void signal_handler(int signal_number) {
    std::cout << "\nShutdown signal received (signal " << signal_number << ")..." << std::endl;
    g_running = false;
}

int main() {
    std::cout << "HFT System Starting..." << std::endl;
    std::cout << "System Components:" << std::endl;
    std::cout << "   - Market Data Feed (Real-time)" << std::endl;
    std::cout << "   - Order Book Engine (High-performance)" << std::endl;
    std::cout << "   - Signal Engine (Market Making)" << std::endl;
    std::cout << "   - Order Manager (Risk Management)" << std::endl;
    std::cout << "   - Latency Tracker (Microsecond precision)" << std::endl;
    std::cout << "   - Memory Pool (Zero allocations)" << std::endl;
    
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

        hft::MarketMakingConfig signal_config;
        signal_config.default_quote_size = 0.1;  // Start with smaller sizes like Python (0.1 BTC)
        signal_config.min_spread_bps = 0.1;  // Allow sub-bps spreads
        signal_config.max_spread_bps = 5.0;   // Maximum 5 bps spread
        signal_config.target_spread_bps = 0.5; // Target 0.5 bps spread
        signal_config.max_position = 10.0;   // Smaller max position like Python system
        signal_config.max_orders_per_second = 1000; // Allow very high order frequency
        
        signal_config.quote_refresh_ms = 500;  // Replace quotes every 500ms
        signal_config.cooldown_ms = 50;    // 50ms cooldown for immediate requoting
        signal_config.enable_aggressive_quotes = true;  // Full aggressive mode
        signal_config.inventory_skew_factor = 0.5;  // Strong inventory skewing
        signal_config.max_inventory_skew_bps = 20.0; // Reasonable skewing limits
        
        // Initialize signal engine
        hft::SignalEngine signal_engine(memory_manager, latency_tracker, signal_config);
        
        hft::RiskLimits risk_limits;
        risk_limits.max_position = 10.0;   // Match signal engine position limits
        risk_limits.max_daily_loss = 1000.0; // Reasonable daily loss limit
        risk_limits.max_orders_per_second = 1000; // Match signal engine order frequency
        
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
        
        // Connect orderbook engine to order manager for fill notifications
        orderbook_engine.set_order_manager(&order_manager);
        
        // Set up trade callback on orderbook engine to process fills
        orderbook_engine.set_trade_callback([&order_manager](const hft::TradeExecution& trade) {
            (void)order_manager;
            (void)trade;
            // Fill processing is handled by simulate_market_order_from_trade in the orderbook engine
            // which calls notify_fill -> order_manager.handle_fill for any matches
        });

        // Ensure signal generation is serialized between market data callbacks and timer refresh.
        std::mutex signal_processing_mutex;
        
        // Set up callbacks for signal processing
        signal_engine.set_signal_callback([&order_manager, &orderbook_engine, &latency_tracker, &signal_engine](const hft::TradingSignal& signal) {
            hft::ScopedCoutSilencer silence_hot_path(!hft::kEnableHotPathLogging);
            // Signal: " << (signal.side == hft::Side::BUY ? "BID" : "ASK") << " $" << signal.price << " x " << signal.quantity
            
            if (signal.type == hft::SignalType::PLACE_BID || signal.type == hft::SignalType::PLACE_ASK) {
                // Get current mid price for performance tracking
                auto top_of_book = orderbook_engine.get_top_of_book();
                hft::price_t current_mid = top_of_book.mid_price;
                
                // Market: Bid $" << top_of_book.bid_price << " Ask $" << top_of_book.ask_price
                
                // Measure order creation latency
                auto creation_start = hft::now();
                auto order_id = order_manager.create_order(signal.side, signal.price, signal.quantity, current_mid);
                auto creation_end = hft::now();
                auto creation_latency = hft::time_diff_us(creation_start, creation_end);
                latency_tracker.add_latency(hft::LatencyType::ORDER_PLACEMENT, creation_latency);
                
                if (order_id > 0) {
//                     std::cout << " DEBUG: Order created successfully - ID: " << order_id << std::endl;
                    
                    // Track order placement in signal engine
                    hft::QuoteSide quote_side = (signal.side == hft::Side::BUY) ? hft::QuoteSide::BID : hft::QuoteSide::ASK;
                    signal_engine.track_order_placement(order_id, quote_side, signal.price, signal.quantity);
                    
                    // Measure order submission latency
                    auto submission_start = hft::now();
                    bool submitted = order_manager.submit_order(order_id);
                    auto submission_end = hft::now();
                    auto submission_latency = hft::time_diff_us(submission_start, submission_end);
                    latency_tracker.add_latency(hft::LatencyType::ORDER_PLACEMENT, submission_latency);

                    if (!submitted) {
                        // Clean up the failed order and remove from tracking
                        order_manager.cancel_order(order_id);
                        signal_engine.track_order_cancellation(order_id);
                    }
                }
            } else if (signal.type == hft::SignalType::CANCEL_BID || signal.type == hft::SignalType::CANCEL_ASK) {
                auto cancel_start = hft::now();
                bool cancelled = order_manager.cancel_order(signal.order_id);
                auto cancel_end = hft::now();
                auto cancel_latency = hft::time_diff_us(cancel_start, cancel_end);

                latency_tracker.add_latency(hft::LatencyType::ORDER_CANCELLATION, cancel_latency);
                
                if (cancelled) {
                    // Track order cancellation in signal engine
                    signal_engine.track_order_cancellation(signal.order_id);
                }
            }
        });
        
        // Set up callbacks for order execution
        order_manager.set_fill_callback([&latency_tracker, &signal_engine](const hft::OrderInfo& order_info, hft::quantity_t fill_qty, 
                                          hft::price_t fill_price, bool is_final_fill) {
            hft::ScopedCoutSilencer silence_hot_path(!hft::kEnableHotPathLogging);
            (void)is_final_fill;
            
            // Track order fill in signal engine
            signal_engine.track_order_fill(order_info.order.order_id, fill_qty, fill_price);
            
            // Calculate and track fill latency
            if (order_info.submission_time != hft::timestamp_t{}) {
                auto fill_latency = hft::time_diff_us(order_info.submission_time, hft::now());
                latency_tracker.add_latency(hft::LatencyType::TRADE_EXECUTION_PROCESSING, fill_latency);
            }
        });
        
        // Set up callbacks for market data
        market_data_feed.set_book_message_callback([&signal_engine, &orderbook_engine, &latency_tracker, &signal_processing_mutex](const hft::CoinbaseBookMessage& book_msg) {
            hft::ScopedCoutSilencer silence_hot_path(!hft::kEnableHotPathLogging);
            // Use arrival time if available, otherwise use current time
            auto market_data_start = book_msg.arrival_time != hft::timestamp_t{} ? book_msg.arrival_time : hft::now();
            
            // Skip latency tracking for the first few messages (connection setup)
            static int callback_message_count = 0;
            callback_message_count++;
            
            // Market data: " << book_msg.changes.size() << " updates
            
            // Always trigger signal engine with updated market data
            auto top_of_book = orderbook_engine.get_top_of_book();
            
            // Remove the market validation that was preventing signal generation
            // The orderbook engine should handle invalid market data internally
            // Processing: Bid $" << top_of_book.bid_price << " Ask $" << top_of_book.ask_price
            
            auto signal_start = hft::now();
            {
                std::lock_guard<std::mutex> lock(signal_processing_mutex);
                signal_engine.process_market_data_update(top_of_book);
            }
            auto signal_end = hft::now();
            (void)hft::time_diff_us(signal_start, signal_end);
            
            // Only track latency after the first 3 messages (connection setup)
            if (callback_message_count > 3) {
                // Calculate complete tick-to-trade latency from arrival to signal completion
                auto tick_to_trade_end = hft::now();
                auto complete_tick_to_trade_latency = hft::time_diff_us(market_data_start, tick_to_trade_end);
                latency_tracker.add_latency(hft::LatencyType::TICK_TO_TRADE, complete_tick_to_trade_latency);
            }
            
            auto market_data_end = hft::now();
            auto market_data_latency = hft::time_diff_us(market_data_start, market_data_end);
            
            // Only track latency after the first 3 messages (connection setup)
            if (callback_message_count > 3) {
                latency_tracker.add_latency(hft::LatencyType::MARKET_DATA_PROCESSING, market_data_latency);
            }
        });
        
        // Set up callbacks for trade messages - PRINT ALL EXCHANGE TRADES
        market_data_feed.set_trade_message_callback([&order_manager, &orderbook_engine, &latency_tracker](const hft::CoinbaseTradeMessage& trade_msg) {
            hft::ScopedCoutSilencer silence_hot_path(!hft::kEnableHotPathLogging);
            (void)trade_msg;
            (void)order_manager;
            (void)orderbook_engine;
            (void)latency_tracker;
            
            // Process trade for potential fills
            // The order book engine processes trades and generates fill events
            // This is already handled in update_order_book_from_trade via process_market_data_trade
            // but we need to ensure the trade callbacks reach the order manager
            
            // Process real market trades for FIFO queue simulation and fill detection
            // Real trades from websocket are processed in update_order_book_from_trade() 
            // which calls process_market_data_trade() -> simulate_market_order_from_trade()
        });
        
        std::cout << "All components initialized successfully with aggressive configuration." << std::endl;
        std::cout << "Aggressive settings summary:" << std::endl;
        std::cout << "   - Quote Size: " << signal_config.default_quote_size << " BTC" << std::endl;
        std::cout << "   - Target Spread: " << signal_config.target_spread_bps << " bps" << std::endl;
        std::cout << "   - Max Position: " << signal_config.max_position << " BTC" << std::endl;
        std::cout << "   - Max Orders/sec: " << signal_config.max_orders_per_second << std::endl;
        std::cout << "   - Quote Refresh: " << signal_config.quote_refresh_ms << " ms" << std::endl;
        std::cout << "   - Aggressive Mode: " << (signal_config.enable_aggressive_quotes ? "ENABLED" : "DISABLED") << std::endl;
        std::cout << "   - Strategy: Inside-spread quoting when possible, join best bid/ask otherwise" << std::endl;
        
        // Start market data feed
        if (!market_data_feed.start()) {
            std::cerr << "Failed to start market data feed" << std::endl;
            return 1;
        }
        
        std::cout << "Market data feed started successfully." << std::endl;
        
        // Start signal engine
        if (!signal_engine.start()) {
            std::cerr << "Failed to start signal engine" << std::endl;
            return 1;
        }
        
        std::cout << "Signal engine started successfully." << std::endl;
        
        // Main loop - keep system running
        std::cout << "System running... Press Ctrl+C to stop" << std::endl;
        
        const auto loop_sleep = std::chrono::milliseconds(100);
        const auto quote_refresh_interval = std::chrono::milliseconds(signal_config.quote_refresh_ms);
        const auto status_interval = std::chrono::seconds(5);
        const auto latency_report_interval = std::chrono::seconds(30);
        const auto stale_cleanup_interval = std::chrono::seconds(30);

        auto last_quote_refresh = hft::now();
        auto last_status_report = hft::now();
        auto last_latency_report = hft::now();
        auto last_stale_cleanup = hft::now();

        while (g_running) {
            std::this_thread::sleep_for(loop_sleep);
            auto now_time = hft::now();

            // Requote on a fixed timer even when market data callbacks are sparse.
            if (now_time - last_quote_refresh >= quote_refresh_interval) {
                auto top_of_book = orderbook_engine.get_top_of_book();
                {
                    std::lock_guard<std::mutex> lock(signal_processing_mutex);
                    signal_engine.process_market_data_update(top_of_book);
                }
                last_quote_refresh = now_time;
            }

            // Forced cleanup of stuck quote state.
            if (now_time - last_stale_cleanup >= stale_cleanup_interval) {
                signal_engine.clear_stale_quotes();
                last_stale_cleanup = now_time;
            }

            if (now_time - last_status_report >= status_interval) {
                auto top_of_book = orderbook_engine.get_top_of_book();
                auto position = order_manager.get_position();
                auto stats = order_manager.get_execution_stats();
                
                std::cout << "\nMARKET MAKING STATUS:" << std::endl;
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
                    std::cout << "\nOUR ACTIVE ORDERS:" << std::endl;
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
                
                if (now_time - last_latency_report >= latency_report_interval) {
                    std::cout << "\nLATENCY STATISTICS:" << std::endl;
                    auto order_latency = latency_tracker.get_statistics(hft::LatencyType::ORDER_PLACEMENT);
                    auto market_data_latency = latency_tracker.get_statistics(hft::LatencyType::MARKET_DATA_PROCESSING);
                    auto tick_to_trade_latency = latency_tracker.get_statistics(hft::LatencyType::TICK_TO_TRADE);
                    
                    std::cout << "   Order Placement - Mean: " << order_latency.mean_us << "us, P95: " << order_latency.p95_us << "us" << std::endl;
                    std::cout << "   Market Data - Mean: " << market_data_latency.mean_us << "us, P95: " << market_data_latency.p95_us << "us" << std::endl;
                    std::cout << "   Tick-to-Trade - Mean: " << tick_to_trade_latency.mean_us << "us, P95: " << tick_to_trade_latency.p95_us << "us" << std::endl;
                    last_latency_report = now_time;
                }

                last_status_report = now_time;
            }
        }
        
        // Shutdown
        std::cout << "Shutting down..." << std::endl;
        
        // Stop components in reverse order of dependencies with timeout
        signal_engine.stop();
        
        // Stop market data feed with timeout
        std::cout << "Stopping market data feed..." << std::endl;
        market_data_feed.stop();
        
        // Wait for market data to stop with timeout
        auto start_time = std::chrono::steady_clock::now();
        while (market_data_feed.is_connected() && 
               std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (market_data_feed.is_connected()) {
            std::cout << "Warning: market data feed still connected after timeout, forcing shutdown" << std::endl;
        }
        
        // Cancel all remaining orders before shutdown
        std::cout << "Cancelling remaining orders..." << std::endl;
        auto active_orders = order_manager.get_active_order_count();
        if (active_orders > 0) {
            std::cout << "Found " << active_orders << " active orders to cancel" << std::endl;
            // The OrderManager destructor will handle cancellation
        }
        
        // Print final statistics
        std::cout << "\nFINAL STATISTICS:" << std::endl;
        latency_tracker.print_latency_report();
        memory_manager.print_memory_report();
        
        std::cout << "System shutdown complete." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 
