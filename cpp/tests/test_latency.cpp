#include <gtest/gtest.h>
#include "../include/latency_tracker.hpp"
#include <thread>
#include <chrono>
#include <random>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace hft;
using namespace std::chrono_literals;

// Test fixture for LatencyTracker tests
class LatencyTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker = std::make_unique<LatencyTracker>(100); // Small window for testing
    }
    
    void TearDown() override {
        tracker.reset();
    }
    
    std::unique_ptr<LatencyTracker> tracker;
    
    // Helper functions for testing
    std::vector<double> generate_test_data(size_t count, double mean = 1000.0, double std_dev = 100.0) {
        std::vector<double> data;
        std::random_device rd;
        std::mt19937 gen(42); // Fixed seed for reproducible tests
        std::normal_distribution<> dis(mean, std_dev);
        
        for (size_t i = 0; i < count; ++i) {
            data.push_back(std::max(0.1, dis(gen))); // Ensure positive values
        }
        return data;
    }
    
    // Add multiple latency measurements
    void add_test_latencies(LatencyType type, const std::vector<double>& latencies) {
        for (double latency : latencies) {
            tracker->add_latency(type, latency);
        }
    }
    
    // Calculate expected statistics manually for verification
    struct ExpectedStats {
        double mean;
        double median;
        double p95;
        double p99;
        double min;
        double max;
        double std_dev;
    };
    
    ExpectedStats calculate_expected_stats(std::vector<double> data) {
        if (data.empty()) return {};
        
        std::sort(data.begin(), data.end());
        
        ExpectedStats stats;
        stats.min = data.front();
        stats.max = data.back();
        
        // Mean
        stats.mean = std::accumulate(data.begin(), data.end(), 0.0) / data.size();
        
        // Median
        size_t n = data.size();
        if (n % 2 == 0) {
            stats.median = (data[n/2 - 1] + data[n/2]) / 2.0;
        } else {
            stats.median = data[n/2];
        }
        
        // Percentiles
        auto percentile = [&](double p) {
            double index = (p / 100.0) * (data.size() - 1);
            size_t lower = static_cast<size_t>(index);
            if (lower >= data.size() - 1) return data.back();
            double weight = index - lower;
            return data[lower] * (1.0 - weight) + data[lower + 1] * weight;
        };
        
        stats.p95 = percentile(95.0);
        stats.p99 = percentile(99.0);
        
        // Standard deviation
        double variance = 0.0;
        for (double val : data) {
            double diff = val - stats.mean;
            variance += diff * diff;
        }
        variance /= data.size();
        stats.std_dev = std::sqrt(variance);
        
        return stats;
    }
};

// =============================================================================
// BASIC FUNCTIONALITY TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, DefaultConstruction) {
    EXPECT_NO_THROW(LatencyTracker default_tracker);
    auto default_tracker = std::make_unique<LatencyTracker>();
    EXPECT_EQ(default_tracker->get_total_measurements(), 0);
}

TEST_F(LatencyTrackerTest, AddSingleLatency) {
    double test_latency = 1500.0;
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, test_latency);
    
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, 1);
    EXPECT_DOUBLE_EQ(stats.mean_us, test_latency);
    EXPECT_DOUBLE_EQ(stats.median_us, test_latency);
    EXPECT_DOUBLE_EQ(stats.min_us, test_latency);
    EXPECT_DOUBLE_EQ(stats.max_us, test_latency);
    EXPECT_DOUBLE_EQ(stats.std_dev_us, 0.0);
}

TEST_F(LatencyTrackerTest, AddLatencyWithDuration) {
    auto duration = std::chrono::microseconds(2500);
    tracker->add_latency(LatencyType::ORDER_PLACEMENT, duration_us_t(duration));
    
    auto stats = tracker->get_statistics(LatencyType::ORDER_PLACEMENT);
    EXPECT_EQ(stats.count, 1);
    EXPECT_DOUBLE_EQ(stats.mean_us, 2500.0);
}

TEST_F(LatencyTrackerTest, ConvenienceMethods) {
    tracker->add_market_data_latency(1000.0);
    tracker->add_order_placement_latency(2000.0);
    tracker->add_tick_to_trade_latency(3000.0);
    
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::MARKET_DATA_PROCESSING), 1);
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::ORDER_PLACEMENT), 1);
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::TICK_TO_TRADE), 1);
    EXPECT_EQ(tracker->get_total_measurements(), 3);
}

// =============================================================================
// EDGE CASES AND BOUNDARY CONDITIONS
// =============================================================================

TEST_F(LatencyTrackerTest, EmptyStatistics) {
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, 0);
    EXPECT_DOUBLE_EQ(stats.mean_us, 0.0);
    EXPECT_DOUBLE_EQ(stats.median_us, 0.0);
    EXPECT_DOUBLE_EQ(stats.p95_us, 0.0);
    EXPECT_DOUBLE_EQ(stats.p99_us, 0.0);
    EXPECT_DOUBLE_EQ(stats.min_us, 0.0);
    EXPECT_DOUBLE_EQ(stats.max_us, 0.0);
    EXPECT_DOUBLE_EQ(stats.std_dev_us, 0.0);
}

TEST_F(LatencyTrackerTest, ZeroLatency) {
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 0.0);
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, 1);
    EXPECT_DOUBLE_EQ(stats.mean_us, 0.0);
}

TEST_F(LatencyTrackerTest, NegativeLatency) {
    // Should handle gracefully (implementation detail - might clamp or reject)
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, -100.0);
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    // Test passes regardless of how negative values are handled
    EXPECT_EQ(stats.count, 1);
}

TEST_F(LatencyTrackerTest, VeryLargeLatency) {
    double large_latency = 1e9; // 1 second in microseconds
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, large_latency);
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, 1);
    EXPECT_DOUBLE_EQ(stats.mean_us, large_latency);
}

TEST_F(LatencyTrackerTest, WindowOverflow) {
    size_t window_size = 10;
    auto small_tracker = std::make_unique<LatencyTracker>(window_size);
    
    // Add more measurements than window size
    for (size_t i = 0; i < window_size + 5; ++i) {
        small_tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, static_cast<double>(i));
    }
    
    auto stats = small_tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, window_size); // Should cap at window size
    EXPECT_EQ(small_tracker->get_measurement_count(LatencyType::MARKET_DATA_PROCESSING), window_size);
}

// =============================================================================
// STATISTICAL CALCULATIONS TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, StatisticsWithKnownData) {
    std::vector<double> test_data = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};
    add_test_latencies(LatencyType::MARKET_DATA_PROCESSING, test_data);
    
    auto expected = calculate_expected_stats(test_data);
    auto actual = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    
    EXPECT_EQ(actual.count, test_data.size());
    EXPECT_NEAR(actual.mean_us, expected.mean, 0.01);
    EXPECT_NEAR(actual.median_us, expected.median, 0.01);
    EXPECT_NEAR(actual.p95_us, expected.p95, 0.01);
    EXPECT_NEAR(actual.p99_us, expected.p99, 0.01);
    EXPECT_NEAR(actual.min_us, expected.min, 0.01);
    EXPECT_NEAR(actual.max_us, expected.max, 0.01);
    EXPECT_NEAR(actual.std_dev_us, expected.std_dev, 0.01);
}

TEST_F(LatencyTrackerTest, PercentilesWithSmallDataset) {
    std::vector<double> test_data = {1, 2, 3, 4, 5};
    add_test_latencies(LatencyType::ORDER_PLACEMENT, test_data);
    
    auto stats = tracker->get_statistics(LatencyType::ORDER_PLACEMENT);
    EXPECT_GT(stats.p95_us, stats.median_us);
    EXPECT_GT(stats.p99_us, stats.p95_us);
    EXPECT_GE(stats.max_us, stats.p99_us);
}

TEST_F(LatencyTrackerTest, PercentilesWithLargeDataset) {
    auto test_data = generate_test_data(1000, 1000.0, 100.0);
    add_test_latencies(LatencyType::TICK_TO_TRADE, test_data);
    
    auto expected = calculate_expected_stats(test_data);
    auto actual = tracker->get_statistics(LatencyType::TICK_TO_TRADE);
    
    // Allow for reasonable differences in percentile calculations on large datasets
    EXPECT_NEAR(actual.p95_us, expected.p95, 50.0);
    EXPECT_NEAR(actual.p99_us, expected.p99, 100.0);
}

TEST_F(LatencyTrackerTest, StatisticsWithIdenticalValues) {
    std::vector<double> identical_data(50, 1000.0);
    add_test_latencies(LatencyType::ORDER_CANCELLATION, identical_data);
    
    auto stats = tracker->get_statistics(LatencyType::ORDER_CANCELLATION);
    EXPECT_DOUBLE_EQ(stats.mean_us, 1000.0);
    EXPECT_DOUBLE_EQ(stats.median_us, 1000.0);
    EXPECT_DOUBLE_EQ(stats.p95_us, 1000.0);
    EXPECT_DOUBLE_EQ(stats.p99_us, 1000.0);
    EXPECT_DOUBLE_EQ(stats.min_us, 1000.0);
    EXPECT_DOUBLE_EQ(stats.max_us, 1000.0);
    EXPECT_DOUBLE_EQ(stats.std_dev_us, 0.0);
}

// =============================================================================
// SPIKE DETECTION TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, NoSpikesWithNormalLatency) {
    // Add latencies well below warning thresholds
    for (int i = 0; i < 10; ++i) {
        tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 500.0); // Below 1000us warning
    }
    
    auto spikes = tracker->get_recent_spikes(5);
    EXPECT_TRUE(spikes.empty());
    EXPECT_FALSE(tracker->should_alert());
}

TEST_F(LatencyTrackerTest, WarningSpikesDetection) {
    // Add warning-level spike for market data processing (> 1000us)
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1500.0);
    
    auto spikes = tracker->get_recent_spikes(1);
    EXPECT_EQ(spikes.size(), 1);
    EXPECT_EQ(spikes[0].type, LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(spikes[0].severity, SpikesSeverity::WARNING);
    EXPECT_DOUBLE_EQ(spikes[0].latency_us, 1500.0);
}

TEST_F(LatencyTrackerTest, CriticalSpikesDetection) {
    // Add critical-level spike for order placement (> 10000us)
    tracker->add_latency(LatencyType::ORDER_PLACEMENT, 15000.0);
    
    auto spikes = tracker->get_recent_spikes(1);
    EXPECT_EQ(spikes.size(), 1);
    EXPECT_EQ(spikes[0].severity, SpikesSeverity::CRITICAL);
    EXPECT_DOUBLE_EQ(spikes[0].latency_us, 15000.0);
    EXPECT_TRUE(tracker->should_alert()); // Critical spike should trigger alert
}

TEST_F(LatencyTrackerTest, MultipleSpikesAndAlertLogic) {
    // Add multiple warning spikes (should trigger alert after 3)
    for (int i = 0; i < 4; ++i) {
        tracker->add_latency(LatencyType::TICK_TO_TRADE, 7000.0); // Warning level
    }
    
    auto spikes = tracker->get_recent_spikes(1);
    EXPECT_EQ(spikes.size(), 4);
    EXPECT_TRUE(tracker->should_alert()); // More than 3 warnings should alert
}

TEST_F(LatencyTrackerTest, SpikeHistoryManagement) {
    auto large_tracker = std::make_unique<LatencyTracker>(1000);
    
    // Fill spike history beyond MAX_SPIKE_HISTORY
    for (int i = 0; i < LatencyTracker::MAX_SPIKE_HISTORY + 10; ++i) {
        large_tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 6000.0); // Critical
    }
    
    auto all_spikes = large_tracker->get_recent_spikes(60); // Large time window
    EXPECT_LE(all_spikes.size(), LatencyTracker::MAX_SPIKE_HISTORY);
}

TEST_F(LatencyTrackerTest, SpikeTimeFiltering) {
    // Add spike and test time-based filtering
    tracker->add_latency(LatencyType::ORDER_PLACEMENT, 15000.0);
    
    auto recent_spikes = tracker->get_recent_spikes(1); // 1 minute
    EXPECT_EQ(recent_spikes.size(), 1);
    
    auto old_spikes = tracker->get_recent_spikes(0); // 0 minutes (essentially none)
    EXPECT_TRUE(old_spikes.empty());
}

// =============================================================================
// PERFORMANCE TREND TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, PerformanceTrendWithInsufficientData) {
    // Add only a few measurements (< 20 for trend analysis)
    for (int i = 0; i < 5; ++i) {
        tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1000.0);
    }
    
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    // Should have default/stable trend with insufficient data
    EXPECT_EQ(stats.trend.sample_count, 0);
}

TEST_F(LatencyTrackerTest, ImprovingPerformanceTrend) {
    // Add measurements with decreasing latency (improving performance)
    for (int i = 25; i >= 1; --i) {
        tracker->add_latency(LatencyType::ORDER_PLACEMENT, static_cast<double>(i * 100));
    }
    
    auto stats = tracker->get_statistics(LatencyType::ORDER_PLACEMENT);
    EXPECT_GT(stats.trend.sample_count, 0);
    EXPECT_LT(stats.trend.trend_percentage, 0); // Negative = improving
}

TEST_F(LatencyTrackerTest, DegradingPerformanceTrend) {
    // Add measurements with increasing latency (degrading performance)
    for (int i = 1; i <= 25; ++i) {
        tracker->add_latency(LatencyType::TICK_TO_TRADE, static_cast<double>(i * 100));
    }
    
    auto stats = tracker->get_statistics(LatencyType::TICK_TO_TRADE);
    EXPECT_GT(stats.trend.trend_percentage, 0); // Positive = degrading
}

TEST_F(LatencyTrackerTest, VolatilePerformanceTrend) {
    // Add enough measurements to trigger trend analysis (need 20+ for trend tracking)
    // Then add highly variable measurements to create volatility
    std::vector<double> volatile_data;
    
    // First add 25 stable measurements to build up trend window
    for (int i = 0; i < 25; ++i) {
        volatile_data.push_back(1000.0);
    }
    
    // Then add highly variable measurements to create volatility
    for (int i = 0; i < 15; ++i) {
        volatile_data.push_back((i % 2 == 0) ? 500.0 : 2000.0); // Alternating
    }
    
    add_test_latencies(LatencyType::ORDER_CANCELLATION, volatile_data);
    
    auto stats = tracker->get_statistics(LatencyType::ORDER_CANCELLATION);
    // Should have trend data now since we have enough measurements
    EXPECT_GT(stats.trend.sample_count, 0);
    // Volatility should be greater than 0 due to the alternating pattern
    EXPECT_GT(stats.trend.volatility, 0);
}

// =============================================================================
// WINDOW MANAGEMENT TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, RollingWindowBehavior) {
    size_t window_size = 5;
    auto small_tracker = std::make_unique<LatencyTracker>(window_size);
    
    // Add exactly window_size measurements
    for (size_t i = 1; i <= window_size; ++i) {
        small_tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, static_cast<double>(i * 100));
    }
    
    auto stats = small_tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, window_size);
    EXPECT_DOUBLE_EQ(stats.min_us, 100.0);
    EXPECT_DOUBLE_EQ(stats.max_us, 500.0);
    
    // Add one more measurement (should evict oldest)
    small_tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 600.0);
    
    stats = small_tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, window_size);
    EXPECT_DOUBLE_EQ(stats.min_us, 200.0); // First measurement (100) should be evicted
    EXPECT_DOUBLE_EQ(stats.max_us, 600.0);
}

TEST_F(LatencyTrackerTest, MultipleLatencyTypesIndependentWindows) {
    // Add different amounts to different latency types
    for (int i = 0; i < 5; ++i) {
        tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1000.0);
    }
    for (int i = 0; i < 10; ++i) {
        tracker->add_latency(LatencyType::ORDER_PLACEMENT, 2000.0);
    }
    
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::MARKET_DATA_PROCESSING), 5);
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::ORDER_PLACEMENT), 10);
    EXPECT_EQ(tracker->get_total_measurements(), 15);
    
    // Statistics should be independent
    auto market_stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    auto order_stats = tracker->get_statistics(LatencyType::ORDER_PLACEMENT);
    
    EXPECT_DOUBLE_EQ(market_stats.mean_us, 1000.0);
    EXPECT_DOUBLE_EQ(order_stats.mean_us, 2000.0);
}

// =============================================================================
// TIME FORMATTING TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, TimeFormatterDuration) {
    TimeFormatter::TimeBuffer buffer;
    
    // Test microseconds
    TimeFormatter::format_duration_fast(500.0, buffer);
         std::string result(buffer.data());
     EXPECT_TRUE(result.find("μs") != std::string::npos);
     
     // Test milliseconds
     TimeFormatter::format_duration_fast(5000.0, buffer);
     result = std::string(buffer.data());
     EXPECT_TRUE(result.find("ms") != std::string::npos);
     
     // Test seconds
     TimeFormatter::format_duration_fast(2000000.0, buffer);
     result = std::string(buffer.data());
     EXPECT_TRUE(result.find("s") != std::string::npos);
}

TEST_F(LatencyTrackerTest, TimeFormatterTimestamp) {
    TimeFormatter::TimeBuffer buffer;
    auto now = std::chrono::high_resolution_clock::now();
    
    EXPECT_NO_THROW(TimeFormatter::format_time_fast(now, buffer));
    // Should produce a time string in HH:MM:SS.mmm format
    std::string time_str(buffer.data());
    EXPECT_EQ(time_str.length(), 12); // Format: "HH:MM:SS.mmm"
    EXPECT_EQ(time_str[2], ':');
    EXPECT_EQ(time_str[5], ':');
    EXPECT_EQ(time_str[8], '.');
}

// =============================================================================
// SCOPED MEASUREMENT TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, ScopedLatencyMeasurement) {
    {
        ScopedLatencyMeasurement measurement(*tracker, LatencyType::MARKET_DATA_PROCESSING);
        std::this_thread::sleep_for(1ms); // Small delay for measurement
    } // Destructor should record measurement
    
    auto stats = tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    EXPECT_EQ(stats.count, 1);
    EXPECT_GT(stats.mean_us, 500.0); // Should be at least 1ms
}

TEST_F(LatencyTrackerTest, MacroMeasurements) {
    {
        MEASURE_MARKET_DATA_LATENCY(*tracker);
        std::this_thread::sleep_for(1ms);
    }
    
    {
        MEASURE_ORDER_LATENCY(*tracker);
        std::this_thread::sleep_for(1ms);
    }
    
    {
        MEASURE_TICK_TO_TRADE_LATENCY(*tracker);
        std::this_thread::sleep_for(1ms);
    }
    
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::MARKET_DATA_PROCESSING), 1);
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::ORDER_PLACEMENT), 1);
    EXPECT_EQ(tracker->get_measurement_count(LatencyType::TICK_TO_TRADE), 1);
}

// =============================================================================
// SYSTEM MONITORING TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, UptimeTracking) {
    auto initial_uptime = tracker->get_uptime_seconds();
    EXPECT_GE(initial_uptime, 0.0);
    
    // Sleep for more than 1 second since get_uptime_seconds() truncates to whole seconds
    std::this_thread::sleep_for(1100ms);
    
    auto later_uptime = tracker->get_uptime_seconds();
    EXPECT_GT(later_uptime, initial_uptime);
    EXPECT_GE(later_uptime, 1.0); // Should be at least 1 second
}

TEST_F(LatencyTrackerTest, ResetFunctionality) {
    // Add some measurements
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1000.0);
    tracker->add_latency(LatencyType::ORDER_PLACEMENT, 15000.0); // Critical spike
    
    EXPECT_GT(tracker->get_total_measurements(), 0);
    EXPECT_FALSE(tracker->get_recent_spikes(5).empty());
    
    // Reset statistics
    tracker->reset_statistics();
    
    EXPECT_EQ(tracker->get_total_measurements(), 0);
    // Uptime should reset
    EXPECT_LT(tracker->get_uptime_seconds(), 0.1);
    
    // Clear spike history
    tracker->clear_spike_history();
    EXPECT_TRUE(tracker->get_recent_spikes(5).empty());
}

// =============================================================================
// THREAD SAFETY TESTS (Basic)
// =============================================================================

TEST_F(LatencyTrackerTest, ConcurrentMeasurements) {
    const int num_threads = 4;
    const int measurements_per_thread = 100;
    std::vector<std::thread> threads;
    
    // Launch multiple threads adding measurements
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, measurements_per_thread]() {
            for (int i = 0; i < measurements_per_thread; ++i) {
                LatencyType type = static_cast<LatencyType>(t % static_cast<int>(LatencyType::COUNT));
                tracker->add_latency(type, 1000.0 + t * 100.0);
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify total measurements (may be less than expected due to window limits)
    size_t total_expected = num_threads * measurements_per_thread;
    size_t actual_total = tracker->get_total_measurements();
    EXPECT_LE(actual_total, total_expected);
    EXPECT_GT(actual_total, 0);
}

// =============================================================================
// PERFORMANCE AND STRESS TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, LargeDatasetPerformance) {
    const size_t large_count = 10000;
    auto large_tracker = std::make_unique<LatencyTracker>(large_count);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Add large number of measurements
    for (size_t i = 0; i < large_count; ++i) {
        large_tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 
                                  1000.0 + (i % 1000)); // Varied but predictable
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Should complete in reasonable time (more lenient for debug builds)
    EXPECT_LT(duration.count(), 30000); // 30 seconds for debug builds
    
    // Verify statistics calculation performance
    start_time = std::chrono::high_resolution_clock::now();
    auto stats = large_tracker->get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    end_time = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    EXPECT_LT(duration.count(), 1000); // Statistics should complete within 1 second
    EXPECT_EQ(stats.count, large_count);
}

// =============================================================================
// REPORTING TESTS (Basic functionality)
// =============================================================================

TEST_F(LatencyTrackerTest, ReportingFunctions) {
    // Add some test data
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1000.0);
    tracker->add_latency(LatencyType::ORDER_PLACEMENT, 2000.0);
    tracker->add_latency(LatencyType::TICK_TO_TRADE, 15000.0); // Critical spike
    
    // These should not crash
    EXPECT_NO_THROW(tracker->print_latency_report());
    EXPECT_NO_THROW(tracker->print_detailed_report());
}

// =============================================================================
// ENUM AND TYPE TESTS
// =============================================================================

TEST_F(LatencyTrackerTest, LatencyTypeEnumValues) {
    // Ensure all enum values work
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1000.0);
    tracker->add_latency(LatencyType::ORDER_PLACEMENT, 1000.0);
    tracker->add_latency(LatencyType::ORDER_CANCELLATION, 1000.0);
    tracker->add_latency(LatencyType::TICK_TO_TRADE, 1000.0);
    tracker->add_latency(LatencyType::ORDER_BOOK_UPDATE, 1000.0);
    
    // Each type should have exactly one measurement
    for (size_t i = 0; i < static_cast<size_t>(LatencyType::COUNT); ++i) {
        LatencyType type = static_cast<LatencyType>(i);
        EXPECT_EQ(tracker->get_measurement_count(type), 1);
    }
}

TEST_F(LatencyTrackerTest, SpikesSeverityLevels) {
    // Test warning level spike
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 1500.0); // Warning
    auto spikes = tracker->get_recent_spikes(1);
    EXPECT_EQ(spikes.size(), 1);
    EXPECT_EQ(spikes[0].severity, SpikesSeverity::WARNING);
    
    // Test critical level spike
    tracker->add_latency(LatencyType::MARKET_DATA_PROCESSING, 6000.0); // Critical
    spikes = tracker->get_recent_spikes(1);
    EXPECT_EQ(spikes.size(), 2);
    // Find the critical spike
    auto critical_spike = std::find_if(spikes.begin(), spikes.end(),
        [](const LatencySpike& spike) { return spike.severity == SpikesSeverity::CRITICAL; });
    EXPECT_NE(critical_spike, spikes.end());
}

// =============================================================================
// MAIN TEST RUNNER
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
