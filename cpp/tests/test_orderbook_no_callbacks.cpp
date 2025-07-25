#include <gtest/gtest.h>
#include "orderbook_engine.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"

using namespace hft;

class OrderBookEngineTestNoCallbacks : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize dependencies
        memory_manager = &MemoryManager::instance();
        latency_tracker = std::make_unique<LatencyTracker>();
        
        // Create order book engine
        engine = std::make_unique<OrderBookEngine>(*memory_manager, *latency_tracker, "TEST_SYMBOL");
        
        // Reset counters
        next_order_id = 1;
        
        // DO NOT SET UP CALLBACKS - this might be causing the infinite loop
    }
    
    void TearDown() override {
        // Clean up
        engine.reset();
        latency_tracker.reset();
    }
    
    // Helper methods for creating orders
    Order create_order(Side side, price_t price, quantity_t quantity) {
        Order order;
        order.order_id = next_order_id++;
        order.side = side;
        order.price = price;
        order.original_quantity = quantity;
        order.remaining_quantity = quantity;
        order.quantity = quantity;  // Set current quantity field
        order.status = OrderStatus::PENDING;
        order.entry_time = now();
        order.last_update_time = order.entry_time;
        return order;
    }
    
    Order create_buy_order(price_t price, quantity_t quantity) {
        return create_order(Side::BUY, price, quantity);
    }
    
    Order create_sell_order(price_t price, quantity_t quantity) {
        return create_order(Side::SELL, price, quantity);
    }
    
    // Test data
    MemoryManager* memory_manager;
    std::unique_ptr<LatencyTracker> latency_tracker;
    std::unique_ptr<OrderBookEngine> engine;
    
    uint64_t next_order_id;
};

TEST_F(OrderBookEngineTestNoCallbacks, SimpleMatchFullFill) {
    // Add a sell order first
    auto sell_order = create_sell_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    engine->add_order(sell_order, executions);
    
    // Add matching buy order (same price and quantity)
    auto buy_order = create_buy_order(100.0, 10.0);
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::FULL_FILL);
    EXPECT_EQ(executions.size(), 1);
    
    auto& trade = executions[0];
    EXPECT_EQ(trade.aggressor_order_id, buy_order.order_id);
    EXPECT_EQ(trade.passive_order_id, sell_order.order_id);
    EXPECT_EQ(trade.price, 100.0);  // Trade at passive order price
    EXPECT_EQ(trade.quantity, 10.0);
    EXPECT_EQ(trade.aggressor_side, Side::BUY);
    
    // Book should be empty after full fill
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 0.0);
    EXPECT_EQ(tob.ask_price, 0.0);
}

TEST_F(OrderBookEngineTestNoCallbacks, AddSingleBuyOrder) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    MatchResult result = engine->add_order(order, executions);
    
    EXPECT_EQ(result, MatchResult::NO_MATCH);
    EXPECT_TRUE(executions.empty());
    
    // Check top of book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 100.0);
    EXPECT_EQ(tob.bid_quantity, 10.0);
    EXPECT_EQ(tob.ask_price, 0.0);  // No asks
    EXPECT_EQ(tob.ask_quantity, 0.0);
}

TEST_F(OrderBookEngineTestNoCallbacks, MultiLevelMatching) {
    // Add multiple sell orders at different prices
    auto sell1 = create_sell_order(100.0, 5.0);
    auto sell2 = create_sell_order(101.0, 10.0);
    auto sell3 = create_sell_order(102.0, 15.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(sell1, executions);
    engine->add_order(sell2, executions);
    engine->add_order(sell3, executions);
    
    // Add large buy order that should match multiple levels
    auto buy_order = create_buy_order(102.0, 20.0);  // Should match all of sell1 and sell2
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::PARTIAL_FILL);
    EXPECT_EQ(executions.size(), 2);  // Should have 2 trades
    
    // First trade should be at 100.0 for 5 shares
    EXPECT_EQ(executions[0].price, 100.0);
    EXPECT_EQ(executions[0].quantity, 5.0);
    
    // Second trade should be at 101.0 for 10 shares
    EXPECT_EQ(executions[1].price, 101.0);
    EXPECT_EQ(executions[1].quantity, 10.0);
    
    // Remaining 5 from buy order should be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 102.0);
    EXPECT_EQ(tob.bid_quantity, 5.0);
    EXPECT_EQ(tob.ask_price, 102.0);  // Only sell3 remains
    EXPECT_EQ(tob.ask_quantity, 15.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 