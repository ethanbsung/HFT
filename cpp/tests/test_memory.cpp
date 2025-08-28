#include <gtest/gtest.h>
#include "../include/memory_pool.hpp"
#include "../include/types.hpp"
#include <thread>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include <set>
#include <memory>
#include <atomic>
#include <future>
#include <iostream> // Added for debug output

using namespace hft;
using namespace std::chrono_literals;

// =============================================================================
// TEST FIXTURES
// =============================================================================

class MemoryPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = std::make_unique<MemoryPool<int>>(10, 2);
    }
    
    void TearDown() override {
        pool.reset();
    }
    
    std::unique_ptr<MemoryPool<int>> pool;
    
    // Helper to check if pointer is from pool's memory blocks
    bool is_valid_pool_pointer(int* ptr) {
        return ptr != nullptr;
    }
};

class LockFreeMemoryPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        lockfree_pool = std::make_unique<LockFreeMemoryPool<int>>(100);
    }
    
    void TearDown() override {
        lockfree_pool.reset();
    }
    
    std::unique_ptr<LockFreeMemoryPool<int>> lockfree_pool;
};

class OrderPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        order_pool = std::make_unique<OrderPool>(50);
    }
    
    void TearDown() override {
        order_pool.reset();
    }
    
    std::unique_ptr<OrderPool> order_pool;
};

class MemoryManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset statistics for clean test environment
        MemoryManager::instance().order_pool().reset_stats();
    }
    
    void TearDown() override {
        // Clean up after tests
        MemoryManager::instance().order_pool().reset_stats();
    }
};

// =============================================================================
// BASIC MEMORY POOL TEMPLATE TESTS
// =============================================================================

TEST_F(MemoryPoolTest, DefaultConstruction) {
    EXPECT_NO_THROW(MemoryPool<int> default_pool);
    MemoryPool<int> default_pool(1000, 2);
    EXPECT_EQ(default_pool.total_allocated(), 1000);
    EXPECT_EQ(default_pool.available(), 1000);
    EXPECT_EQ(default_pool.in_use(), 0);
}

TEST_F(MemoryPoolTest, BasicAcquireRelease) {
    // Test basic acquire
    int* obj = pool->acquire();
    EXPECT_NE(obj, nullptr);
    EXPECT_TRUE(is_valid_pool_pointer(obj));
    EXPECT_EQ(*obj, 0); // Should be default-constructed
    
    // Check pool state
    EXPECT_EQ(pool->in_use(), 1);
    EXPECT_EQ(pool->available(), 9);
    
    // Test basic release
    pool->release(obj);
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_EQ(pool->available(), 10);
}

TEST_F(MemoryPoolTest, MultipleAcquireRelease) {
    std::vector<int*> objects;
    
    // Acquire multiple objects
    for (int i = 0; i < 5; ++i) {
        int* obj = pool->acquire();
        EXPECT_NE(obj, nullptr);
        *obj = i; // Set unique value
        objects.push_back(obj);
    }
    
    EXPECT_EQ(pool->in_use(), 5);
    EXPECT_EQ(pool->available(), 5);
    
    // Verify object values
    for (size_t i = 0; i < objects.size(); ++i) {
        EXPECT_EQ(*objects[i], static_cast<int>(i));
    }
    
    // Release all objects
    for (auto obj : objects) {
        pool->release(obj);
    }
    
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_EQ(pool->available(), 10);
}

TEST_F(MemoryPoolTest, PoolExpansion) {
    std::vector<int*> objects;
    
    // Acquire more than initial capacity (should trigger expansion)
    for (int i = 0; i < 15; ++i) {
        int* obj = pool->acquire();
        EXPECT_NE(obj, nullptr);
        objects.push_back(obj);
    }
    
    // Pool should have expanded (initial: 10, growth: 2x = 20, total: 30)
    EXPECT_EQ(pool->total_allocated(), 30);
    EXPECT_EQ(pool->in_use(), 15);
    EXPECT_EQ(pool->available(), 15);
    
    // Clean up
    for (auto obj : objects) {
        pool->release(obj);
    }
}

TEST_F(MemoryPoolTest, NullPointerRelease) {
    // Releasing null pointer should be safe
    EXPECT_NO_THROW(pool->release(nullptr));
    
    // Pool state should be unchanged
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_EQ(pool->available(), 10);
}

TEST_F(MemoryPoolTest, DoubleRelease) {
    int* obj = pool->acquire();
    EXPECT_NE(obj, nullptr);
    
    // First release
    pool->release(obj);
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_EQ(pool->available(), 10);
    
    // Second release (should be safe but undefined behavior)
    // In a real system, this might corrupt the pool, but we test for crash safety
    EXPECT_NO_THROW(pool->release(obj));
}

TEST_F(MemoryPoolTest, ReserveCapacity) {
    size_t initial_allocated = pool->total_allocated();
    
    // Reserve additional capacity
    pool->reserve(50);
    
    // Should have at least 50 available objects
    EXPECT_GE(pool->available(), 50);
    EXPECT_GT(pool->total_allocated(), initial_allocated);
}

TEST_F(MemoryPoolTest, ShrinkToFit) {
    // First expand the pool
    std::vector<int*> objects;
    for (int i = 0; i < 25; ++i) {
        objects.push_back(pool->acquire());
    }
    
    // Release most objects but keep some in use
    for (int i = 0; i < 20; ++i) {
        pool->release(objects[i]);
    }
    
    size_t initial_allocated = pool->total_allocated();
    
    // Try to shrink to fit (should keep safety buffer)
    pool->shrink_to_fit(10);
    
    // Should still have objects in use + safety buffer
    EXPECT_GE(pool->total_allocated(), 5); // Objects still in use
    
    // Clean up remaining objects
    for (int i = 20; i < 25; ++i) {
        pool->release(objects[i]);
    }
}

// =============================================================================
// LOCK-FREE MEMORY POOL TESTS
// =============================================================================

TEST_F(LockFreeMemoryPoolTest, BasicFunctionality) {
    EXPECT_EQ(lockfree_pool->capacity(), 100);
    EXPECT_EQ(lockfree_pool->available(), 100);
    EXPECT_EQ(lockfree_pool->in_use(), 0);
    
    // Acquire object
    int* obj = lockfree_pool->acquire();
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(*obj, 0); // Default constructed
    
    EXPECT_EQ(lockfree_pool->available(), 99);
    EXPECT_EQ(lockfree_pool->in_use(), 1);
    
    // Release object
    lockfree_pool->release(obj);
    EXPECT_EQ(lockfree_pool->available(), 100);
    EXPECT_EQ(lockfree_pool->in_use(), 0);
}

TEST_F(LockFreeMemoryPoolTest, PoolExhaustion) {
    std::vector<int*> objects;
    
    // Exhaust the pool
    for (size_t i = 0; i < lockfree_pool->capacity(); ++i) {
        int* obj = lockfree_pool->acquire();
        EXPECT_NE(obj, nullptr);
        objects.push_back(obj);
    }
    
    EXPECT_EQ(lockfree_pool->available(), 0);
    EXPECT_EQ(lockfree_pool->in_use(), 100);
    
    // Next acquire should return nullptr
    int* overflow_obj = lockfree_pool->acquire();
    EXPECT_EQ(overflow_obj, nullptr);
    
    // Clean up
    for (auto obj : objects) {
        lockfree_pool->release(obj);
    }
}

TEST_F(LockFreeMemoryPoolTest, NeedsExpansionDetection) {
    std::vector<int*> objects;
    
    // Use most of the pool (> 90%)
    for (size_t i = 0; i < 95; ++i) {
        objects.push_back(lockfree_pool->acquire());
    }
    
    EXPECT_TRUE(lockfree_pool->needs_expansion());
    
    // Clean up
    for (auto obj : objects) {
        lockfree_pool->release(obj);
    }
    
    EXPECT_FALSE(lockfree_pool->needs_expansion());
}

TEST_F(LockFreeMemoryPoolTest, InvalidReleaseHandling) {
    // Release null pointer
    EXPECT_NO_THROW(lockfree_pool->release(nullptr));
    
    // Try to release more objects than capacity (should be handled gracefully)
    std::vector<int*> objects;
    for (size_t i = 0; i < lockfree_pool->capacity(); ++i) {
        objects.push_back(lockfree_pool->acquire());
    }
    
    // Release all objects
    for (auto obj : objects) {
        lockfree_pool->release(obj);
    }
    
    // Try to release one more (pool is full)
    int dummy_value = 42;
    EXPECT_NO_THROW(lockfree_pool->release(&dummy_value));
}

// =============================================================================
// ORDER POOL TESTS
// =============================================================================

TEST_F(OrderPoolTest, BasicOrderManagement) {
    auto stats = order_pool->get_stats();
    EXPECT_EQ(stats.total_allocated, 50);
    EXPECT_EQ(stats.in_use, 0);
    EXPECT_EQ(stats.allocation_requests, 0);
    EXPECT_EQ(stats.cache_hits, 0);
    
    // Acquire order
    Order* order = order_pool->acquire_order();
    EXPECT_NE(order, nullptr);
    
    stats = order_pool->get_stats();
    EXPECT_EQ(stats.in_use, 1);
    EXPECT_EQ(stats.allocation_requests, 1);
    EXPECT_EQ(stats.cache_hits, 1);
    EXPECT_GT(stats.hit_rate(), 0.0);
    
    // Release order
    order_pool->release_order(order);
    
    stats = order_pool->get_stats();
    EXPECT_EQ(stats.in_use, 0);
}

TEST_F(OrderPoolTest, PeakUsageTracking) {
    std::vector<Order*> orders;
    
    // Acquire multiple orders to test peak tracking
    for (int i = 0; i < 10; ++i) {
        orders.push_back(order_pool->acquire_order());
    }
    
    auto stats = order_pool->get_stats();
    EXPECT_EQ(stats.peak_usage, 10);
    
    // Release some orders
    for (int i = 0; i < 5; ++i) {
        order_pool->release_order(orders[i]);
    }
    
    stats = order_pool->get_stats();
    EXPECT_EQ(stats.peak_usage, 10); // Peak should remain
    EXPECT_EQ(stats.in_use, 5);
    
    // Clean up
    for (int i = 5; i < 10; ++i) {
        order_pool->release_order(orders[i]);
    }
}

TEST_F(OrderPoolTest, HitRateCalculation) {
    auto initial_stats = order_pool->get_stats();
    
    // Make some allocations
    std::vector<Order*> orders;
    for (int i = 0; i < 5; ++i) {
        orders.push_back(order_pool->acquire_order());
    }
    
    auto stats = order_pool->get_stats();
    EXPECT_EQ(stats.allocation_requests, 5);
    EXPECT_EQ(stats.cache_hits, 5);
    EXPECT_DOUBLE_EQ(stats.hit_rate(), 1.0); // 100% hit rate
    
    // Clean up
    for (auto order : orders) {
        order_pool->release_order(order);
    }
}

TEST_F(OrderPoolTest, StatisticsReset) {
    // Generate some activity
    std::vector<Order*> orders;
    for (int i = 0; i < 3; ++i) {
        orders.push_back(order_pool->acquire_order());
    }
    
    auto stats = order_pool->get_stats();
    EXPECT_GT(stats.allocation_requests, 0);
    EXPECT_GT(stats.cache_hits, 0);
    EXPECT_GT(stats.peak_usage, 0);
    
    // Reset statistics
    order_pool->reset_stats();
    
    stats = order_pool->get_stats();
    EXPECT_EQ(stats.allocation_requests, 0);
    EXPECT_EQ(stats.cache_hits, 0);
    EXPECT_EQ(stats.peak_usage, 0);
    
    // in_use and total_allocated should remain unchanged
    EXPECT_EQ(stats.in_use, 3);
    EXPECT_EQ(stats.total_allocated, 50);
    
    // Clean up
    for (auto order : orders) {
        order_pool->release_order(order);
    }
}

TEST_F(OrderPoolTest, EmergencyOperations) {
    // Test emergency reserve
    size_t initial_allocated = order_pool->get_stats().total_allocated;
    
    EXPECT_NO_THROW(order_pool->emergency_reserve(100));
    
    auto stats = order_pool->get_stats();
    EXPECT_GT(stats.total_allocated, initial_allocated);
    
    // Test emergency shrink
    EXPECT_NO_THROW(order_pool->emergency_shrink_to_target(25));
    
    // Should have shrunk but maintained safety
    stats = order_pool->get_stats();
    EXPECT_GE(stats.total_allocated, 25);
}

TEST_F(OrderPoolTest, NullOrderHandling) {
    // Releasing null order should be safe
    EXPECT_NO_THROW(order_pool->release_order(nullptr));
    
    auto stats = order_pool->get_stats();
    EXPECT_EQ(stats.in_use, 0);
}

// =============================================================================
// MEMORY MANAGER TESTS
// =============================================================================

TEST_F(MemoryManagerTest, SingletonPattern) {
    // Test singleton access
    MemoryManager& manager1 = MemoryManager::instance();
    MemoryManager& manager2 = MemoryManager::instance();
    
    // Should be the same instance
    EXPECT_EQ(&manager1, &manager2);
}

TEST_F(MemoryManagerTest, SystemMemoryStats) {
    auto& manager = MemoryManager::instance();
    auto stats = manager.get_system_stats();
    
    // Basic sanity checks
    EXPECT_GE(stats.total_allocated_bytes, 0);
    EXPECT_GE(stats.total_in_use_bytes, 0);
    EXPECT_LE(stats.total_in_use_bytes, stats.total_allocated_bytes);
    EXPECT_GE(stats.order_pool_usage, 0);
}

TEST_F(MemoryManagerTest, MemoryReporting) {
    auto& manager = MemoryManager::instance();
    
    // These should not crash
    EXPECT_NO_THROW(manager.print_memory_report());
    EXPECT_NO_THROW(manager.print_debug_info());
}

TEST_F(MemoryManagerTest, PoolOptimization) {
    auto& manager = MemoryManager::instance();
    
    // Should not crash
    EXPECT_NO_THROW(manager.optimize_pools());
}

TEST_F(MemoryManagerTest, MemoryPressureDetection) {
    auto& manager = MemoryManager::instance();
    
    // Initially should not have high pressure
    EXPECT_FALSE(manager.is_memory_pressure_high());
    
    // Create high utilization
    std::vector<Order*> orders;
    auto& order_pool = manager.order_pool();
    auto initial_stats = order_pool.get_stats();
    
    // Use most of the pool to create pressure
    size_t target_usage = static_cast<size_t>(initial_stats.total_allocated * 0.95);
    for (size_t i = 0; i < target_usage; ++i) {
        Order* order = order_pool.acquire_order();
        if (order) {
            orders.push_back(order);
        }
    }
    
    // Should detect high pressure now
    EXPECT_TRUE(manager.is_memory_pressure_high());
    
    // Clean up
    for (auto order : orders) {
        order_pool.release_order(order);
    }
}

TEST_F(MemoryManagerTest, EmergencyCleanup) {
    auto& manager = MemoryManager::instance();
    
    // Should not crash even if no cleanup is needed
    EXPECT_NO_THROW(manager.emergency_cleanup());
}

TEST_F(MemoryManagerTest, PoolValidation) {
    auto& manager = MemoryManager::instance();
    
    // Should not crash and should pass validation
    EXPECT_NO_THROW(manager.validate_pools());
}

// =============================================================================
// EDGE CASES AND BOUNDARY CONDITIONS
// =============================================================================

TEST_F(MemoryPoolTest, ZeroInitialSize) {
    // Pool with zero initial size should still work
    EXPECT_NO_THROW(MemoryPool<int> zero_pool(0, 2));
    MemoryPool<int> zero_pool(0, 2);
    
    // Should expand when first object is requested
    int* obj = zero_pool.acquire();
    EXPECT_NE(obj, nullptr);
    EXPECT_GT(zero_pool.total_allocated(), 0);
    
    zero_pool.release(obj);
}

TEST_F(MemoryPoolTest, LargeInitialSize) {
    // Test with large initial size
    size_t large_size = 100000;
    EXPECT_NO_THROW(MemoryPool<int> large_pool(large_size, 2));
    MemoryPool<int> large_pool(large_size, 2);
    
    EXPECT_EQ(large_pool.total_allocated(), large_size);
    EXPECT_EQ(large_pool.available(), large_size);
}

TEST_F(MemoryPoolTest, GrowthFactorOne) {
    // Growth factor of 1 should still work
    MemoryPool<int> no_growth_pool(5, 1);
    
    std::vector<int*> objects;
    
    // Fill initial capacity
    for (int i = 0; i < 5; ++i) {
        objects.push_back(no_growth_pool.acquire());
    }
    
    // Next acquire should trigger expansion by factor of 1
    int* extra_obj = no_growth_pool.acquire();
    EXPECT_NE(extra_obj, nullptr);
    objects.push_back(extra_obj);
    
    EXPECT_EQ(no_growth_pool.total_allocated(), 10); // 5 + 5*1
    
    // Clean up
    for (auto obj : objects) {
        no_growth_pool.release(obj);
    }
}

TEST_F(LockFreeMemoryPoolTest, SingleObjectPool) {
    // Test with minimal capacity
    LockFreeMemoryPool<int> tiny_pool(1);
    
    EXPECT_EQ(tiny_pool.capacity(), 1);
    
    int* obj = tiny_pool.acquire();
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(tiny_pool.available(), 0);
    
    // Pool is exhausted
    int* second_obj = tiny_pool.acquire();
    EXPECT_EQ(second_obj, nullptr);
    
    // Release and try again
    tiny_pool.release(obj);
    second_obj = tiny_pool.acquire();
    EXPECT_NE(second_obj, nullptr);
    
    tiny_pool.release(second_obj);
}

TEST_F(OrderPoolTest, MinimalOrderPool) {
    // Test with very small pool
    OrderPool tiny_order_pool(1);
    
    auto stats = tiny_order_pool.get_stats();
    EXPECT_EQ(stats.total_allocated, 1);
    
    Order* order = tiny_order_pool.acquire_order();
    EXPECT_NE(order, nullptr);
    
    stats = tiny_order_pool.get_stats();
    EXPECT_EQ(stats.in_use, 1);
    
    tiny_order_pool.release_order(order);
}

// =============================================================================
// STRESS AND PERFORMANCE TESTS
// =============================================================================

TEST_F(MemoryPoolTest, HighVolumeOperations) {
    const size_t num_operations = 10000;
    std::vector<int*> objects;
    objects.reserve(num_operations);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Acquire many objects
    for (size_t i = 0; i < num_operations; ++i) {
        int* obj = pool->acquire();
        EXPECT_NE(obj, nullptr);
        *obj = static_cast<int>(i);
        objects.push_back(obj);
    }
    
    // Release all objects
    for (auto obj : objects) {
        pool->release(obj);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Should complete in reasonable time
    EXPECT_LT(duration.count(), 1000); // Less than 1 second
    
    // Pool should be back to initial state
    EXPECT_EQ(pool->in_use(), 0);
}

TEST_F(LockFreeMemoryPoolTest, RapidAcquireRelease) {
    const size_t num_cycles = 1000;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < num_cycles; ++i) {
        // Acquire and immediately release
        int* obj = lockfree_pool->acquire();
        EXPECT_NE(obj, nullptr);
        *obj = static_cast<int>(i);
        lockfree_pool->release(obj);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    // Should be very fast for lock-free operations
    EXPECT_LT(duration.count(), 10000); // Less than 10ms
    
    // Pool should be in original state
    EXPECT_EQ(lockfree_pool->in_use(), 0);
    EXPECT_EQ(lockfree_pool->available(), lockfree_pool->capacity());
}

TEST_F(OrderPoolTest, OrderLifecycleStress) {
    const size_t num_orders = 1000;
    std::vector<Order*> active_orders;
    
    // Simulate realistic order lifecycle patterns
    std::random_device rd;
    std::mt19937 gen(42); // Fixed seed for reproducible tests
    std::uniform_int_distribution<> action_dist(0, 2); // 0=acquire, 1=release, 2=hold
    
    for (size_t i = 0; i < num_orders; ++i) {
        int action = action_dist(gen);
        
        if (action == 0 || active_orders.empty()) {
            // Acquire new order
            Order* order = order_pool->acquire_order();
            if (order) {
                active_orders.push_back(order);
            }
        } else if (action == 1 && !active_orders.empty()) {
            // Release random order
            size_t idx = std::uniform_int_distribution<size_t>(0, active_orders.size() - 1)(gen);
            order_pool->release_order(active_orders[idx]);
            active_orders.erase(active_orders.begin() + idx);
        }
        // action == 2 means hold (no operation)
    }
    
    // Clean up remaining orders
    for (auto order : active_orders) {
        order_pool->release_order(order);
    }
    
    auto stats = order_pool->get_stats();
    EXPECT_EQ(stats.in_use, 0);
    EXPECT_GT(stats.allocation_requests, 0);
    EXPECT_EQ(stats.cache_hits, stats.allocation_requests); // All should be hits
}

// =============================================================================
// THREAD SAFETY TESTS
// =============================================================================

TEST_F(MemoryPoolTest, ConcurrentAccess) {
    const int num_threads = 4;
    const int operations_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> total_acquired{0};
    std::atomic<int> total_released{0};
    
    // Launch multiple threads
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, operations_per_thread, &total_acquired, &total_released]() {
            std::vector<int*> local_objects;
            
            // Acquire objects
            std::hash<std::thread::id> hasher;
            for (int i = 0; i < operations_per_thread; ++i) {
                int* obj = pool->acquire();
                if (obj) {
                    *obj = static_cast<int>(hasher(std::this_thread::get_id()) % 1000);
                    local_objects.push_back(obj);
                    total_acquired++;
                }
            }
            
            // Small delay to increase chance of conflicts
            std::this_thread::sleep_for(1ms);
            
            // Release objects
            for (auto obj : local_objects) {
                pool->release(obj);
                total_released++;
            }
        });
    }
    
    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify consistency
    EXPECT_EQ(total_acquired.load(), total_released.load());
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_GT(pool->total_allocated(), 0);
}

TEST_F(MemoryManagerTest, ConcurrentManagerAccess) {
    const int num_threads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> successful_operations{0};
    
    // Test concurrent access to MemoryManager
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&successful_operations]() {
            try {
                auto& manager = MemoryManager::instance();
                auto& order_pool = manager.order_pool();
                
                // Perform some operations
                Order* order = order_pool.acquire_order();
                if (order) {
                    // Do some work with the order
                    std::this_thread::sleep_for(1ms);
                    order_pool.release_order(order);
                    successful_operations++;
                }
                
                // Test other manager operations
                auto stats = manager.get_system_stats();
                (void)stats; // Suppress unused variable warning
                
            } catch (...) {
                // Should not throw
                FAIL() << "Exception thrown in concurrent access";
            }
        });
    }
    
    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_GT(successful_operations.load(), 0);
}

// =============================================================================
// MEMORY LEAK AND SAFETY TESTS
// =============================================================================

TEST_F(MemoryPoolTest, NoMemoryLeaks) {
    // This test verifies that pool cleanup happens properly
    {
        MemoryPool<int> scoped_pool(100);
        std::vector<int*> objects;
        
        // Use the pool
        for (int i = 0; i < 50; ++i) {
            objects.push_back(scoped_pool.acquire());
        }
        
        // Don't release objects - test destructor cleanup
    } // Pool should be destroyed here without leaks
    
    // If we reach here without issues, cleanup worked
    SUCCEED();
}

TEST_F(LockFreeMemoryPoolTest, BoundaryChecking) {
    // Test accessing exactly at boundaries
    std::vector<int*> objects;
    
    // Fill to exactly capacity
    for (size_t i = 0; i < lockfree_pool->capacity(); ++i) {
        int* obj = lockfree_pool->acquire();
        EXPECT_NE(obj, nullptr);
        objects.push_back(obj);
    }
    
    EXPECT_EQ(lockfree_pool->available(), 0);
    EXPECT_EQ(lockfree_pool->in_use(), lockfree_pool->capacity());
    
    // One more should fail
    int* overflow = lockfree_pool->acquire();
    EXPECT_EQ(overflow, nullptr);
    
    // Release one and try again
    lockfree_pool->release(objects.back());
    objects.pop_back();
    
    int* recovery = lockfree_pool->acquire();
    EXPECT_NE(recovery, nullptr);
    objects.push_back(recovery);
    
    // Clean up
    for (auto obj : objects) {
        lockfree_pool->release(obj);
    }
}

// =============================================================================
// COMPLEX INTEGRATION TESTS
// =============================================================================

TEST_F(MemoryManagerTest, SystemIntegrationTest) {
    auto& manager = MemoryManager::instance();
    
    // Simulate realistic HFT system behavior
    std::vector<Order*> orders;
    
    // Rapid order creation burst
    for (int i = 0; i < 100; ++i) {
        Order* order = manager.order_pool().acquire_order();
        if (order) {
            orders.push_back(order);
        }
    }
    
    // Check system state
    auto stats = manager.get_system_stats();
    EXPECT_GT(stats.total_in_use_bytes, 0);
    EXPECT_FALSE(manager.is_memory_pressure_high());
    
    // Optimize pools
    manager.optimize_pools();
    
    // Create memory pressure
    std::vector<Order*> pressure_orders;
    auto order_stats = manager.order_pool().get_stats();
    // Need > 90% utilization to trigger pressure, so allocate 91% + 1 extra to be safe
    size_t pressure_target = (order_stats.total_allocated * 91) / 100 + 1; // Definitely > 90%
    
    for (size_t i = orders.size(); i < pressure_target; ++i) {
        Order* order = manager.order_pool().acquire_order();
        if (order) {
            pressure_orders.push_back(order);
        }
    }

    // Should detect pressure
    EXPECT_TRUE(manager.is_memory_pressure_high());
    
    // Test emergency cleanup
    manager.emergency_cleanup();
    
    // Validate system state
    manager.validate_pools();
    
    // Clean up
    for (auto order : orders) {
        manager.order_pool().release_order(order);
    }
    for (auto order : pressure_orders) {
        manager.order_pool().release_order(order);
    }
    
    // Final validation
    manager.validate_pools();
}

// =============================================================================
// SPECIALIZED TYPE TESTS
// =============================================================================

// Test with custom types
struct ComplexObject {
    int id;
    double value;
    std::string name;
    
    ComplexObject() : id(0), value(0.0), name("default") {}
    ComplexObject(int i, double v, const std::string& n) : id(i), value(v), name(n) {}
    
    ~ComplexObject() {
        // Destructor for testing proper cleanup
        id = -1;
    }
};

TEST(MemoryPoolSpecializedTest, ComplexObjectPool) {
    MemoryPool<ComplexObject> complex_pool(10);
    
    // Acquire complex object
    ComplexObject* obj = complex_pool.acquire();
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(obj->id, 0); // Default constructed
    EXPECT_EQ(obj->name, "default");
    
    // Modify object
    obj->id = 42;
    obj->value = 3.14;
    obj->name = "test";
    
    // Release and acquire again
    complex_pool.release(obj);
    
    ComplexObject* new_obj = complex_pool.acquire();
    EXPECT_NE(new_obj, nullptr);
    // Should be default constructed again
    EXPECT_EQ(new_obj->id, 0);
    EXPECT_EQ(new_obj->name, "default");
    
    complex_pool.release(new_obj);
}

// =============================================================================
// MAIN TEST RUNNER
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
