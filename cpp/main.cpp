#include "memory_pool.hpp"
#include "latency_tracker.hpp"
#include <iostream>

int main() {
    // Test memory manager
    auto& manager = hft::MemoryManager::instance();
    manager.print_memory_report();
    
    // Test latency tracker
    hft::LatencyTracker tracker;
    tracker.add_market_data_latency(500.0);
    tracker.print_latency_report();
    
    std::cout << "✅ All tests completed successfully!" << std::endl;
    return 0;
}
