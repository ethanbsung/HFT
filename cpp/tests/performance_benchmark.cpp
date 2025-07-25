#include "../include/latency_tracker.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <thread>

using namespace hft;
using namespace std::chrono;

class PerformanceBenchmark {
private:
    std::mt19937 gen{42}; // Fixed seed for reproducible results
    std::uniform_real_distribution<double> latency_dist{100.0, 5000.0};

public:
    struct BenchmarkResults {
        double operations_per_second;
        double avg_latency_ns;
        double total_time_ms;
        size_t operations_count;
        double memory_mb;
    };

    // Benchmark hot path operations (add_latency_fast_path)
    BenchmarkResults benchmark_hot_path_additions(size_t num_operations) {
        LatencyTracker tracker(1024);
        
        auto start = high_resolution_clock::now();
        
        for (size_t i = 0; i < num_operations; ++i) {
            double latency = latency_dist(gen);
            tracker.add_latency_fast_path(LatencyType::ORDER_PLACEMENT, latency);
        }
        
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<nanoseconds>(end - start);
        
        BenchmarkResults results;
        results.operations_count = num_operations;
        results.total_time_ms = duration.count() / 1e6;
        results.avg_latency_ns = static_cast<double>(duration.count()) / num_operations;
        results.operations_per_second = num_operations / (duration.count() / 1e9);
        results.memory_mb = sizeof(LatencyTracker) / (1024.0 * 1024.0);
        
        return results;
    }

    // Benchmark traditional path operations (add_latency)
    BenchmarkResults benchmark_traditional_path_additions(size_t num_operations) {
        LatencyTracker tracker(1024);
        
        auto start = high_resolution_clock::now();
        
        for (size_t i = 0; i < num_operations; ++i) {
            double latency = latency_dist(gen);
            tracker.add_latency(LatencyType::ORDER_PLACEMENT, latency);
        }
        
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<nanoseconds>(end - start);
        
        BenchmarkResults results;
        results.operations_count = num_operations;
        results.total_time_ms = duration.count() / 1e6;
        results.avg_latency_ns = static_cast<double>(duration.count()) / num_operations;
        results.operations_per_second = num_operations / (duration.count() / 1e9);
        results.memory_mb = sizeof(LatencyTracker) / (1024.0 * 1024.0);
        
        return results;
    }

    // Benchmark statistics calculation (optimized vs fallback)
    BenchmarkResults benchmark_statistics_calculation(size_t num_measurements) {
        LatencyTracker tracker(2048);
        
        // Pre-populate with data
        for (size_t i = 0; i < num_measurements; ++i) {
            tracker.add_latency(LatencyType::MARKET_DATA_PROCESSING, latency_dist(gen));
        }
        
        const size_t num_stat_calls = 1000;
        auto start = high_resolution_clock::now();
        
        for (size_t i = 0; i < num_stat_calls; ++i) {
            auto stats = tracker.get_statistics(LatencyType::MARKET_DATA_PROCESSING);
            (void)stats; // Suppress unused variable warning
        }
        
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<nanoseconds>(end - start);
        
        BenchmarkResults results;
        results.operations_count = num_stat_calls;
        results.total_time_ms = duration.count() / 1e6;
        results.avg_latency_ns = static_cast<double>(duration.count()) / num_stat_calls;
        results.operations_per_second = num_stat_calls / (duration.count() / 1e9);
        results.memory_mb = sizeof(LatencyTracker) / (1024.0 * 1024.0);
        
        return results;
    }

    // Benchmark concurrent operations
    BenchmarkResults benchmark_concurrent_operations(size_t num_threads, size_t ops_per_thread) {
        LatencyTracker tracker(4096);
        std::vector<std::thread> threads;
        
        auto start = high_resolution_clock::now();
        
        for (size_t t = 0; t < num_threads; ++t) {
            threads.emplace_back([&tracker, ops_per_thread, this]() {
                std::hash<std::thread::id> hasher;
                std::mt19937 local_gen(42 + hasher(std::this_thread::get_id()));
                std::uniform_real_distribution<double> local_dist(100.0, 5000.0);
                
                for (size_t i = 0; i < ops_per_thread; ++i) {
                    double latency = local_dist(local_gen);
                    tracker.add_latency_fast_path(LatencyType::ORDER_PLACEMENT, latency);
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<nanoseconds>(end - start);
        
        size_t total_ops = num_threads * ops_per_thread;
        BenchmarkResults results;
        results.operations_count = total_ops;
        results.total_time_ms = duration.count() / 1e6;
        results.avg_latency_ns = static_cast<double>(duration.count()) / total_ops;
        results.operations_per_second = total_ops / (duration.count() / 1e9);
        results.memory_mb = sizeof(LatencyTracker) / (1024.0 * 1024.0);
        
        return results;
    }

    // Benchmark memory efficiency
    void benchmark_memory_usage() {
        std::cout << "\n📊 === MEMORY USAGE ANALYSIS === 📊" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        
        std::cout << "LatencyTracker object size: " << sizeof(LatencyTracker) << " bytes" << std::endl;
        std::cout << "LockFreeCircularBuffer<1024> size: " << sizeof(LockFreeCircularBuffer<1024>) << " bytes" << std::endl;
        std::cout << "ApproximatePercentile size: " << sizeof(ApproximatePercentile) << " bytes" << std::endl;
        
        // Memory per latency type
        size_t per_type_memory = sizeof(LockFreeCircularBuffer<1024>) + 
                                sizeof(ApproximatePercentile) * 2 + // P95 + P99
                                sizeof(std::deque<double>); // Legacy deque
        
        std::cout << "Memory per latency type: " << per_type_memory << " bytes" << std::endl;
        std::cout << "Total memory for " << static_cast<int>(LatencyType::COUNT) << " types: " 
                  << per_type_memory * static_cast<int>(LatencyType::COUNT) << " bytes" << std::endl;
    }

    void print_results(const std::string& test_name, const BenchmarkResults& results) {
        std::cout << "\n🔥 " << test_name << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Operations: " << results.operations_count << std::endl;
        std::cout << "  Total time: " << results.total_time_ms << " ms" << std::endl;
        std::cout << "  Avg latency: " << results.avg_latency_ns << " ns/op" << std::endl;
        std::cout << "  Throughput: " << std::setprecision(0) << results.operations_per_second << " ops/sec" << std::endl;
        std::cout << "  Memory: " << std::setprecision(3) << results.memory_mb << " MB" << std::endl;
    }

    void print_comparison(const std::string& test_name, 
                         const BenchmarkResults& optimized, 
                         const BenchmarkResults& traditional) {
        std::cout << "\n⚡ " << test_name << " COMPARISON" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        
        double latency_improvement = ((traditional.avg_latency_ns - optimized.avg_latency_ns) / traditional.avg_latency_ns) * 100.0;
        double throughput_improvement = ((optimized.operations_per_second - traditional.operations_per_second) / traditional.operations_per_second) * 100.0;
        double time_improvement = ((traditional.total_time_ms - optimized.total_time_ms) / traditional.total_time_ms) * 100.0;
        
        std::cout << "  Optimized latency: " << optimized.avg_latency_ns << " ns/op" << std::endl;
        std::cout << "  Traditional latency: " << traditional.avg_latency_ns << " ns/op" << std::endl;
        std::cout << "  🚀 Latency improvement: " << latency_improvement << "%" << std::endl;
        
        std::cout << "  Optimized throughput: " << std::setprecision(0) << optimized.operations_per_second << " ops/sec" << std::endl;
        std::cout << "  Traditional throughput: " << traditional.operations_per_second << " ops/sec" << std::endl;
        std::cout << "  🚀 Throughput improvement: " << std::setprecision(2) << throughput_improvement << "%" << std::endl;
        
        std::cout << "  🚀 Total time improvement: " << time_improvement << "%" << std::endl;
    }
};

int main() {
    std::cout << "🚀 === HFT LATENCY TRACKER PERFORMANCE BENCHMARK === 🚀" << std::endl;
    std::cout << "Running comprehensive performance analysis..." << std::endl;
    
    PerformanceBenchmark benchmark;
    
    // Memory usage analysis
    benchmark.benchmark_memory_usage();
    
    // Test 1: Hot path vs Traditional path (100K operations)
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "TEST 1: Hot Path vs Traditional Path (100K operations)" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    auto hot_path_100k = benchmark.benchmark_hot_path_additions(100000);
    auto traditional_100k = benchmark.benchmark_traditional_path_additions(100000);
    
    benchmark.print_results("HOT PATH (add_latency_fast_path)", hot_path_100k);
    benchmark.print_results("TRADITIONAL PATH (add_latency)", traditional_100k);
    benchmark.print_comparison("HOT PATH vs TRADITIONAL", hot_path_100k, traditional_100k);
    
    // Test 2: High-frequency test (1M operations)
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "TEST 2: High-Frequency Load Test (1M operations)" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    auto hot_path_1m = benchmark.benchmark_hot_path_additions(1000000);
    auto traditional_1m = benchmark.benchmark_traditional_path_additions(1000000);
    
    benchmark.print_results("HOT PATH (1M ops)", hot_path_1m);
    benchmark.print_results("TRADITIONAL PATH (1M ops)", traditional_1m);
    benchmark.print_comparison("1M OPERATIONS", hot_path_1m, traditional_1m);
    
    // Test 3: Statistics calculation performance
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "TEST 3: Statistics Calculation (1000 calls with 10K measurements)" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    auto stats_perf = benchmark.benchmark_statistics_calculation(10000);
    benchmark.print_results("STATISTICS CALCULATION", stats_perf);
    
    // Test 4: Concurrent operations
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "TEST 4: Concurrent Operations (4 threads, 50K ops each)" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    auto concurrent_perf = benchmark.benchmark_concurrent_operations(4, 50000);
    benchmark.print_results("CONCURRENT OPERATIONS", concurrent_perf);
    
    // HFT Performance Assessment
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "🎯 HFT PERFORMANCE ASSESSMENT" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    std::cout << "\n📊 HOT PATH PERFORMANCE:" << std::endl;
    std::cout << "  • Per-operation latency: " << hot_path_100k.avg_latency_ns << " ns" << std::endl;
    std::cout << "  • Throughput: " << std::setprecision(0) << hot_path_100k.operations_per_second << " ops/sec" << std::endl;
    
    if (hot_path_100k.avg_latency_ns < 100) {
        std::cout << "  🟢 EXCELLENT: < 100ns per operation" << std::endl;
    } else if (hot_path_100k.avg_latency_ns < 500) {
        std::cout << "  🟡 GOOD: < 500ns per operation" << std::endl;
    } else if (hot_path_100k.avg_latency_ns < 1000) {
        std::cout << "  🟠 ACCEPTABLE: < 1μs per operation" << std::endl;
    } else {
        std::cout << "  🔴 NEEDS OPTIMIZATION: > 1μs per operation" << std::endl;
    }
    
    std::cout << "\n📈 IMPROVEMENT SUMMARY:" << std::endl;
    double latency_improvement = ((traditional_100k.avg_latency_ns - hot_path_100k.avg_latency_ns) / traditional_100k.avg_latency_ns) * 100.0;
    double throughput_improvement = ((hot_path_100k.operations_per_second - traditional_100k.operations_per_second) / traditional_100k.operations_per_second) * 100.0;
    
    std::cout << "  🚀 Latency reduced by: " << std::setprecision(1) << latency_improvement << "%" << std::endl;
    std::cout << "  🚀 Throughput increased by: " << throughput_improvement << "%" << std::endl;
    std::cout << "  🚀 Memory efficient: " << std::setprecision(2) << hot_path_100k.memory_mb << " MB total" << std::endl;
    
    std::cout << "\n✅ CONCLUSION: ";
    if (latency_improvement > 30 && hot_path_100k.avg_latency_ns < 500) {
        std::cout << "PRODUCTION READY FOR HFT!" << std::endl;
    } else if (latency_improvement > 15 && hot_path_100k.avg_latency_ns < 1000) {
        std::cout << "SUITABLE FOR HIGH-FREQUENCY TRADING" << std::endl;
    } else {
        std::cout << "FURTHER OPTIMIZATION RECOMMENDED" << std::endl;
    }
    
    return 0;
} 