#include "memory_pool.hpp"
#include "types.hpp"
#include <iostream>
#include <iomanip>

namespace hft {

// =============================================================================
// ORDER POOL IMPLEMENTATION
// =============================================================================

OrderPool::OrderPool(size_t initial_size) 
    : pool_(initial_size), peak_usage_(0), allocation_requests_(0), cache_hits_(0) {
    
    // Warm the cache
    std::vector<Order*> warmup_orders;
    warmup_orders.reserve(initial_size);
    for (size_t i = 0; i < initial_size; ++i) {
      Order* order = pool.acquire();
      warmup_orders.push_back(order);
    }
    for (Order *order : warmup_orders) {
      pool_.release(order);
    }
}

Order* OrderPool::acquire_order() {
    allocation_requests_++;
    
    Order* order = pool_.acquire();
    
    if (order) {
      cache_hits_++;

      size_t current_in_use = pool_.in_use();
      size_t prev_peak = peak_usage_.load();
      while (current_in_use > prev_peak && !peak_usage_.compare_exchange_weak(prev_peak, current_in_use)) {
        // prev_peak is updated with the latest value if the exchange fails
      }
    
    return order;
}

void OrderPool::release_order(Order* order) {
    if (!order) {
      return;
    }

    pool_.release(order);
}

OrderPool::PoolStats OrderPool::get_stats() const {
    PoolStats stats;
    stats.total_allocated = pool_.total_allocated();
    stats.in_use = pool_.in_use();
    stats.peak_usage = peak_usage_.load();
    stats.allocation_requests = allocation_requests_.load();
    stats.cache_hits = cache_hits_.load();
    return stats;
}

void OrderPool::reset_stats() {
    peak_usage_.store(0);
    allocation_requests_.store(0);
    cache_hits_.store(0);
}

// =============================================================================
// MEMORY MANAGER IMPLEMENTATION (SINGLETON PATTERN)
// =============================================================================

std::unique_ptr<MemoryManager> MemoryManager::instance_ = nullptr;
std::once_flag MemoryManager::init_flag_;

MemoryManager::MemoryManager() : order_pool_(1000), peak_memory_usage_(0) {
  std::cout << "MemoryManager initialized with 1000 order capacity" << std::endl;
}

MemoryManager& MemoryManager::instance() {
    std::call_once(init_flag_, []() {
      instance_.reset(new MemoryManager());
    });
    
    return *instance_;
}

MemoryManager::SystemMemoryStats MemoryManager::get_system_stats() const {
    SystemMemoryStats stats;

    auto order_stats = order_pool_.get_stats();

    stats.total_allocated_bytes = order_stats.total_allocated * sizeof(Order);
    stats.total_in_use_bytes = order_stats.in_use * sizeof(Order);
    stats.order_pool_usage = order_stats.in_use;
    stats.peak_memory_usage = peak_memory_usage_.load();
    
    return stats;
}

void MemoryManager::print_memory_report() const {
    // Get current system statistics
    auto system_stats = get_system_stats();
    
    // Get detailed order pool statistics
    auto order_stats = order_pool_.get_stats();
    
    // Print formatted report header
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "🧠 MEMORY POOL PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    
    // Print system memory statistics in KB
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "📊 SYSTEM MEMORY USAGE:" << std::endl;
    std::cout << "   Total Allocated: " << (system_stats.total_allocated_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "   Currently In Use: " << (system_stats.total_in_use_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "   Peak Usage: " << (system_stats.peak_memory_usage / 1024.0) << " KB" << std::endl;
    
    // Print order pool specific statistics
    std::cout << "\n📈 ORDER POOL STATISTICS:" << std::endl;
    std::cout << "   Total Orders: " << order_stats.total_allocated << std::endl;
    std::cout << "   Orders In Use: " << order_stats.in_use << std::endl;
    std::cout << "   Peak Usage: " << order_stats.peak_usage << std::endl;
    std::cout << "   Allocation Requests: " << order_stats.allocation_requests << std::endl;
    std::cout << "   Cache Hits: " << order_stats.cache_hits << std::endl;
    std::cout << "   Hit Rate: " << (order_stats.hit_rate() * 100.0) << "%" << std::endl;
    
    // Calculate and display efficiency metrics
    double memory_utilization = 0.0;
    if (system_stats.total_allocated_bytes > 0) {
        memory_utilization = (static_cast<double>(system_stats.total_in_use_bytes) / 
                             system_stats.total_allocated_bytes) * 100.0;
    }
    std::cout << "\n⚡ EFFICIENCY METRICS:" << std::endl;
    std::cout << "   Memory Utilization: " << memory_utilization << "%" << std::endl;
    
    // Provide performance assessment
    std::cout << "\n🏆 PERFORMANCE ASSESSMENT:" << std::endl;
    double hit_rate = order_stats.hit_rate() * 100.0;
    if (hit_rate > 95.0) {
        std::cout << "   Status: Excellent ✅" << std::endl;
    } else if (hit_rate > 90.0) {
        std::cout << "   Status: Good 👍" << std::endl;
    } else if (hit_rate > 80.0) {
        std::cout << "   Status: Fair ⚠️" << std::endl;
    } else {
        std::cout << "   Status: Poor ❌" << std::endl;
    }
    
    std::cout << std::string(50, '=') << std::endl;
}

// =============================================================================
// ADDITIONAL UTILITY METHODS
// =============================================================================
template<typename PoolType>
void MemoryManager::optimize_pools(PoolType& pool) {
    // TODO 27: Implement pool optimization
    // Analyze usage patterns and adjust pool sizes
    // This could involve:
    // - Shrinking underutilized pools
    // - Pre-expanding frequently used pools
    // - Defragmenting memory if needed

    if (stats.peak_usage < stats.total_allocated) pool_
    
    // TODO 28: Log optimization actions
    // In production, you'd want to log what optimizations were performed
}

bool MemoryManager::is_memory_pressure_high() const {
    // TODO 29: Detect memory pressure
    // Check if pools are running low on available objects
    // Return true if any pool is > 90% utilized
    
    // TODO 30: Consider system memory pressure
    // In a real system, you might check system memory usage
    // or monitor allocation failure rates
    
    return false; // TODO: Replace with actual pressure detection
}

void MemoryManager::emergency_cleanup() {
    // TODO 31: Implement emergency memory cleanup
    // This might be called when memory pressure is detected
    // Actions could include:
    // - Forcing garbage collection in pools
    // - Releasing unused memory blocks
    // - Triggering alerts to monitoring systems
    
    // TODO 32: Reset statistics after cleanup
    // Clear peak usage and other metrics that might be stale
}

// =============================================================================
// DEBUGGING AND MONITORING HELPERS
// =============================================================================

void MemoryManager::print_debug_info() const {
    // TODO 33: Print detailed debugging information
    // Include memory addresses, pool states, and internal counters
    // This is useful for troubleshooting memory issues
    
    std::cout << "\n=== MEMORY MANAGER DEBUG INFO ===" << std::endl;
    // TODO: Add detailed debug output
}

void MemoryManager::validate_pools() const {
    // TODO 34: Validate pool integrity
    // Check for memory leaks, corruption, or inconsistent states
    // This could involve:
    // - Verifying total_allocated == (in_use + available)
    // - Checking for null pointers in free lists
    // - Validating atomic counter consistency
    
    // TODO 35: Assert or log validation results
    // Use assert() for debug builds or logging for production
}

} // namespace hft
