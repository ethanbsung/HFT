#include "include/latency_tracker.hpp"
#include <iostream>

int main() {
    std::cout << "🚀 Testing Latency Tracker Implementation" << std::endl;
    
    // Test constructor
    hft::LatencyTracker tracker(500);  // Smaller window for testing
    std::cout << "✅ Constructor completed" << std::endl;
    
    // Test basic functionality (once you implement TODO 3-6)
    std::cout << "\n📊 Testing latency measurements..." << std::endl;
    tracker.add_market_data_latency(1500.0);  // Should trigger warning
    tracker.add_order_placement_latency(500.0);   // Normal latency
    tracker.add_tick_to_trade_latency(8000.0);    // Should trigger critical
    
    std::cout << "✅ Added test latencies" << std::endl;
    
    // Test statistics (once you implement TODO 7-15)
    std::cout << "\n📈 Testing statistics..." << std::endl;
    auto stats = tracker.get_statistics(hft::LatencyType::MARKET_DATA_PROCESSING);
    std::cout << "Market data measurements: " << stats.count << std::endl;
    
    // Test reporting (once you implement TODO 36-37)
    std::cout << "\n📋 Testing reports..." << std::endl;
    tracker.print_latency_report();
    
    std::cout << "\n🎉 All tests completed!" << std::endl;
    return 0;
}

/*
USAGE:
1. Implement some TODOs in latency_tracker.cpp
2. Compile: g++ -std=c++17 -Iinclude test_latency.cpp lib/libhft_core.a -o test_latency
3. Run: ./test_latency
4. See your progress!

This will help you test each phase as you implement it.
*/ 