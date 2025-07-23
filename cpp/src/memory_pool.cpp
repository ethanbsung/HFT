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
      Order* order = pool_.acquire();
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
// MEMORY MANAGEMENT (COMPATIBLE WITH TEMPLATE OPTIMIZATION)
// =============================================================================

void OrderPool::shrink_to_fit(size_t target_objects) {
    std::cout << "[ORDER POOL] 🔧 Shrinking to " << target_objects << " objects" << std::endl;
    pool_.shrink_to_fit(target_objects);
}

void OrderPool::reserve(size_t additional_objects) {
    std::cout << "[ORDER POOL] 🔧 Reserving " << additional_objects << " additional objects" << std::endl;
    pool_.reserve(additional_objects);
}

// =============================================================================
// EMERGENCY CLEANUP INTERFACE
// =============================================================================

void OrderPool::emergency_shrink_to_target(size_t target_objects) {
    std::cout << "[ORDER POOL EMERGENCY] 🚨 Emergency shrink to " << target_objects << " objects" << std::endl;
    
    // Get current stats for logging
    auto stats = get_stats();
    std::cout << "[ORDER POOL EMERGENCY] Current: " << stats.total_allocated 
              << " allocated, " << stats.in_use << " in use" << std::endl;
    
    // Call underlying pool's shrink method
    pool_.shrink_to_fit(target_objects);
    
    // Log result
    auto new_stats = get_stats();
    std::cout << "[ORDER POOL EMERGENCY] After shrink: " << new_stats.total_allocated 
              << " allocated (freed " << (stats.total_allocated - new_stats.total_allocated) 
              << " objects)" << std::endl;
}

void OrderPool::emergency_reserve(size_t additional_objects) {
    std::cout << "[ORDER POOL EMERGENCY] 🚨 Emergency reserve " << additional_objects << " additional objects" << std::endl;
    
    auto stats = get_stats();
    std::cout << "[ORDER POOL EMERGENCY] Current: " << stats.total_allocated 
              << " allocated, requesting " << additional_objects << " more" << std::endl;
    
    // Call underlying pool's reserve method
    pool_.reserve(additional_objects);
    
    // Log result
    auto new_stats = get_stats();
    std::cout << "[ORDER POOL EMERGENCY] After reserve: " << new_stats.total_allocated 
              << " allocated (added " << (new_stats.total_allocated - stats.total_allocated) 
              << " objects)" << std::endl;
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

template<typename T>
MemoryPool<T>& MemoryManager::get_pool() {
    // For now, this is a placeholder implementation
    // In a full HFT system, you'd maintain a map of pools by type
    static MemoryPool<T> generic_pool(1000);
    return generic_pool;
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
    auto stats = pool.get_stats();

    if (stats.peak_usage < (0.5 * stats.total_allocated)) {
        size_t target_size = static_cast<size_t>(stats.peak_usage * 1.2);
        
        std::cout << "[POOL OPTIMIZE] Peak usage: " << stats.peak_usage 
                  << ", Total allocated: " << stats.total_allocated 
                  << ", Target size: " << target_size << std::endl;
        std::cout << "[POOL OPTIMIZE] Pool underutilized, shrinking..." << std::endl;
        
        pool.shrink_to_fit(target_size);
        
    } else if (stats.in_use > (0.8 * stats.total_allocated)) {
        std::cout << "[POOL OPTIMIZE] In use: " << stats.in_use 
                  << ", Total allocated: " << stats.total_allocated << std::endl;
        std::cout << "[POOL OPTIMIZE] Pool overutilized, expanding..." << std::endl;
        
        pool.reserve(stats.total_allocated);
        
    } else if (stats.hit_rate() < 0.9) {
        std::cout << "[POOL OPTIMIZE] Hit rate low: " << (stats.hit_rate() * 100.0) 
                  << "%, expanding pool..." << std::endl;
        
        pool.reserve(stats.total_allocated / 2);
    }
}

void MemoryManager::optimize_pools() {
    std::cout << "[MEMORY MANAGER] 🔧 Optimizing all pools..." << std::endl;
    
    // Optimize the order pool
    optimize_pools(order_pool_);
    
    std::cout << "[MEMORY MANAGER] ✅ Pool optimization complete" << std::endl;
}

bool MemoryManager::is_memory_pressure_high() const {
    // Check order pool pressure
    auto order_stats = order_pool_.get_stats();
    
    // Critical threshold: > 90% utilization
    double utilization_rate = static_cast<double>(order_stats.in_use) / order_stats.total_allocated;
    if (utilization_rate > 0.9) {
        std::cout << "[MEMORY PRESSURE] Order pool utilization critical: " 
                  << (utilization_rate * 100.0) << "%" << std::endl;
        return true;
    }
    
    // Available objects threshold: < 10% of total or < 100 objects (whichever is larger)
    size_t available_objects = order_stats.total_allocated - order_stats.in_use;
    size_t minimum_available = std::max(
        static_cast<size_t>(order_stats.total_allocated * 0.1),  // 10% of total
        static_cast<size_t>(100)  // At least 100 objects
    );
    
    if (available_objects < minimum_available) {
        std::cout << "[MEMORY PRESSURE] Available objects critically low: " 
                  << available_objects << " (minimum: " << minimum_available << ")" << std::endl;
        return true;
    }
    
    // Hit rate degradation: < 85% (indicates frequent pool expansion stress)
    double hit_rate = order_stats.hit_rate();
    if (hit_rate < 0.85 && order_stats.allocation_requests > 1000) {  // Only check if significant activity
        std::cout << "[MEMORY PRESSURE] Hit rate degraded: " 
                  << (hit_rate * 100.0) << "% (requests: " << order_stats.allocation_requests << ")" << std::endl;
        return true;
    }
    
    // System-wide memory check
    auto system_stats = get_system_stats();
    
    // Peak memory pressure: current usage approaching peak
    if (system_stats.peak_memory_usage > 0) {
        double peak_ratio = static_cast<double>(system_stats.total_in_use_bytes) / system_stats.peak_memory_usage;
        if (peak_ratio > 0.95) {
            std::cout << "[MEMORY PRESSURE] Approaching peak memory usage: " 
                      << (peak_ratio * 100.0) << "%" << std::endl;
            return true;
        }
    }
    
    // No pressure detected
    return false;
}

void MemoryManager::emergency_cleanup() {
    std::cout << "\n[EMERGENCY CLEANUP] ⚠️  Initiating emergency memory cleanup..." << std::endl;
    
    auto system_stats_before = get_system_stats();
    std::cout << "[EMERGENCY CLEANUP] Pre-cleanup memory: " 
              << (system_stats_before.total_allocated_bytes / 1024.0) << " KB allocated, "
              << (system_stats_before.total_in_use_bytes / 1024.0) << " KB in use" << std::endl;
    
    // 1. AGGRESSIVE POOL SHRINKING
    auto order_stats = order_pool_.get_stats();
    
    // Emergency threshold: much more aggressive than normal optimization
    // Shrink if current usage < 70% of allocated (vs 50% in normal optimization)
    double utilization = static_cast<double>(order_stats.in_use) / order_stats.total_allocated;
    
    if (utilization < 0.7) {
        // Emergency target: current usage + 50% buffer (vs 20% in normal)
        size_t emergency_target = static_cast<size_t>(order_stats.in_use * 1.5);
        
        std::cout << "[EMERGENCY CLEANUP] 🔥 Aggressive shrinking - utilization: " 
                  << (utilization * 100.0) << "%, target: " << emergency_target << std::endl;
        
        // Execute emergency shrinking
        order_pool_.emergency_shrink_to_target(emergency_target);
    }
    
    // 2. RESET POTENTIALLY STALE STATISTICS
    std::cout << "[EMERGENCY CLEANUP] 🔄 Resetting stale statistics..." << std::endl;
    
    // Reset peak usage (might be from an unusual spike)
    order_pool_.reset_stats();
    
    // Reset system peak memory usage
    peak_memory_usage_.store(system_stats_before.total_in_use_bytes);
    
    // 3. VALIDATE POOL INTEGRITY
    std::cout << "[EMERGENCY CLEANUP] ✅ Validating pool integrity..." << std::endl;
    validate_pools();
    
    // 4. FORCE IMMEDIATE OPTIMIZATION
    // Run optimization with emergency parameters
    std::cout << "[EMERGENCY CLEANUP] ⚡ Running emergency optimization..." << std::endl;
    
    // Note: We can't directly call optimize_pools on order_pool_ because it's not the right type
    // We would need to modify the design or add specific emergency optimization
    auto post_reset_stats = order_pool_.get_stats();
    
    // Emergency expansion if we're still critically low after cleanup
    if (post_reset_stats.in_use > post_reset_stats.total_allocated * 0.85) {
        size_t emergency_expansion = post_reset_stats.total_allocated; // Double the pool
        std::cout << "[EMERGENCY CLEANUP] 📈 Still under pressure, emergency expansion by " 
                  << emergency_expansion << " objects..." << std::endl;
        
        order_pool_.emergency_reserve(emergency_expansion);
    }
    
    // 5. LOG CLEANUP RESULTS
    auto system_stats_after = get_system_stats();
    
    size_t memory_freed = system_stats_before.total_allocated_bytes - system_stats_after.total_allocated_bytes;
    
    std::cout << "[EMERGENCY CLEANUP] 📊 Cleanup complete!" << std::endl;
    std::cout << "[EMERGENCY CLEANUP] Memory freed: " << (memory_freed / 1024.0) << " KB" << std::endl;
    std::cout << "[EMERGENCY CLEANUP] New allocation: " 
              << (system_stats_after.total_allocated_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "[EMERGENCY CLEANUP] New utilization: " 
              << (static_cast<double>(system_stats_after.total_in_use_bytes) / 
                  system_stats_after.total_allocated_bytes * 100.0) << "%" << std::endl;
    
    // 6. ALERT MONITORING SYSTEMS
    if (memory_freed > 0) {
        std::cout << "[EMERGENCY CLEANUP] ✅ Emergency cleanup successful - " 
                  << (memory_freed / 1024.0) << " KB recovered" << std::endl;
    } else {
        std::cout << "[EMERGENCY CLEANUP] ⚠️  No memory could be freed - system may need external intervention" << std::endl;
    }
    
    std::cout << "[EMERGENCY CLEANUP] 🏁 Emergency cleanup completed\n" << std::endl;
}

// =============================================================================
// DEBUGGING AND MONITORING HELPERS
// =============================================================================

void MemoryManager::print_debug_info() const {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "🔧 MEMORY MANAGER DEBUG INFORMATION" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    // System-wide memory information
    auto system_stats = get_system_stats();
    std::cout << "\n📊 SYSTEM MEMORY STATE:" << std::endl;
    std::cout << "   Total Allocated: " << system_stats.total_allocated_bytes << " bytes ("
              << (system_stats.total_allocated_bytes / 1024.0) << " KB)" << std::endl;
    std::cout << "   Total In Use: " << system_stats.total_in_use_bytes << " bytes ("
              << (system_stats.total_in_use_bytes / 1024.0) << " KB)" << std::endl;
    std::cout << "   Peak Memory Usage: " << system_stats.peak_memory_usage << " bytes ("
              << (system_stats.peak_memory_usage / 1024.0) << " KB)" << std::endl;
    std::cout << "   Order Pool Usage: " << system_stats.order_pool_usage << " orders" << std::endl;
    
    // Detailed order pool information
    auto order_stats = order_pool_.get_stats();
    std::cout << "\n🎯 ORDER POOL DETAILED STATE:" << std::endl;
    std::cout << "   Pool Address: " << &order_pool_ << std::endl;
    std::cout << "   Total Allocated: " << order_stats.total_allocated << " orders" << std::endl;
    std::cout << "   Currently In Use: " << order_stats.in_use << " orders" << std::endl;
    std::cout << "   Available Objects: " << (order_stats.total_allocated - order_stats.in_use) << " orders" << std::endl;
    std::cout << "   Peak Usage: " << order_stats.peak_usage << " orders" << std::endl;
    std::cout << "   Allocation Requests: " << order_stats.allocation_requests << std::endl;
    std::cout << "   Cache Hits: " << order_stats.cache_hits << std::endl;
    std::cout << "   Hit Rate: " << std::fixed << std::setprecision(2) 
              << (order_stats.hit_rate() * 100.0) << "%" << std::endl;
    
    // Memory utilization analysis
    std::cout << "\n📈 UTILIZATION ANALYSIS:" << std::endl;
    if (order_stats.total_allocated > 0) {
        double current_util = static_cast<double>(order_stats.in_use) / order_stats.total_allocated * 100.0;
        double peak_util = static_cast<double>(order_stats.peak_usage) / order_stats.total_allocated * 100.0;
        
        std::cout << "   Current Utilization: " << std::fixed << std::setprecision(1) 
                  << current_util << "%" << std::endl;
        std::cout << "   Peak Utilization: " << std::fixed << std::setprecision(1) 
                  << peak_util << "%" << std::endl;
        
        // Memory pressure indicators
        if (current_util > 90.0) {
            std::cout << "   ⚠️  CRITICAL: High memory pressure detected!" << std::endl;
        } else if (current_util > 80.0) {
            std::cout << "   ⚠️  WARNING: Approaching high utilization" << std::endl;
        } else if (current_util < 30.0) {
            std::cout << "   💡 INFO: Pool may be over-allocated" << std::endl;
        } else {
            std::cout << "   ✅ INFO: Utilization within normal range" << std::endl;
        }
    }
    
    // Access to underlying pool details via friend relationship
    const auto& underlying_pool = order_pool_.pool_;
    std::cout << "\n🔍 UNDERLYING MEMORY POOL STATE:" << std::endl;
    std::cout << "   Pool Address: " << &underlying_pool << std::endl;
    std::cout << "   Total Allocated (atomic): " << underlying_pool.total_allocated() << " objects" << std::endl;
    std::cout << "   Available Objects: " << underlying_pool.available() << " objects" << std::endl;
    std::cout << "   Objects In Use: " << underlying_pool.in_use() << " objects" << std::endl;
    
    // Memory block information
    std::cout << "\n🧱 MEMORY BLOCK DETAILS:" << std::endl;
    std::cout << "   Number of Blocks: " << underlying_pool.memory_blocks_.size() << std::endl;
    std::cout << "   Block Size: " << underlying_pool.block_size_ << " objects per block" << std::endl;
    std::cout << "   Growth Factor: " << underlying_pool.growth_factor_ << "x" << std::endl;
    
    // Individual block analysis
    for (size_t i = 0; i < underlying_pool.memory_blocks_.size(); ++i) {
        const auto& block = underlying_pool.memory_blocks_[i];
        std::cout << "   Block " << i << ": Address " << block.get() 
                  << ", Size " << underlying_pool.block_size_ << " objects" << std::endl;
    }
    
    // Performance metrics
    std::cout << "\n⚡ PERFORMANCE METRICS:" << std::endl;
    if (order_stats.allocation_requests > 0) {
        double cache_efficiency = order_stats.hit_rate() * 100.0;
        std::cout << "   Cache Efficiency: " << std::fixed << std::setprecision(2) 
                  << cache_efficiency << "%" << std::endl;
        
        if (cache_efficiency > 95.0) {
            std::cout << "   ✅ Excellent cache performance" << std::endl;
        } else if (cache_efficiency > 90.0) {
            std::cout << "   👍 Good cache performance" << std::endl;
        } else if (cache_efficiency > 80.0) {
            std::cout << "   ⚠️  Fair cache performance - consider optimization" << std::endl;
        } else {
            std::cout << "   ❌ Poor cache performance - requires attention" << std::endl;
        }
        
        // Calculate requests per allocation (expansion frequency)
        double requests_per_alloc = static_cast<double>(order_stats.allocation_requests) / 
                                   std::max(static_cast<size_t>(1), order_stats.total_allocated);
        std::cout << "   Requests per Object: " << std::fixed << std::setprecision(2) 
                  << requests_per_alloc << std::endl;
    } else {
        std::cout << "   No allocation activity recorded" << std::endl;
    }
    
    // Memory fragmentation analysis
    std::cout << "\n🗂️  FRAGMENTATION ANALYSIS:" << std::endl;
    size_t total_objects = underlying_pool.memory_blocks_.size() * underlying_pool.block_size_;
    size_t wasted_objects = total_objects - order_stats.total_allocated;
    
    if (wasted_objects > 0) {
        double fragmentation = static_cast<double>(wasted_objects) / total_objects * 100.0;
        std::cout << "   Internal Fragmentation: " << wasted_objects << " objects (" 
                  << std::fixed << std::setprecision(1) << fragmentation << "%)" << std::endl;
    } else {
        std::cout << "   No internal fragmentation detected" << std::endl;
    }
    
    // Singleton state information
    std::cout << "\n🔧 SINGLETON STATE:" << std::endl;
    std::cout << "   MemoryManager Instance: " << this << std::endl;
    std::cout << "   Peak Memory Tracker: " << peak_memory_usage_.load() << " bytes" << std::endl;
    
    // Memory safety checks
    std::cout << "\n🛡️  MEMORY SAFETY STATUS:" << std::endl;
    bool safety_ok = true;
    
    // Check for obvious inconsistencies
    if (order_stats.total_allocated < order_stats.in_use) {
        std::cout << "   ❌ CRITICAL: total_allocated < in_use" << std::endl;
        safety_ok = false;
    }
    
    if (order_stats.cache_hits > order_stats.allocation_requests) {
        std::cout << "   ❌ CRITICAL: cache_hits > allocation_requests" << std::endl;
        safety_ok = false;
    }
    
    if (system_stats.total_in_use_bytes > system_stats.total_allocated_bytes) {
        std::cout << "   ❌ CRITICAL: system in_use > allocated" << std::endl;
        safety_ok = false;
    }
    
    if (safety_ok) {
        std::cout << "   ✅ All safety checks passed" << std::endl;
    }
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "🏁 DEBUG INFORMATION COMPLETE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
}

void MemoryManager::validate_pools() const {
    std::cout << "[POOL VALIDATION] 🔍 Validating pool integrity..." << std::endl;
    
    // Validate order pool
    auto order_stats = order_pool_.get_stats();
    
    // Check basic consistency: total_allocated should >= in_use
    if (order_stats.total_allocated < order_stats.in_use) {
        std::cout << "[POOL VALIDATION] ❌ ERROR: total_allocated (" << order_stats.total_allocated 
                  << ") < in_use (" << order_stats.in_use << ")" << std::endl;
        return;
    }
    
    // Check available count consistency
    size_t available = order_stats.total_allocated - order_stats.in_use;
    std::cout << "[POOL VALIDATION] Available objects: " << available << std::endl;
    
    // Check peak usage sanity
    if (order_stats.peak_usage > order_stats.total_allocated) {
        std::cout << "[POOL VALIDATION] ⚠️  WARNING: peak_usage (" << order_stats.peak_usage 
                  << ") > total_allocated (" << order_stats.total_allocated << ")" << std::endl;
    }
    
    // Check hit rate sanity
    if (order_stats.cache_hits > order_stats.allocation_requests) {
        std::cout << "[POOL VALIDATION] ❌ ERROR: cache_hits (" << order_stats.cache_hits 
                  << ") > allocation_requests (" << order_stats.allocation_requests << ")" << std::endl;
        return;
    }
    
    // Validate system-level consistency
    auto system_stats = get_system_stats();
    
    // Check system memory consistency
    if (system_stats.total_in_use_bytes > system_stats.total_allocated_bytes) {
        std::cout << "[POOL VALIDATION] ❌ ERROR: system in_use > allocated" << std::endl;
        return;
    }
    
    std::cout << "[POOL VALIDATION] ✅ All validation checks passed" << std::endl;
}

} // namespace hft
