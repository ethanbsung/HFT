#include <gtest/gtest.h>
#include "orderbook_engine.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"

using namespace hft;

class SimpleOrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_manager = &MemoryManager::instance();
        latency_tracker = std::make_unique<LatencyTracker>();
        engine = std::make_unique<OrderBookEngine>(*memory_manager, *latency_tracker, "TEST_SYMBOL");
    }
    
    void TearDown() override {
        engine.reset();
        latency_tracker.reset();
    }
    
    MemoryManager* memory_manager;
    std::unique_ptr<LatencyTracker> latency_tracker;
    std::unique_ptr<OrderBookEngine> engine;
};

TEST_F(SimpleOrderBookTest, BasicMatch) {
    // Create sell order
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
    MatchResult result1 = engine->add_order(sell_order, executions);
    EXPECT_EQ(result1, MatchResult::NO_MATCH);
    EXPECT_TRUE(executions.empty());
    
    // Create buy order
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
    
    executions.clear();
    MatchResult result2 = engine->add_order(buy_order, executions);
    EXPECT_EQ(result2, MatchResult::FULL_FILL);
    EXPECT_EQ(executions.size(), 1);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 