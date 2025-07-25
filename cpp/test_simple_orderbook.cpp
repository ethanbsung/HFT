#include <iostream>
#include "orderbook_engine.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"

using namespace hft;

int main() {
    std::cout << "Testing simple OrderBook matching..." << std::endl;
    
    // Initialize dependencies
    auto& memory_manager = MemoryManager::instance();
    LatencyTracker latency_tracker;
    
    // Create order book
    OrderBookEngine engine(memory_manager, latency_tracker, "TEST");
    
    std::cout << "1. Creating sell order..." << std::endl;
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
    MatchResult result1 = engine.add_order(sell_order, executions);
    std::cout << "Sell order result: " << static_cast<int>(result1) << std::endl;
    std::cout << "Executions: " << executions.size() << std::endl;
    
    std::cout << "2. Creating buy order..." << std::endl;
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
    
    std::cout << "About to call add_order for buy order..." << std::endl;
    executions.clear();
    MatchResult result2 = engine.add_order(buy_order, executions);
    std::cout << "Buy order result: " << static_cast<int>(result2) << std::endl;
    std::cout << "Executions: " << executions.size() << std::endl;
    
    std::cout << "Test completed successfully!" << std::endl;
    return 0;
} 