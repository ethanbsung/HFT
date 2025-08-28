#include <gtest/gtest.h>
#include "order_manager.hpp"
#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include "orderbook_engine.hpp"
#include "types.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <future>
#include <chrono>
#include <set>

using namespace hft;

// =============================================================================
// TEST FIXTURES
// =============================================================================

class OrderManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up reasonable risk limits for testing
        risk_limits.max_position = 1000.0;
        risk_limits.max_daily_loss = 10000.0;
        risk_limits.max_orders_per_second = 100;
        
        // Initialize components
        memory_manager = &MemoryManager::instance();
        latency_tracker = std::make_unique<LatencyTracker>(1000);
        order_manager = std::make_unique<OrderManager>(*memory_manager, *latency_tracker, risk_limits);
    }
    
    void TearDown() override {
        order_manager.reset();
        latency_tracker.reset();
    }
    
    MemoryManager* memory_manager;
    std::unique_ptr<LatencyTracker> latency_tracker;
    std::unique_ptr<OrderManager> order_manager;
    RiskLimits risk_limits;
};

class OrderManagerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        risk_limits.max_position = 1000.0;
        risk_limits.max_daily_loss = 10000.0;
        risk_limits.max_orders_per_second = 100;
        
        memory_manager = &MemoryManager::instance();
        latency_tracker = std::make_unique<LatencyTracker>(1000);
        orderbook_engine = std::make_unique<OrderBookEngine>(*memory_manager, *latency_tracker, "TEST_SYMBOL");
        order_manager = std::make_unique<OrderManager>(*memory_manager, *latency_tracker, risk_limits);
        
        // Connect order manager to orderbook engine
        order_manager->set_orderbook_engine(orderbook_engine.get());
    }
    
    void TearDown() override {
        order_manager.reset();
        orderbook_engine.reset();
        latency_tracker.reset();
    }
    
    MemoryManager* memory_manager;
    std::unique_ptr<LatencyTracker> latency_tracker;
    std::unique_ptr<OrderBookEngine> orderbook_engine;
    std::unique_ptr<OrderManager> order_manager;
    RiskLimits risk_limits;
};

// =============================================================================
// BASIC ORDER OPERATIONS TESTS
// =============================================================================

TEST_F(OrderManagerTest, BasicOrderCreation) {
    // Test basic order creation
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    
    EXPECT_NE(order_id, 0);
    EXPECT_EQ(order_manager->get_pending_order_count(), 1);
    EXPECT_EQ(order_manager->get_active_order_count(), 0);
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->order.side, Side::BUY);
    EXPECT_EQ(info->order.price, 100.0);
    EXPECT_EQ(info->order.original_quantity, 10.0);
    EXPECT_EQ(info->order.remaining_quantity, 10.0);
    EXPECT_EQ(info->execution_state, ExecutionState::PENDING_SUBMISSION);
}

TEST_F(OrderManagerTest, MultipleOrderCreation) {
    // Test creating multiple orders
    std::vector<uint64_t> order_ids;
    
    for (int i = 0; i < 10; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0 + i, 10.0, 99.5);
        EXPECT_NE(id, 0);
        order_ids.push_back(id);
    }
    
    EXPECT_EQ(order_manager->get_pending_order_count(), 10);
    
    // Verify all orders are unique
    std::set<uint64_t> unique_ids(order_ids.begin(), order_ids.end());
    EXPECT_EQ(unique_ids.size(), 10);
}

TEST_F(OrderManagerTest, OrderModification) {
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    // Test price modification
    EXPECT_TRUE(order_manager->modify_order(order_id, 101.0, 10.0, ModificationType::PRICE_ONLY));
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->order.price, 101.0);
    EXPECT_EQ(info->order.remaining_quantity, 10.0);
    
    // Test quantity modification (reduction only)
    EXPECT_TRUE(order_manager->modify_order(order_id, 101.0, 8.0, ModificationType::QUANTITY_ONLY));
    EXPECT_EQ(info->order.remaining_quantity, 8.0);
    
    // Test both price and quantity
    EXPECT_TRUE(order_manager->modify_order(order_id, 102.0, 5.0, ModificationType::PRICE_AND_QUANTITY));
    EXPECT_EQ(info->order.price, 102.0);
    EXPECT_EQ(info->order.remaining_quantity, 5.0);
}

TEST_F(OrderManagerTest, OrderCancellation) {
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    EXPECT_TRUE(order_manager->cancel_order(order_id));
    EXPECT_EQ(order_manager->get_pending_order_count(), 0);
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->execution_state, ExecutionState::CANCELLED);
    EXPECT_EQ(info->order.status, OrderStatus::CANCELLED);
}

TEST_F(OrderManagerTest, OrderSubmission) {
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    EXPECT_TRUE(order_manager->submit_order(order_id));
    EXPECT_EQ(order_manager->get_pending_order_count(), 0);
    EXPECT_EQ(order_manager->get_active_order_count(), 1);
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->execution_state, ExecutionState::SUBMITTED);
    EXPECT_EQ(info->order.status, OrderStatus::ACTIVE);
}

// =============================================================================
// EDGE CASES AND BOUNDARY CONDITIONS
// =============================================================================

TEST_F(OrderManagerTest, InvalidOrderParameters) {
    // Test zero quantity
    uint64_t id1 = order_manager->create_order(Side::BUY, 100.0, 0.0, 99.5);
    EXPECT_EQ(id1, 0);  // Should reject
    
    // Test negative quantity
    uint64_t id2 = order_manager->create_order(Side::BUY, 100.0, -10.0, 99.5);
    EXPECT_EQ(id2, 0);  // Should reject
    
    // Test negative price
    uint64_t id3 = order_manager->create_order(Side::BUY, -100.0, 10.0, 99.5);
    EXPECT_EQ(id3, 0);  // Should reject
}

TEST_F(OrderManagerTest, ModifyNonExistentOrder) {
    // Try to modify order that doesn't exist
    EXPECT_FALSE(order_manager->modify_order(999999, 100.0, 10.0));
    
    // Try to cancel order that doesn't exist
    EXPECT_FALSE(order_manager->cancel_order(999999));
}

TEST_F(OrderManagerTest, ModifyCompletedOrder) {
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    // Cancel the order first
    EXPECT_TRUE(order_manager->cancel_order(order_id));
    
    // Try to modify cancelled order - should fail
    EXPECT_FALSE(order_manager->modify_order(order_id, 101.0, 10.0));
}

TEST_F(OrderManagerTest, QuantityIncreaseRejection) {
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    // Try to increase quantity - should be rejected
    EXPECT_FALSE(order_manager->modify_order(order_id, 100.0, 15.0, ModificationType::QUANTITY_ONLY));
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->order.remaining_quantity, 10.0);  // Should remain unchanged
}

// =============================================================================
// RISK MANAGEMENT TESTS
// =============================================================================

TEST_F(OrderManagerTest, PositionLimitCheck) {
    // Test position limit by simulating executed trades that approach the limit
    risk_limits.max_position = 50.0;
    order_manager->update_risk_limits(risk_limits);
    
    // Simulate some executed position to approach the limit
    order_manager->update_position(40.0, 100.0, Side::BUY);  // Now at 40 position
    
    // Order within remaining limit should succeed
    uint64_t id1 = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    EXPECT_NE(id1, 0);
    
    // Order that would exceed limit should be rejected
    uint64_t id2 = order_manager->create_order(Side::BUY, 100.0, 20.0, 99.5);
    EXPECT_EQ(id2, 0);  // Should be rejected (40 + 20 = 60 > 50)
}

TEST_F(OrderManagerTest, OrderRateLimitCheck) {
    // Set very low rate limit for testing
    risk_limits.max_orders_per_second = 2;
    order_manager->update_risk_limits(risk_limits);
    
    // First two orders should succeed
    uint64_t id1 = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    uint64_t id2 = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    EXPECT_NE(id1, 0);
    EXPECT_NE(id2, 0);
    
    // Submit both orders to trigger rate limiting
    EXPECT_TRUE(order_manager->submit_order(id1));
    EXPECT_TRUE(order_manager->submit_order(id2));
    
    // Third order creation should succeed, but submission might fail due to rate limit
    uint64_t id3 = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    EXPECT_NE(id3, 0);
    
    // Immediate submission should hit rate limit
    EXPECT_FALSE(order_manager->submit_order(id3));
}

TEST_F(OrderManagerTest, EmergencyShutdown) {
    // Create several orders
    std::vector<uint64_t> order_ids;
    for (int i = 0; i < 5; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0 + i, 10.0, 99.5);
        EXPECT_NE(id, 0);
        order_ids.push_back(id);
        order_manager->submit_order(id);
    }
    
    EXPECT_EQ(order_manager->get_active_order_count(), 5);
    
    // Trigger emergency shutdown
    order_manager->emergency_shutdown("Test emergency");
    
    // All orders should be cancelled
    EXPECT_EQ(order_manager->get_active_order_count(), 0);
    
    // Should not be able to create new orders
    uint64_t new_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    EXPECT_EQ(new_id, 0);
}

// =============================================================================
// POSITION AND P&L TESTS
// =============================================================================

TEST_F(OrderManagerTest, BasicPositionTracking) {
    // Start with zero position
    auto position = order_manager->get_position();
    EXPECT_EQ(position.net_position, 0.0);
    EXPECT_EQ(position.realized_pnl, 0.0);
    
    // Simulate a buy trade
    order_manager->update_position(100.0, 50.0, Side::BUY);
    position = order_manager->get_position();
    EXPECT_EQ(position.net_position, 100.0);
    EXPECT_EQ(position.avg_price, 50.0);
    
    // Simulate a sell trade (partial close)
    order_manager->update_position(60.0, 55.0, Side::SELL);
    position = order_manager->get_position();
    EXPECT_EQ(position.net_position, 40.0);
    EXPECT_EQ(position.avg_price, 50.0);  // Average price should remain the same
    EXPECT_EQ(position.realized_pnl, 300.0);  // (55 - 50) * 60 = 300
}

TEST_F(OrderManagerTest, PositionFlipping) {
    // Start with long position
    order_manager->update_position(100.0, 50.0, Side::BUY);
    auto position = order_manager->get_position();
    EXPECT_EQ(position.net_position, 100.0);
    
    // Flip to short position
    order_manager->update_position(150.0, 55.0, Side::SELL);
    position = order_manager->get_position();
    EXPECT_EQ(position.net_position, -50.0);
    EXPECT_EQ(position.avg_price, 55.0);  // New position uses sell price
    EXPECT_EQ(position.realized_pnl, 500.0);  // (55 - 50) * 100 = 500
}

TEST_F(OrderManagerTest, UnrealizedPnLCalculation) {
    // Create long position
    order_manager->update_position(100.0, 50.0, Side::BUY);
    
    // Calculate unrealized P&L with different market prices
    double pnl_up = order_manager->calculate_unrealized_pnl(55.0);
    EXPECT_EQ(pnl_up, 500.0);  // (55 - 50) * 100
    
    double pnl_down = order_manager->calculate_unrealized_pnl(45.0);
    EXPECT_EQ(pnl_down, -500.0);  // (45 - 50) * 100
    
    double pnl_flat = order_manager->calculate_unrealized_pnl(50.0);
    EXPECT_EQ(pnl_flat, 0.0);
}

// =============================================================================
// ORDER LIFECYCLE TESTS
// =============================================================================

TEST_F(OrderManagerTest, CompleteOrderLifecycle) {
    // Create order
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    // Submit order
    EXPECT_TRUE(order_manager->submit_order(order_id));
    
    // Handle acknowledgment
    auto ack_time = now();
    EXPECT_TRUE(order_manager->handle_order_ack(order_id, ack_time));
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->execution_state, ExecutionState::ACKNOWLEDGED);
    
    // Handle partial fill
    auto fill_time = now();
    EXPECT_TRUE(order_manager->handle_fill(order_id, 6.0, 100.5, fill_time, false));
    EXPECT_EQ(info->execution_state, ExecutionState::PARTIALLY_FILLED);
    EXPECT_EQ(info->filled_quantity, 6.0);
    EXPECT_EQ(info->average_fill_price, 100.5);
    
    // Handle final fill
    EXPECT_TRUE(order_manager->handle_fill(order_id, 4.0, 101.0, fill_time, true));
    EXPECT_EQ(info->execution_state, ExecutionState::FILLED);
    EXPECT_EQ(info->filled_quantity, 10.0);
    
    // Calculate volume-weighted average price: (6.0 * 100.5 + 4.0 * 101.0) / 10.0 = 100.7
    EXPECT_NEAR(info->average_fill_price, 100.7, 0.001);
    
    // Order should be removed from active orders
    EXPECT_EQ(order_manager->get_active_order_count(), 0);
}

TEST_F(OrderManagerTest, OrderRejection) {
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(order_id, 0);
    
    EXPECT_TRUE(order_manager->submit_order(order_id));
    EXPECT_EQ(order_manager->get_active_order_count(), 1);
    
    // Handle rejection
    EXPECT_TRUE(order_manager->handle_rejection(order_id, "Insufficient funds"));
    
    const OrderInfo* info = order_manager->get_order_info(order_id);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->execution_state, ExecutionState::REJECTED);
    EXPECT_EQ(order_manager->get_active_order_count(), 0);
}

// =============================================================================
// PERFORMANCE AND STATISTICS TESTS
// =============================================================================

TEST_F(OrderManagerTest, ExecutionStatistics) {
    // Create and execute several orders to generate statistics
    std::vector<uint64_t> order_ids;
    
    // Create 10 orders
    for (int i = 0; i < 10; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
        ASSERT_NE(id, 0);
        order_ids.push_back(id);
        order_manager->submit_order(id);
    }
    
    // Fill 6 orders
    for (int i = 0; i < 6; ++i) {
        order_manager->handle_fill(order_ids[i], 10.0, 100.5, now(), true);
    }
    
    // Cancel 2 orders
    for (int i = 6; i < 8; ++i) {
        order_manager->cancel_order(order_ids[i]);
    }
    
    // Reject 2 orders
    for (int i = 8; i < 10; ++i) {
        order_manager->handle_rejection(order_ids[i], "Test rejection");
    }
    
    auto stats = order_manager->get_execution_stats();
    EXPECT_EQ(stats.total_orders, 10);
    EXPECT_EQ(stats.filled_orders, 6);
    EXPECT_EQ(stats.cancelled_orders, 2);
    EXPECT_EQ(stats.rejected_orders, 2);
    EXPECT_NEAR(stats.fill_rate, 0.6, 0.001);  // 6/10 = 60%
}

TEST_F(OrderManagerTest, DailyStatsReset) {
    // Generate some activity
    uint64_t id1 = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    uint64_t id2 = order_manager->create_order(Side::SELL, 100.0, 10.0, 99.5);
    order_manager->submit_order(id1);
    order_manager->submit_order(id2);
    order_manager->handle_fill(id1, 10.0, 100.5, now(), true);
    order_manager->cancel_order(id2);
    
    auto stats_before = order_manager->get_execution_stats();
    EXPECT_GT(stats_before.total_orders, 0);
    
    // Reset stats
    order_manager->reset_daily_stats();
    
    auto stats_after = order_manager->get_execution_stats();
    EXPECT_EQ(stats_after.total_orders, 0);
    EXPECT_EQ(stats_after.filled_orders, 0);
    EXPECT_EQ(stats_after.cancelled_orders, 0);
}

// =============================================================================
// CONCURRENCY TESTS
// =============================================================================

TEST_F(OrderManagerTest, ConcurrentOrderCreation) {
    const int num_threads = 4;
    const int orders_per_thread = 25;
    std::vector<std::thread> threads;
    std::vector<std::vector<uint64_t>> thread_results(num_threads);
    
    // Launch threads to create orders concurrently
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, orders_per_thread, &thread_results]() {
            for (int i = 0; i < orders_per_thread; ++i) {
                uint64_t id = order_manager->create_order(Side::BUY, 100.0 + t, 10.0, 99.5);
                if (id != 0) {
                    thread_results[t].push_back(id);
                }
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify all orders were created with unique IDs
    std::set<uint64_t> all_ids;
    size_t total_orders = 0;
    
    for (const auto& results : thread_results) {
        total_orders += results.size();
        for (uint64_t id : results) {
            EXPECT_TRUE(all_ids.insert(id).second);  // Should be unique
        }
    }
    
    EXPECT_EQ(total_orders, num_threads * orders_per_thread);
    EXPECT_EQ(all_ids.size(), total_orders);
}

TEST_F(OrderManagerTest, ConcurrentOrderOperations) {
    // Create orders first
    std::vector<uint64_t> order_ids;
    for (int i = 0; i < 20; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0 + i, 10.0, 99.5);
        ASSERT_NE(id, 0);
        order_ids.push_back(id);
    }
    
    std::atomic<int> modifications(0);
    std::atomic<int> cancellations(0);
    std::atomic<int> submissions(0);
    
    std::vector<std::thread> threads;
    
    // Thread 1: Modify orders
    threads.emplace_back([&]() {
        for (int i = 0; i < 10; ++i) {
            if (order_manager->modify_order(order_ids[i], 101.0 + i, 8.0)) {
                modifications++;
            }
        }
    });
    
    // Thread 2: Cancel orders
    threads.emplace_back([&]() {
        for (int i = 10; i < 15; ++i) {
            if (order_manager->cancel_order(order_ids[i])) {
                cancellations++;
            }
        }
    });
    
    // Thread 3: Submit orders
    threads.emplace_back([&]() {
        for (int i = 15; i < 20; ++i) {
            if (order_manager->submit_order(order_ids[i])) {
                submissions++;
            }
        }
    });
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_GT(modifications.load(), 0);
    EXPECT_GT(cancellations.load(), 0);
    EXPECT_GT(submissions.load(), 0);
}

// =============================================================================
// INTEGRATION TESTS WITH ORDERBOOK ENGINE
// =============================================================================

TEST_F(OrderManagerIntegrationTest, OrderBookIntegration) {
    // Create and submit a buy order
    uint64_t buy_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    ASSERT_NE(buy_id, 0);
    EXPECT_TRUE(order_manager->submit_order(buy_id));
    
    // Create and submit a sell order that should match
    uint64_t sell_id = order_manager->create_order(Side::SELL, 100.0, 10.0, 100.5);
    ASSERT_NE(sell_id, 0);
    EXPECT_TRUE(order_manager->submit_order(sell_id));
    
    // Orders should have executed
    const OrderInfo* buy_info = order_manager->get_order_info(buy_id);
    const OrderInfo* sell_info = order_manager->get_order_info(sell_id);
    
    ASSERT_NE(buy_info, nullptr);
    ASSERT_NE(sell_info, nullptr);
    
    // Check that at least one of the orders got filled
    EXPECT_TRUE(buy_info->execution_state == ExecutionState::FILLED || 
                buy_info->execution_state == ExecutionState::PARTIALLY_FILLED ||
                sell_info->execution_state == ExecutionState::FILLED ||
                sell_info->execution_state == ExecutionState::PARTIALLY_FILLED);
}

// =============================================================================
// CALLBACK TESTS
// =============================================================================

TEST_F(OrderManagerTest, OrderCallbacks) {
    bool order_callback_triggered = false;
    bool fill_callback_triggered = false;
    bool risk_callback_triggered = false;
    
    order_manager->set_order_callback([&](const OrderInfo& info) {
        order_callback_triggered = true;
        EXPECT_NE(info.order.order_id, 0);
    });
    
    order_manager->set_fill_callback([&](const OrderInfo& info, quantity_t qty, price_t price, bool final) {
        (void)info;   // Suppress unused parameter warning
        (void)final;  // Suppress unused parameter warning
        fill_callback_triggered = true;
        EXPECT_GT(qty, 0);
        EXPECT_GT(price, 0);
    });
    
    order_manager->set_risk_callback([&](RiskViolationType type, const std::string& message) {
        (void)type;  // Suppress unused parameter warning
        risk_callback_triggered = true;
        EXPECT_FALSE(message.empty());
    });
    
    // Create order (should trigger order callback)
    uint64_t order_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    EXPECT_TRUE(order_callback_triggered);
    
    // Submit and fill order (should trigger fill callback)
    order_manager->submit_order(order_id);
    order_manager->handle_fill(order_id, 10.0, 100.5, now(), true);
    EXPECT_TRUE(fill_callback_triggered);
    
    // Trigger emergency shutdown (should trigger risk callback)
    order_manager->emergency_shutdown("Test emergency");
    EXPECT_TRUE(risk_callback_triggered);
}

// =============================================================================
// STRESS TESTS
// =============================================================================

TEST_F(OrderManagerTest, HighVolumeOrderProcessing) {
    const int num_orders = 1000;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::vector<uint64_t> order_ids;
    order_ids.reserve(num_orders);
    
    // Create many orders
    for (int i = 0; i < num_orders; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
        EXPECT_NE(id, 0);
        order_ids.push_back(id);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    double avg_latency_ns = static_cast<double>(duration.count() * 1000) / num_orders;
    
    std::cout << "[PERF] Created " << num_orders << " orders in " << duration.count() 
              << " μs (avg: " << std::fixed << std::setprecision(2) << avg_latency_ns << " ns/order)" << std::endl;
    
    // Verify performance is reasonable (less than 10μs per order on average in debug builds)
    EXPECT_LT(avg_latency_ns, 10000.0);
    
    EXPECT_EQ(order_manager->get_pending_order_count(), num_orders);
}

// =============================================================================
// BOUNDARY CONDITION TESTS
// =============================================================================

TEST_F(OrderManagerTest, MaxOrderIdBoundary) {
    // This test ensures order ID generation works correctly
    // even near boundary conditions
    const int num_orders = 100;
    std::set<uint64_t> generated_ids;
    
    for (int i = 0; i < num_orders; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
        EXPECT_NE(id, 0);
        EXPECT_TRUE(generated_ids.insert(id).second);  // Should be unique
    }
    
    EXPECT_EQ(generated_ids.size(), num_orders);
}

TEST_F(OrderManagerTest, MemoryPoolExhaustion) {
    // Test behavior when memory pool runs out
    // This depends on the pool size - we'll create many orders to stress test
    const int stress_orders = 2000;  // Assuming pool might be smaller
    std::vector<uint64_t> created_orders;
    
    for (int i = 0; i < stress_orders; ++i) {
        uint64_t id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
        if (id != 0) {
            created_orders.push_back(id);
        } else {
            // Pool exhausted - this is expected behavior
            break;
        }
    }
    
    // Should have created at least some orders
    EXPECT_GT(created_orders.size(), 0);
    std::cout << "[INFO] Created " << created_orders.size() << " orders before potential pool exhaustion" << std::endl;
    
    // Cancel some orders to free up pool space
    size_t orders_to_cancel = std::min(created_orders.size(), static_cast<size_t>(100));
    for (size_t i = 0; i < orders_to_cancel; ++i) {
        order_manager->cancel_order(created_orders[i]);
    }
    
    // Should be able to create new orders again
    uint64_t new_id = order_manager->create_order(Side::BUY, 100.0, 10.0, 99.5);
    EXPECT_NE(new_id, 0);
}

// =============================================================================
// MAIN TEST RUNNER
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
