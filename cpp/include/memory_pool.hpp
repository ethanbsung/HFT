#pragma once

#include "types.hpp"
#include <memory>
#include <vector>
#include <stack>
#include <mutex>
#include <cstddef>
#include <atomic>
#include <algorithm>
#include <iostream> // Added for logging in template implementations

namespace hft {

/**
 * High-performance memory pool for HFT systems
 * Pre-allocates blocks to avoid malloc/free overhead during trading
 */
template<typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t initial_size = 1000, size_t growth_factor = 2);
    ~MemoryPool();
    
    // Non-copyable, non-movable for safety
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;
    
    // Acquire and release objects
    T* acquire();
    void release(T* obj);
    
    // Pool statistics
    size_t total_allocated() const { return total_allocated_.load(); }
    size_t available() const;
    size_t in_use() const { return total_allocated() - available(); }
    
    // Memory management
    void reserve(size_t additional_capacity);
    void shrink_to_fit(size_t target_objects);
    
    // Allow MemoryManager to access private members for debugging
    friend class MemoryManager;
    
private:
    void expand_pool(size_t new_capacity);
    
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<T[]>> memory_blocks_;
    std::stack<T*> available_objects_;
    std::atomic<size_t> total_allocated_;
    size_t block_size_;
    size_t growth_factor_;
};

/**
 * Lock-free memory pool for single-threaded high-frequency operations
 * Used in critical path where mutex overhead is unacceptable
 */
template<typename T>
class LockFreeMemoryPool {
public:
    explicit LockFreeMemoryPool(size_t pool_size = 10000);
    ~LockFreeMemoryPool();
    
    // Non-copyable, non-movable
    LockFreeMemoryPool(const LockFreeMemoryPool&) = delete;
    LockFreeMemoryPool& operator=(const LockFreeMemoryPool&) = delete;
    
    // Fast acquire/release (not thread-safe)
    T* acquire();
    void release(T* obj);
    
    // Statistics
    size_t capacity() const { return capacity_; }
    size_t available() const { return next_available_; }
    size_t in_use() const { return capacity_ - available(); }
    
    // Check if pool needs expansion
    bool needs_expansion() const { return next_available_ < capacity_ * 0.1; }
    
private:
    std::unique_ptr<T[]> memory_block_;
    std::vector<T*> free_list_;
    size_t capacity_;
    size_t next_available_;
};

/**
 * Object pool specifically optimized for Order objects
 * Includes pre-warming and statistics tracking
 */
class OrderPool {
public:
    explicit OrderPool(size_t initial_size = 1000);
    ~OrderPool() = default;
    
    struct Order* acquire_order();
    void release_order(struct Order* order);
    
    // Statistics for monitoring
    struct PoolStats {
        size_t total_allocated;
        size_t in_use;
        size_t peak_usage;
        size_t allocation_requests;
        size_t cache_hits;
        double hit_rate() const { 
            return allocation_requests > 0 ? 
                static_cast<double>(cache_hits) / allocation_requests : 0.0; 
        }
    };
    
    PoolStats get_stats() const;
    void reset_stats();
    
    // Memory management (compatible with template optimization)
    void shrink_to_fit(size_t target_objects);
    void reserve(size_t additional_objects);
    
    // Emergency cleanup interface
    void emergency_shrink_to_target(size_t target_objects);
    void emergency_reserve(size_t additional_objects);
    
    // Allow MemoryManager emergency access to underlying pool
    friend class MemoryManager;
    
private:
    MemoryPool<struct Order> pool_;
    mutable std::atomic<size_t> peak_usage_;
    mutable std::atomic<size_t> allocation_requests_;
    mutable std::atomic<size_t> cache_hits_;
};

/**
 * Global memory pool manager for the entire HFT system
 * Provides centralized memory management with different pools for different object types
 */
class MemoryManager {
public:
    static MemoryManager& instance();
    
    // Object-specific pools
    OrderPool& order_pool() { return order_pool_; }
    
    // Add TradeExecution pool for efficient trade processing
    MemoryPool<TradeExecution>& trade_execution_pool() { return trade_execution_pool_; }
    
    // Generic pools for different sizes
    template<typename T>
    MemoryPool<T>& get_pool();
    
    // System-wide memory statistics
    struct SystemMemoryStats {
        size_t total_allocated_bytes;
        size_t total_in_use_bytes;
        size_t order_pool_usage;
        size_t peak_memory_usage;
    };
    
    SystemMemoryStats get_system_stats() const;
    void print_memory_report() const;
    
    // Additional utility methods for advanced learning
    void optimize_pools();
    template<typename PoolType>
    void optimize_pools(PoolType& pool);
    bool is_memory_pressure_high() const;
    void emergency_cleanup();
    void print_debug_info() const;
    void validate_pools() const;
    
public:
    ~MemoryManager() = default;
    
private:
    MemoryManager();
    
    OrderPool order_pool_;
    MemoryPool<TradeExecution> trade_execution_pool_;  // Pool for trade executions
    mutable std::atomic<size_t> peak_memory_usage_;
    
    // Singleton pattern
    static std::unique_ptr<MemoryManager> instance_;
    static std::once_flag init_flag_;
};

// Template implementations need to be in header for most compilers

template<typename T>
MemoryPool<T>::MemoryPool(size_t initial_size, size_t growth_factor)
    : total_allocated_(0), block_size_(initial_size), growth_factor_(growth_factor) {
    // Don't expand with zero size, defer expansion until first acquire()
    if (initial_size > 0) {
        expand_pool(initial_size);
    }
}

template<typename T>
MemoryPool<T>::~MemoryPool() {
    // Memory blocks are automatically cleaned up via unique_ptr
}

template<typename T>
T* MemoryPool<T>::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (available_objects_.empty()) {
        // Handle case where block_size_ is 0 (zero initial size)
        size_t expansion_size = (block_size_ == 0) ? growth_factor_ : (block_size_ * growth_factor_);
        expand_pool(expansion_size);
    }
    
    T* obj = available_objects_.top();
    available_objects_.pop();
    
    // Reset object to default state
    new(obj) T();
    
    return obj;
}

template<typename T>
void MemoryPool<T>::release(T* obj) {
    if (!obj) return;
    
    // Explicitly destroy object
    obj->~T();
    
    std::lock_guard<std::mutex> lock(mutex_);
    available_objects_.push(obj);
}

template<typename T>
size_t MemoryPool<T>::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_objects_.size();
}

template<typename T>
void MemoryPool<T>::expand_pool(size_t new_capacity) {
    auto new_block = std::make_unique<T[]>(new_capacity);
    
    // Add all new objects to available stack
    for (size_t i = 0; i < new_capacity; ++i) {
        available_objects_.push(&new_block[i]);
    }
    
    memory_blocks_.push_back(std::move(new_block));
    total_allocated_ += new_capacity;
    block_size_ = new_capacity;
}

template<typename T>
void MemoryPool<T>::reserve(size_t additional_capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t current_available = available_objects_.size();
    
    // Only expand if we don't have enough available objects
    if (current_available < additional_capacity) {
        size_t needed = additional_capacity - current_available;
        expand_pool(needed);
    }
}

template<typename T>
void MemoryPool<T>::shrink_to_fit(size_t target_objects) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t current_in_use = total_allocated_.load() - available_objects_.size();
    size_t safety_buffer = std::max(static_cast<size_t>(100), current_in_use / 10); // 10% buffer or min 100
    size_t minimum_objects = current_in_use + safety_buffer;
    
    // Safety check: don't shrink below minimum safe size
    size_t effective_target = std::max(target_objects, minimum_objects);
    size_t target_blocks = (effective_target + block_size_ - 1) / block_size_; // Ceiling division
    size_t current_blocks = memory_blocks_.size();
    
    // Log the shrinking decision
    std::cout << "[POOL SHRINK] Target: " << target_objects 
              << ", Effective: " << effective_target 
              << ", Current: " << total_allocated_.load()
              << ", In Use: " << current_in_use << std::endl;
    
    if (current_blocks <= target_blocks) {
        std::cout << "[POOL SHRINK] No shrinking needed. Current blocks: " 
                  << current_blocks << ", Target blocks: " << target_blocks << std::endl;
        return;
    }
    
    size_t blocks_to_remove = current_blocks - target_blocks;
    size_t blocks_removed = 0;
    
    for (auto it = memory_blocks_.begin(); it != memory_blocks_.end() && blocks_removed < blocks_to_remove; ) {
        T* block_start = it->get();
        T* block_end = block_start + block_size_;

        // Count how many objects from this block are in available_objects_
        std::stack<T*> temp_stack;
        size_t objects_from_block = 0;

        while (!available_objects_.empty()) {
            T* obj = available_objects_.top();
            available_objects_.pop();

            if (obj >= block_start && obj < block_end) {
                objects_from_block++;
            } else {
                temp_stack.push(obj);
            }
        }

        // Restore non-block objects
        while (!temp_stack.empty()) {
            available_objects_.push(temp_stack.top());
            temp_stack.pop();
        }

        // If all objects from this block were available, remove the block
        if (objects_from_block == block_size_) {
            std::cout << "[POOL SHRINK] Removing block " << (blocks_removed + 1) 
                      << "/" << blocks_to_remove << " (" << block_size_ 
                      << " objects)" << std::endl;
            
            it = memory_blocks_.erase(it);
            total_allocated_ -= block_size_;
            blocks_removed++;
        } else {
            // Put objects back if keeping the block
            for (size_t i = 0; i < objects_from_block; ++i) {
                available_objects_.push(block_start + i);
            }
            ++it;
        }
    }
    
    // Final logging
    std::cout << "[POOL SHRINK] Complete. Removed " << blocks_removed 
              << " blocks. New total: " << total_allocated_.load() 
              << " objects" << std::endl;
}

// Lock-free pool implementation
template<typename T>
LockFreeMemoryPool<T>::LockFreeMemoryPool(size_t pool_size)
    : capacity_(pool_size), next_available_(pool_size) {
    
    memory_block_ = std::make_unique<T[]>(capacity_);
    free_list_.reserve(capacity_);
    
    // Initialize free list with all objects
    for (size_t i = 0; i < capacity_; ++i) {
        free_list_.push_back(&memory_block_[i]);
    }
}

template<typename T>
LockFreeMemoryPool<T>::~LockFreeMemoryPool() {
    // Memory block automatically cleaned up
}

template<typename T>
T* LockFreeMemoryPool<T>::acquire() {
    if (next_available_ == 0) {
        return nullptr; // Pool exhausted
    }
    
    T* obj = free_list_[--next_available_];
    new(obj) T(); // Placement new to reset state
    return obj;
}

template<typename T>
void LockFreeMemoryPool<T>::release(T* obj) {
    if (!obj || next_available_ >= capacity_) {
        return; // Invalid object or pool full
    }
    
    obj->~T(); // Explicit destruction
    free_list_[next_available_++] = obj;
}

} // namespace hft
