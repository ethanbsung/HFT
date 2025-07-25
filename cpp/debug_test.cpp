#include <iostream>
#include <chrono>
#include "orderbook_engine.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"

using namespace hft;

void debug_log(const std::string& message) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    std::cout << "[" << millis << "] " << message << std::endl;
    std::cout.flush();
}

int main() {
    debug_log("Starting debug test...");
    
    // Initialize dependencies
    debug_log("Getting memory manager instance...");
    auto& memory_manager = MemoryManager::instance();
    debug_log("Creating latency tracker...");
    LatencyTracker latency_tracker;
    
    debug_log("Creating order book engine...");
    OrderBookEngine engine(memory_manager, latency_tracker, "DEBUG_TEST");
    
    // Set up callbacks with logging
    debug_log("Setting up callbacks...");
    engine.set_book_update_callback([](const TopOfBook& tob) {
        debug_log("Book update callback called");
    });
    
    engine.set_trade_callback([](const TradeExecution& trade) {
        debug_log("Trade callback called");
    });
    
    engine.set_depth_update_callback([](const MarketDepth& depth) {
        debug_log("Depth update callback called");
    });
    
    debug_log("Creating sell order...");
    Order sell_order;
    sell_order.order_id = 1;
    sell_order.side = Side::SELL;
    sell_order.price = 100.0;
    sell_order.original_quantity = 10.0;
    sell_order.remaining_quantity = 10.0;
    sell_order.quantity = 10.0;
    sell_order.status = OrderStatus::PENDING;
    sell_order.entry_time = now();
    sell_order.last_update_time = sell_order.entry_time;
    
    std::vector<TradeExecution> executions;
    debug_log("Adding sell order...");
    MatchResult result1 = engine.add_order(sell_order, executions);
    debug_log("Sell order added, result: " + std::to_string(static_cast<int>(result1)));
    
    debug_log("Creating buy order...");
    Order buy_order;
    buy_order.order_id = 2;
    buy_order.side = Side::BUY;
    buy_order.price = 100.0;
    buy_order.original_quantity = 10.0;
    buy_order.remaining_quantity = 10.0;
    buy_order.quantity = 10.0;
    buy_order.status = OrderStatus::PENDING;
    buy_order.entry_time = now();
    buy_order.last_update_time = buy_order.entry_time;
    
    debug_log("About to add buy order - THIS IS WHERE IT MIGHT HANG");
    executions.clear();
    MatchResult result2 = engine.add_order(buy_order, executions);
    debug_log("Buy order added, result: " + std::to_string(static_cast<int>(result2)));
    
    debug_log("Test completed successfully!");
    return 0;
} 