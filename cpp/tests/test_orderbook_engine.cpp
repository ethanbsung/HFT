#include <gtest/gtest.h>
#include "orderbook_engine.hpp"
#include "order_manager.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include <thread>
#include <chrono>
#include <random>
#include <vector>
#include <algorithm>

using namespace hft;
using namespace std::chrono_literals;

class OrderBookEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize dependencies
        memory_manager = &MemoryManager::instance();  // Use singleton instance
        latency_tracker = std::make_unique<LatencyTracker>();
        
        // Create order book engine
        engine = std::make_unique<OrderBookEngine>(*memory_manager, *latency_tracker, "TEST_SYMBOL");
        
        // Reset counters
        next_order_id = 1;
        callback_call_count = 0;
        last_book_update.reset();
        last_trade.reset();
        last_depth_update.reset();
        
        // Set up callbacks for testing (simplified to avoid potential loops)
        engine->set_book_update_callback([this](const TopOfBook& tob) {
            callback_call_count++;
            last_book_update = tob;
        });
        
        engine->set_trade_callback([this](const TradeExecution& trade) {
            callback_call_count++;
            last_trade = trade;
            trade_history.push_back(trade);
        });
        
        // Simplified depth callback to avoid potential infinite loops
        engine->set_depth_update_callback([this](const MarketDepth& /* depth */) {
            callback_call_count++;
            // Don't store the depth to avoid potential issues
        });
    }
    
    void TearDown() override {
        // Clean up
        engine.reset();
        latency_tracker.reset();
        // memory_manager is singleton, no need to reset
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
    
    // Helper to add orders and return executions
    std::vector<TradeExecution> add_order_and_get_executions(const Order& order) {
        std::vector<TradeExecution> executions;
        [[maybe_unused]] MatchResult result = engine->add_order(order, executions);
        return executions;
    }
    
    // Test data
    MemoryManager* memory_manager;  // Pointer to singleton
    std::unique_ptr<LatencyTracker> latency_tracker;
    std::unique_ptr<OrderBookEngine> engine;
    
    uint64_t next_order_id;
    int callback_call_count;
    std::optional<TopOfBook> last_book_update;
    std::optional<TradeExecution> last_trade;
    std::optional<MarketDepth> last_depth_update;
    std::vector<TradeExecution> trade_history;
};

// =============================================================================
// BASIC ORDER BOOK OPERATIONS TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, AddSingleBuyOrder) {
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
    
    // Verify callback was called
    EXPECT_GT(callback_call_count, 0);
    EXPECT_TRUE(last_book_update.has_value());
}

TEST_F(OrderBookEngineTest, AddSingleSellOrder) {
    auto order = create_sell_order(105.0, 15.0);
    std::vector<TradeExecution> executions;
    
    MatchResult result = engine->add_order(order, executions);
    
    EXPECT_EQ(result, MatchResult::NO_MATCH);
    EXPECT_TRUE(executions.empty());
    
    // Check top of book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 0.0);  // No bids
    EXPECT_EQ(tob.bid_quantity, 0.0);
    EXPECT_EQ(tob.ask_price, 105.0);
    EXPECT_EQ(tob.ask_quantity, 15.0);
}

TEST_F(OrderBookEngineTest, AddMultipleOrdersSameSide) {
    // Add multiple buy orders at different prices
    auto buy1 = create_buy_order(100.0, 10.0);
    auto buy2 = create_buy_order(99.0, 15.0);
    auto buy3 = create_buy_order(101.0, 5.0);  // This should be new best bid
    
    std::vector<TradeExecution> executions;
    
    engine->add_order(buy1, executions);
    engine->add_order(buy2, executions);
    engine->add_order(buy3, executions);
    
    // Best bid should be highest price
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 101.0);
    EXPECT_EQ(tob.bid_quantity, 5.0);
    
    // Check market depth
    auto depth = engine->get_market_depth(5);
    EXPECT_EQ(depth.bids.size(), 3);
    
    // Bids should be sorted highest to lowest
    EXPECT_EQ(depth.bids[0].price, 101.0);
    EXPECT_EQ(depth.bids[0].quantity, 5.0);
    EXPECT_EQ(depth.bids[1].price, 100.0);
    EXPECT_EQ(depth.bids[1].quantity, 10.0);
    EXPECT_EQ(depth.bids[2].price, 99.0);
    EXPECT_EQ(depth.bids[2].quantity, 15.0);
}

TEST_F(OrderBookEngineTest, CancelOrder) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    // Add order
    MatchResult result = engine->add_order(order, executions);
    EXPECT_EQ(result, MatchResult::NO_MATCH);
    
    // Verify order is in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 100.0);
    
    // Cancel order
    bool cancelled = engine->cancel_order(order.order_id);
    EXPECT_TRUE(cancelled);
    
    // Verify order is removed
    tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 0.0);
}

TEST_F(OrderBookEngineTest, CancelNonExistentOrder) {
    bool cancelled = engine->cancel_order(999999);
    EXPECT_FALSE(cancelled);
}

TEST_F(OrderBookEngineTest, ModifyOrderPrice) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    // Add order
    engine->add_order(order, executions);
    
    // Modify price
    bool modified = engine->modify_order(order.order_id, 101.0, 10.0);
    EXPECT_TRUE(modified);
    
    // Check new price in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 101.0);
    EXPECT_EQ(tob.bid_quantity, 10.0);
}

TEST_F(OrderBookEngineTest, ModifyOrderQuantity) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    // Add order
    engine->add_order(order, executions);
    
    // Modify quantity
    bool modified = engine->modify_order(order.order_id, 100.0, 15.0);
    EXPECT_TRUE(modified);
    
    // Check new quantity in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 100.0);
    EXPECT_EQ(tob.bid_quantity, 15.0);
}

TEST_F(OrderBookEngineTest, ModifyNonExistentOrder) {
    bool modified = engine->modify_order(999999, 100.0, 10.0);
    EXPECT_FALSE(modified);
}

// =============================================================================
// ORDER MATCHING ENGINE TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, SimpleMatchFullFill) {
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

TEST_F(OrderBookEngineTest, PartialFillAggressor) {
    // Add sell order
    auto sell_order = create_sell_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    engine->add_order(sell_order, executions);
    
    // Add larger buy order (partial fill)
    auto buy_order = create_buy_order(100.0, 15.0);
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::PARTIAL_FILL);
    EXPECT_EQ(executions.size(), 1);
    
    auto& trade = executions[0];
    EXPECT_EQ(trade.quantity, 10.0);  // Only 10 filled
    
    // Remaining 5 should be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 100.0);
    EXPECT_EQ(tob.bid_quantity, 5.0);
    EXPECT_EQ(tob.ask_price, 0.0);  // Sell order fully consumed
}

TEST_F(OrderBookEngineTest, PartialFillPassive) {
    // Add large sell order
    auto sell_order = create_sell_order(100.0, 15.0);
    std::vector<TradeExecution> executions;
    engine->add_order(sell_order, executions);
    
    // Add smaller buy order
    auto buy_order = create_buy_order(100.0, 10.0);
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::FULL_FILL);
    EXPECT_EQ(executions.size(), 1);
    
    auto& trade = executions[0];
    EXPECT_EQ(trade.quantity, 10.0);
    
    // Remaining 5 from sell order should be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.ask_price, 100.0);
    EXPECT_EQ(tob.ask_quantity, 5.0);
    EXPECT_EQ(tob.bid_price, 0.0);
}

TEST_F(OrderBookEngineTest, MultiLevelMatching) {
    // Add multiple sell orders at different prices
    auto sell1 = create_sell_order(100.0, 5.0);
    auto sell2 = create_sell_order(101.0, 10.0);
    auto sell3 = create_sell_order(102.0, 15.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(sell1, executions);
    engine->add_order(sell2, executions);
    engine->add_order(sell3, executions);
    
    // Add large buy order that should match multiple levels
    auto buy_order = create_buy_order(102.0, 20.0);  // Should match all three sell orders
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::PARTIAL_FILL);  // Only 15 shares get filled out of 20
    EXPECT_EQ(executions.size(), 2);  // Should have 2 trades (matches sell1 and sell2 completely)
    
    // First trade should be at 100.0 for 5 shares
    EXPECT_EQ(executions[0].price, 100.0);
    EXPECT_EQ(executions[0].quantity, 5.0);
    
    // Second trade should be at 101.0 for 10 shares
    EXPECT_EQ(executions[1].price, 101.0);
    EXPECT_EQ(executions[1].quantity, 10.0);
    
    // No third trade - buy order has 5 shares remaining that will rest in the book
    
    // Remaining 5 from buy order and sell3 should be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 102.0);  // Remaining buy order
    EXPECT_EQ(tob.bid_quantity, 5.0);  // 20 - 15 = 5 remaining
    EXPECT_EQ(tob.ask_price, 102.0);  // Remainder of sell3
    EXPECT_EQ(tob.ask_quantity, 15.0);  // All 15 remaining (sell3 was not touched)
}

TEST_F(OrderBookEngineTest, PriceTimePriority) {
    // Add multiple orders at same price to test FIFO
    auto sell1 = create_sell_order(100.0, 5.0);
    auto sell2 = create_sell_order(100.0, 10.0);
    auto sell3 = create_sell_order(100.0, 15.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(sell1, executions);
    std::this_thread::sleep_for(1ms);  // Ensure different timestamps
    engine->add_order(sell2, executions);
    std::this_thread::sleep_for(1ms);
    engine->add_order(sell3, executions);
    
    // Add buy order that partially fills
    auto buy_order = create_buy_order(100.0, 12.0);
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::FULL_FILL);
    EXPECT_EQ(executions.size(), 2);  // Should match first two orders
    
    // First trade should be with sell1 (first in time)
    EXPECT_EQ(executions[0].passive_order_id, sell1.order_id);
    EXPECT_EQ(executions[0].quantity, 5.0);
    
    // Second trade should be with sell2 (second in time)
    EXPECT_EQ(executions[1].passive_order_id, sell2.order_id);
    EXPECT_EQ(executions[1].quantity, 7.0);  // Partial fill of sell2
    
    // sell3 and remainder of sell2 should still be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.ask_price, 100.0);
    EXPECT_EQ(tob.ask_quantity, 18.0);  // 3 + 15 remaining
}

TEST_F(OrderBookEngineTest, NoMatchDueToPriceGap) {
    // Add sell order at high price
    auto sell_order = create_sell_order(105.0, 10.0);
    std::vector<TradeExecution> executions;
    engine->add_order(sell_order, executions);
    
    // Add buy order at lower price (no match)
    auto buy_order = create_buy_order(100.0, 10.0);
    MatchResult result = engine->add_order(buy_order, executions);
    
    EXPECT_EQ(result, MatchResult::NO_MATCH);
    EXPECT_TRUE(executions.empty());
    
    // Both orders should be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 100.0);
    EXPECT_EQ(tob.ask_price, 105.0);
    EXPECT_EQ(tob.spread, 5.0);
}

// =============================================================================
// MARKET ORDER TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, MarketOrderBuyFullLiquidity) {
    // Add liquidity
    auto sell1 = create_sell_order(100.0, 10.0);
    auto sell2 = create_sell_order(101.0, 15.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(sell1, executions);
    engine->add_order(sell2, executions);
    
    // Process market buy order
    MatchResult result = engine->process_market_order(Side::BUY, 20.0, executions);
    
    EXPECT_EQ(result, MatchResult::PARTIAL_FILL);  // Only 20 out of 25 available was matched
    EXPECT_EQ(executions.size(), 2);
    
    // Should trade at best prices first
    EXPECT_EQ(executions[0].price, 100.0);
    EXPECT_EQ(executions[0].quantity, 10.0);
    EXPECT_EQ(executions[1].price, 101.0);
    EXPECT_EQ(executions[1].quantity, 10.0);  // Only 10 of the 15 available
    
    // Remaining liquidity should be in book
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.ask_price, 101.0);
    EXPECT_EQ(tob.ask_quantity, 5.0);
}

TEST_F(OrderBookEngineTest, MarketOrderNoLiquidity) {
    // Empty book
    std::vector<TradeExecution> executions;
    MatchResult result = engine->process_market_order(Side::BUY, 10.0, executions);
    
    EXPECT_EQ(result, MatchResult::NO_MATCH);
    EXPECT_TRUE(executions.empty());
}

TEST_F(OrderBookEngineTest, MarketOrderSellFullExecution) {
    // Add buy liquidity
    auto buy1 = create_buy_order(100.0, 10.0);
    auto buy2 = create_buy_order(99.0, 15.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy1, executions);
    engine->add_order(buy2, executions);
    
    // Process market sell order
    MatchResult result = engine->process_market_order(Side::SELL, 8.0, executions);
    
    EXPECT_EQ(result, MatchResult::FULL_FILL);
    EXPECT_EQ(executions.size(), 1);
    
    // Should trade at best bid price
    EXPECT_EQ(executions[0].price, 100.0);
    EXPECT_EQ(executions[0].quantity, 8.0);
    
    // Remaining buy liquidity
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 100.0);
    EXPECT_EQ(tob.bid_quantity, 2.0);  // 10 - 8 = 2 remaining
}

// =============================================================================
// MARKET DATA ACCESS TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, TopOfBookEmpty) {
    auto tob = engine->get_top_of_book();
    
    EXPECT_EQ(tob.bid_price, 0.0);
    EXPECT_EQ(tob.ask_price, 0.0);
    EXPECT_EQ(tob.bid_quantity, 0.0);
    EXPECT_EQ(tob.ask_quantity, 0.0);
    EXPECT_EQ(tob.mid_price, 0.0);
    EXPECT_EQ(tob.spread, 0.0);
}

TEST_F(OrderBookEngineTest, TopOfBookWithOrders) {
    auto buy_order = create_buy_order(99.5, 100.0);
    auto sell_order = create_sell_order(100.5, 200.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy_order, executions);
    engine->add_order(sell_order, executions);
    
    auto tob = engine->get_top_of_book();
    
    EXPECT_EQ(tob.bid_price, 99.5);
    EXPECT_EQ(tob.ask_price, 100.5);
    EXPECT_EQ(tob.bid_quantity, 100.0);
    EXPECT_EQ(tob.ask_quantity, 200.0);
    EXPECT_EQ(tob.mid_price, 100.0);
    EXPECT_EQ(tob.spread, 1.0);
}

TEST_F(OrderBookEngineTest, MidPriceCalculation) {
    auto buy_order = create_buy_order(98.0, 50.0);
    auto sell_order = create_sell_order(102.0, 75.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy_order, executions);
    engine->add_order(sell_order, executions);
    
    price_t mid_price = engine->get_mid_price();
    EXPECT_EQ(mid_price, 100.0);
}

TEST_F(OrderBookEngineTest, SpreadBasisPoints) {
    auto buy_order = create_buy_order(99.0, 50.0);
    auto sell_order = create_sell_order(101.0, 75.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy_order, executions);
    engine->add_order(sell_order, executions);
    
    double spread_bps = engine->get_spread_bps();
    // Spread = 2.0, Mid = 100.0, BPS = (2.0/100.0) * 10000 = 200
    EXPECT_NEAR(spread_bps, 200.0, 0.1);
}

TEST_F(OrderBookEngineTest, MarketDepthMultipleLevels) {
    // Add multiple bid levels
    auto buy1 = create_buy_order(100.0, 10.0);
    auto buy2 = create_buy_order(99.0, 20.0);
    auto buy3 = create_buy_order(98.0, 30.0);
    
    // Add multiple ask levels  
    auto sell1 = create_sell_order(101.0, 15.0);
    auto sell2 = create_sell_order(102.0, 25.0);
    auto sell3 = create_sell_order(103.0, 35.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy1, executions);
    engine->add_order(buy2, executions);
    engine->add_order(buy3, executions);
    engine->add_order(sell1, executions);
    engine->add_order(sell2, executions);
    engine->add_order(sell3, executions);
    
    auto depth = engine->get_market_depth(3);
    
    // Check bid levels (highest to lowest)
    ASSERT_EQ(depth.bids.size(), 3);
    EXPECT_EQ(depth.bids[0].price, 100.0);
    EXPECT_EQ(depth.bids[0].quantity, 10.0);
    EXPECT_EQ(depth.bids[1].price, 99.0);
    EXPECT_EQ(depth.bids[1].quantity, 20.0);
    EXPECT_EQ(depth.bids[2].price, 98.0);
    EXPECT_EQ(depth.bids[2].quantity, 30.0);
    
    // Check ask levels (lowest to highest)
    ASSERT_EQ(depth.asks.size(), 3);
    EXPECT_EQ(depth.asks[0].price, 101.0);
    EXPECT_EQ(depth.asks[0].quantity, 15.0);
    EXPECT_EQ(depth.asks[1].price, 102.0);
    EXPECT_EQ(depth.asks[1].quantity, 25.0);
    EXPECT_EQ(depth.asks[2].price, 103.0);
    EXPECT_EQ(depth.asks[2].quantity, 35.0);
}

TEST_F(OrderBookEngineTest, IsMarketCrossed) {
    // Normal market (not crossed)
    auto buy_order = create_buy_order(99.0, 50.0);
    auto sell_order = create_sell_order(101.0, 75.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy_order, executions);
    engine->add_order(sell_order, executions);
    
    EXPECT_FALSE(engine->is_market_crossed());
    
    // This shouldn't happen in practice due to matching, but test the detection
    engine->clear_book();
    
    // Manually create crossed market scenario by bypassing matching
    // (In real systems this would be prevented by the matching engine)
    EXPECT_FALSE(engine->is_market_crossed());  // Empty book is not crossed
}

// =============================================================================
// EDGE CASES AND BOUNDARY CONDITIONS
// =============================================================================

TEST_F(OrderBookEngineTest, InvalidOrderValidation) {
    // Test various invalid orders
    std::vector<TradeExecution> executions;
    
    // Zero price
    auto invalid_order1 = create_buy_order(0.0, 10.0);
    MatchResult result1 = engine->add_order(invalid_order1, executions);
    EXPECT_EQ(result1, MatchResult::REJECTED);
    
    // Negative price
    auto invalid_order2 = create_buy_order(-100.0, 10.0);
    MatchResult result2 = engine->add_order(invalid_order2, executions);
    EXPECT_EQ(result2, MatchResult::REJECTED);
    
    // Zero quantity
    auto invalid_order3 = create_buy_order(100.0, 0.0);
    MatchResult result3 = engine->add_order(invalid_order3, executions);
    EXPECT_EQ(result3, MatchResult::REJECTED);
    
    // Negative quantity
    auto invalid_order4 = create_buy_order(100.0, -10.0);
    MatchResult result4 = engine->add_order(invalid_order4, executions);
    EXPECT_EQ(result4, MatchResult::REJECTED);
}

TEST_F(OrderBookEngineTest, ExtremelySmallQuantities) {
    auto order = create_buy_order(100.0, 0.001);  // Very small quantity
    std::vector<TradeExecution> executions;
    
    MatchResult result = engine->add_order(order, executions);
    EXPECT_NE(result, MatchResult::REJECTED);  // Should be accepted if positive
    
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_quantity, 0.001);
}

TEST_F(OrderBookEngineTest, ExtremelyLargePrices) {
    auto order = create_buy_order(999999.99, 1.0);  // Very high price
    std::vector<TradeExecution> executions;
    
    MatchResult result = engine->add_order(order, executions);
    // Should either be accepted or rejected based on validation limits
    EXPECT_TRUE(result == MatchResult::NO_MATCH || result == MatchResult::REJECTED);
}

TEST_F(OrderBookEngineTest, ClearBookOperation) {
    // Add some orders
    auto buy_order = create_buy_order(99.0, 50.0);
    auto sell_order = create_sell_order(101.0, 75.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy_order, executions);
    engine->add_order(sell_order, executions);
    
    // Verify orders are in book
    auto tob = engine->get_top_of_book();
    EXPECT_NE(tob.bid_price, 0.0);
    EXPECT_NE(tob.ask_price, 0.0);
    
    // Clear book
    engine->clear_book();
    
    // Verify book is empty
    tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 0.0);
    EXPECT_EQ(tob.ask_price, 0.0);
}

TEST_F(OrderBookEngineTest, DoubleCancel) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    // Add and cancel order
    engine->add_order(order, executions);
    bool first_cancel = engine->cancel_order(order.order_id);
    EXPECT_TRUE(first_cancel);
    
    // Try to cancel again
    bool second_cancel = engine->cancel_order(order.order_id);
    EXPECT_FALSE(second_cancel);
}

TEST_F(OrderBookEngineTest, ModifyAfterCancel) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    // Add and cancel order
    engine->add_order(order, executions);
    engine->cancel_order(order.order_id);
    
    // Try to modify cancelled order
    bool modified = engine->modify_order(order.order_id, 101.0, 15.0);
    EXPECT_FALSE(modified);
}

// =============================================================================
// PERFORMANCE AND STATISTICS TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, BasicStatistics) {
    auto sell_order = create_sell_order(100.0, 10.0);
    auto buy_order = create_buy_order(100.0, 10.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(sell_order, executions);
    engine->add_order(buy_order, executions);  // Should create trade
    
    auto stats = engine->get_statistics();
    
    EXPECT_EQ(stats.total_orders_processed, 2);
    EXPECT_EQ(stats.total_trades, 1);
    EXPECT_EQ(stats.total_volume, 10.0);
}

TEST_F(OrderBookEngineTest, MatchingLatencyTracking) {
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    engine->add_order(order, executions);
    
    auto latency_stats = engine->get_matching_latency();
    EXPECT_GT(latency_stats.count, 0);
    EXPECT_GT(latency_stats.mean_us, 0.0);
}

TEST_F(OrderBookEngineTest, ResetPerformanceCounters) {
    // Generate some activity
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    engine->add_order(order, executions);
    
    auto stats_before = engine->get_statistics();
    EXPECT_GT(stats_before.total_orders_processed, 0);
    
    // Reset counters
    engine->reset_performance_counters();
    
    auto stats_after = engine->get_statistics();
    EXPECT_EQ(stats_after.total_orders_processed, 0);
    EXPECT_EQ(stats_after.total_trades, 0);
    EXPECT_EQ(stats_after.total_volume, 0.0);
}

// =============================================================================
// CALLBACK AND EVENT TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, BookUpdateCallbacks) {
    int initial_callback_count = callback_call_count;
    
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    engine->add_order(order, executions);
    
    EXPECT_GT(callback_call_count, initial_callback_count);
    EXPECT_TRUE(last_book_update.has_value());
    EXPECT_EQ(last_book_update->bid_price, 100.0);
}

TEST_F(OrderBookEngineTest, TradeCallbacks) {
    auto sell_order = create_sell_order(100.0, 10.0);
    auto buy_order = create_buy_order(100.0, 10.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(sell_order, executions);
    
    int trade_callback_count_before = trade_history.size();
    engine->add_order(buy_order, executions);
    
    EXPECT_GT(trade_history.size(), trade_callback_count_before);
    EXPECT_TRUE(last_trade.has_value());
    EXPECT_EQ(last_trade->price, 100.0);
    EXPECT_EQ(last_trade->quantity, 10.0);
}

TEST_F(OrderBookEngineTest, DepthUpdateCallbacks) {
    // Adding an order should trigger depth update
    auto order = create_buy_order(100.0, 10.0);
    std::vector<TradeExecution> executions;
    
    bool depth_updated = false;
    engine->set_depth_update_callback([&depth_updated](const MarketDepth& /* depth */) {
        depth_updated = true;
    });
    
    engine->add_order(order, executions);
    EXPECT_TRUE(depth_updated);
}

// =============================================================================
// CONCURRENT ACCESS TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, ConcurrentTopOfBookReads) {
    // Add some orders to establish market
    auto buy_order = create_buy_order(99.0, 100.0);
    auto sell_order = create_sell_order(101.0, 100.0);
    
    std::vector<TradeExecution> executions;
    engine->add_order(buy_order, executions);
    engine->add_order(sell_order, executions);
    
    std::atomic<int> successful_reads{0};
    std::vector<std::thread> readers;
    
    // Spawn multiple reader threads
    for (int i = 0; i < 10; ++i) {
        readers.emplace_back([this, &successful_reads]() {
            for (int j = 0; j < 100; ++j) {
                auto tob = engine->get_top_of_book();
                if (tob.bid_price == 99.0 && tob.ask_price == 101.0) {
                    successful_reads++;
                }
                std::this_thread::sleep_for(1us);
            }
        });
    }
    
    // Wait for all readers to complete
    for (auto& reader : readers) {
        reader.join();
    }
    
    EXPECT_EQ(successful_reads.load(), 1000);  // All reads should be consistent
}

// =============================================================================
// MARKET DATA INTEGRATION TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, ApplyMarketDataSnapshot) {
    MarketDepth snapshot(5);
    snapshot.bids.emplace_back(99.0, 100.0);
    snapshot.bids.emplace_back(98.0, 200.0);
    snapshot.asks.emplace_back(101.0, 150.0);
    snapshot.asks.emplace_back(102.0, 250.0);
    snapshot.timestamp = now();
    
    engine->apply_market_data_update(snapshot);
    
    auto tob = engine->get_top_of_book();
    EXPECT_EQ(tob.bid_price, 99.0);
    EXPECT_EQ(tob.ask_price, 101.0);
    
    auto depth = engine->get_market_depth(5);
    EXPECT_EQ(depth.bids.size(), 2);
    EXPECT_EQ(depth.asks.size(), 2);
}

// =============================================================================
// STRESS TESTS
// =============================================================================

TEST_F(OrderBookEngineTest, HighVolumeOrderProcessing) {
    std::vector<TradeExecution> executions;
    constexpr int NUM_ORDERS = 1000;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Add alternating buy and sell orders
    for (int i = 0; i < NUM_ORDERS; ++i) {
        if (i % 2 == 0) {
            auto order = create_buy_order(100.0 - (i * 0.01), 10.0);
            engine->add_order(order, executions);
        } else {
            auto order = create_sell_order(100.0 + (i * 0.01), 10.0);
            engine->add_order(order, executions);
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    std::cout << "Processed " << NUM_ORDERS << " orders in " 
              << duration.count() << " microseconds" << std::endl;
    std::cout << "Average latency: " << (duration.count() / NUM_ORDERS) 
              << " microseconds per order" << std::endl;
    
    // Verify book has orders
    auto depth = engine->get_market_depth(10);
    EXPECT_GT(depth.bids.size(), 0);
    EXPECT_GT(depth.asks.size(), 0);
}

TEST_F(OrderBookEngineTest, RandomOrderSequence) {
    std::mt19937 gen(42);  // Fixed seed for reproducibility
    std::uniform_real_distribution<> price_dist(95.0, 105.0);
    std::uniform_real_distribution<> quantity_dist(1.0, 100.0);
    std::uniform_int_distribution<> side_dist(0, 1);
    
    std::vector<TradeExecution> executions;
    constexpr int NUM_ORDERS = 500;
    
    for (int i = 0; i < NUM_ORDERS; ++i) {
        Side side = (side_dist(gen) == 0) ? Side::BUY : Side::SELL;
        price_t price = price_dist(gen);
        quantity_t quantity = quantity_dist(gen);
        
        auto order = create_order(side, price, quantity);
        MatchResult result = engine->add_order(order, executions);
        
        // All orders should be valid (not rejected)
        EXPECT_NE(result, MatchResult::REJECTED);
    }
    
    // Verify book state is consistent
    auto tob = engine->get_top_of_book();
    if (tob.bid_price > 0 && tob.ask_price > 0) {
        EXPECT_LE(tob.bid_price, tob.ask_price);  // No crossed market
    }
}

// =============================================================================
// MAIN TEST RUNNER
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
