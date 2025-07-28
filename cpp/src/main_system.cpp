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

// Global system instance for signal handling
std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n🛑 Shutdown signal received..." << std::endl;
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
        
        // Create market making config
        hft::MarketMakingConfig signal_config;
        signal_config.default_quote_size = 10.0;
        signal_config.min_spread_bps = 5.0;
        signal_config.max_spread_bps = 50.0;
        signal_config.target_spread_bps = 15.0;
        signal_config.max_position = 100.0;
        signal_config.max_orders_per_second = 100;
        
        // Initialize signal engine
        hft::SignalEngine signal_engine(memory_manager, latency_tracker, signal_config);
        
        // Create risk limits
        hft::RiskLimits risk_limits;
        risk_limits.max_position = 100.0;
        risk_limits.max_daily_loss = 1000.0;
        risk_limits.max_orders_per_second = 100;
        
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
        
        // Set up callbacks for signal processing
        signal_engine.set_signal_callback([&order_manager](const hft::TradingSignal& signal) {
            if (signal.type == hft::SignalType::PLACE_BID || signal.type == hft::SignalType::PLACE_ASK) {
                auto order_id = order_manager.create_order(signal.side, signal.price, signal.quantity);
                if (order_id > 0) {
                    std::cout << "🎯 MARKET MAKING: " 
                              << (signal.side == hft::Side::BUY ? "BID" : "ASK") 
                              << " $" << signal.price << " x " << signal.quantity 
                              << " (Order ID: " << order_id << ")" << std::endl;
                }
            } else if (signal.type == hft::SignalType::CANCEL_BID || signal.type == hft::SignalType::CANCEL_ASK) {
                std::cout << "❌ CANCEL: " 
                          << (signal.side == hft::Side::BUY ? "BID" : "ASK") 
                          << " Order ID: " << signal.order_id << std::endl;
            }
        });
        
        // Set up callbacks for order execution
        order_manager.set_fill_callback([](const hft::OrderInfo& order_info, hft::quantity_t fill_qty, 
                                          hft::price_t fill_price, bool is_final_fill) {
            std::cout << "💰 FILL: " << (order_info.order.side == hft::Side::BUY ? "BUY" : "SELL") 
                      << " " << fill_qty << " @ $" << fill_price 
                      << " (Order ID: " << order_info.order.order_id << ")" << std::endl;
        });
        
        // Set up callbacks for market data
        market_data_feed.set_book_message_callback([&signal_engine, &orderbook_engine](const hft::CoinbaseBookMessage& book_msg) {
            std::cout << "📊 MARKET DATA: " << book_msg.product_id 
                      << " - " << book_msg.changes.size() << " updates" << std::endl;
            
            // Trigger signal engine with updated market data
            auto top_of_book = orderbook_engine.get_top_of_book();
            if (top_of_book.bid_price > 0.0 && top_of_book.ask_price > 0.0) {
                signal_engine.process_market_data_update(top_of_book);
            }
        });
        
        // Set up callbacks for trade messages
        market_data_feed.set_trade_message_callback([](const hft::CoinbaseTradeMessage& trade_msg) {
            std::cout << "💱 TRADE: " << trade_msg.product_id 
                      << " " << trade_msg.side << " " << trade_msg.size 
                      << " @ $" << trade_msg.price << std::endl;
        });
        
        std::cout << "✅ All components initialized successfully!" << std::endl;
        
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
            
            // Print periodic status
            static int status_counter = 0;
            if (++status_counter % 10 == 0) {
                auto top_of_book = orderbook_engine.get_top_of_book();
                auto position = order_manager.get_position();
                auto stats = order_manager.get_execution_stats();
                
                std::cout << "\n📊 MARKET MAKING STATUS:" << std::endl;
                std::cout << "   Bid: $" << top_of_book.bid_price 
                          << " Ask: $" << top_of_book.ask_price 
                          << " Spread: $" << top_of_book.spread << std::endl;
                std::cout << "   Position: " << position.net_position 
                          << " P&L: $" << position.realized_pnl << std::endl;
                std::cout << "   Orders: " << stats.total_orders 
                          << " Fills: " << stats.filled_orders 
                          << " Fill Rate: " << (stats.total_orders > 0 ? (stats.filled_orders * 100.0 / stats.total_orders) : 0) << "%" << std::endl;
                std::cout << "   Active Orders: " << order_manager.get_active_order_count() << std::endl;
            }
        }
        
        // Shutdown
        std::cout << "🛑 Shutting down..." << std::endl;
        
        signal_engine.stop();
        market_data_feed.stop();
        
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