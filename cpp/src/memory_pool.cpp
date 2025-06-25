#include "memory_pool.hpp"
#include "types.hpp"
#include <iostream>
#include <iomanip>

namespace hft {

// OrderPool implementation
OrderPool::OrderPool(size_t initial_size) 
    : pool_(initial_size), peak_usage_(0), allocation_requests_(0), cache_hits_(0) {
}

Order* OrderPool::acquire_order() {
    allocation_requests_.fetch_add(1);
    
    Order* order = pool_.acquire();
    if (order) {
        cache_hits_.fetch_add(1);
        
        // Update peak usage tracking
        size_t current_usage = pool_.in_use();
        size_t current_peak = peak_usage_.load();
        while (current_usage > current_peak && 
               !peak_usage_.compare_exchange_weak(current_peak, current_usage)) {
            // Retry if another thread updated peak_usage
        }
    }
    
    return order;
}

void OrderPool::release_order(Order* order) {
    if (order) {
        pool_.release(order);
    }
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

// MemoryManager implementation
std::unique_ptr<MemoryManager> MemoryManager::instance_ = nullptr;
std::once_flag MemoryManager::init_flag_;

MemoryManager::MemoryManager() : order_pool_(1000), peak_memory_usage_(0) {
}

MemoryManager& MemoryManager::instance() {
    std::call_once(init_flag_, []() {
        instance_ = std::unique_ptr<MemoryManager>(new MemoryManager());
    });
    return *instance_;
}

MemoryManager::SystemMemoryStats MemoryManager::get_system_stats() const {
    SystemMemoryStats stats;
    
    // Order pool statistics
    auto order_stats = order_pool_.get_stats();
    stats.order_pool_usage = order_stats.in_use;
    
    // Calculate total allocated bytes (rough estimate)
    stats.total_allocated_bytes = order_stats.total_allocated * sizeof(Order);
    stats.total_in_use_bytes = order_stats.in_use * sizeof(Order);
    
    stats.peak_memory_usage = peak_memory_usage_.load();
    
    // Update peak if current usage is higher
    if (stats.total_in_use_bytes > stats.peak_memory_usage) {
        peak_memory_usage_.store(stats.total_in_use_bytes);
        stats.peak_memory_usage = stats.total_in_use_bytes;
    }
    
    return stats;
}

void MemoryManager::print_memory_report() const {
    auto stats = get_system_stats();
    auto order_stats = order_pool_.get_stats();
    
    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "🧠 MEMORY POOL PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    
    // System-wide statistics
    std::cout << "System Memory:" << std::endl;
    std::cout << "  Total Allocated: " << std::fixed << std::setprecision(2) 
              << stats.total_allocated_bytes / 1024.0 << " KB" << std::endl;
    std::cout << "  Currently In Use: " << std::fixed << std::setprecision(2)
              << stats.total_in_use_bytes / 1024.0 << " KB" << std::endl;
    std::cout << "  Peak Usage: " << std::fixed << std::setprecision(2)
              << stats.peak_memory_usage / 1024.0 << " KB" << std::endl;
    
    std::cout << "\nOrder Pool Statistics:" << std::endl;
    std::cout << "  Total Allocated: " << order_stats.total_allocated << " orders" << std::endl;
    std::cout << "  Currently In Use: " << order_stats.in_use << " orders" << std::endl;
    std::cout << "  Peak Usage: " << order_stats.peak_usage << " orders" << std::endl;
    std::cout << "  Allocation Requests: " << order_stats.allocation_requests << std::endl;
    std::cout << "  Cache Hit Rate: " << std::fixed << std::setprecision(2) 
              << order_stats.hit_rate() * 100 << "%" << std::endl;
    
    // Memory efficiency metrics
    double memory_efficiency = stats.total_in_use_bytes > 0 ? 
        static_cast<double>(stats.total_in_use_bytes) / stats.total_allocated_bytes * 100 : 0;
    
    std::cout << "\nEfficiency Metrics:" << std::endl;
    std::cout << "  Memory Utilization: " << std::fixed << std::setprecision(1)
              << memory_efficiency << "%" << std::endl;
    
    if (order_stats.allocation_requests > 0) {
        std::cout << "  Pool Performance: ";
        if (order_stats.hit_rate() > 0.95) {
            std::cout << "Excellent ✅" << std::endl;
        } else if (order_stats.hit_rate() > 0.90) {
            std::cout << "Good 👍" << std::endl;
        } else if (order_stats.hit_rate() > 0.80) {
            std::cout << "Fair ⚠️" << std::endl;
        } else {
            std::cout << "Poor ❌" << std::endl;
        }
    }
    
    std::cout << std::string(50, '=') << std::endl;
}

// Template specializations for common types (if needed)
template<>
MemoryPool<Order>& MemoryManager::get_pool<Order>() {
    // Return the specialized order pool's underlying memory pool
    // This is a simplified approach - in practice you might want 
    // to expose the OrderPool directly
    static MemoryPool<Order> order_memory_pool(1000);
    return order_memory_pool;
}

} // namespace hft
