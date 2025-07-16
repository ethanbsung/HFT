#pragma once

#include "types.hpp"  // Our foundation types
#include <deque>
#include <vector>
#include <array>
#include <algorithm>  // LEARNING: STL algorithms (sort, nth_element)
#include <numeric>    // LEARNING: Mathematical operations (accumulate)
#include <string>
#include <iostream>   // LEARNING: Input/output operations
#include <iomanip>    // LEARNING: Output formatting
#include <chrono>     // For time formatting
#include <cstring>    // For fast string operations

namespace hft {

/**
 * LEARNING GOAL: This enum teaches you about strongly-typed categories
 * Instead of using strings (slow), we use enum for fast comparisons
 */
enum class LatencyType : uint8_t {
    MARKET_DATA_PROCESSING = 0,  // Time to process incoming market data
    ORDER_PLACEMENT = 1,         // Time to place order on exchange
    ORDER_CANCELLATION = 2,      // Time to cancel existing order
    TICK_TO_TRADE = 3,          // Time from price change to trade decision
    COUNT = 4                    // Total number of types (useful for arrays)
};

/**
 * LEARNING GOAL: This enum teaches severity levels and bit manipulation
 */
enum class SpikesSeverity : uint8_t {
    WARNING = 1,   // Latency above warning threshold
    CRITICAL = 2   // Latency above critical threshold
};

/**
 * Performance trend analysis for optimization
 */
enum class PerformanceTrend : uint8_t {
    IMPROVING = 0,     // Performance getting better
    STABLE = 1,        // Performance consistent
    DEGRADING = 2,     // Performance getting worse
    VOLATILE = 3       // Performance inconsistent
};

/**
 * LEARNING GOAL: This struct teaches you about POD (Plain Old Data) types
 * POD types are faster and more cache-friendly than complex classes
 */
struct LatencySpike {
    timestamp_t timestamp;        // When the spike occurred
    LatencyType type;            // What type of operation was slow
    double latency_us;           // How long it took (microseconds)
    SpikesSeverity severity;     // How bad was it
    
    // LEARNING: Default constructor for clean initialization
    LatencySpike() = default;
    
    // LEARNING: Parameterized constructor for easy creation
    LatencySpike(timestamp_t ts, LatencyType t, double lat, SpikesSeverity sev)
        : timestamp(ts), type(t), latency_us(lat), severity(sev) {}
};

/**
 * Performance trend data for analysis
 */
struct PerformanceTrendData {
    PerformanceTrend trend;
    double trend_percentage;     // Positive = improving, negative = degrading
    double volatility;           // Standard deviation of recent changes
    uint32_t sample_count;       // Number of samples in trend calculation
    
    PerformanceTrendData() : trend(PerformanceTrend::STABLE), trend_percentage(0.0),
                            volatility(0.0), sample_count(0) {}
};

/**
 * LEARNING GOAL: This struct teaches aggregation and statistics
 * Shows how to organize related data together
 */
struct LatencyStatistics {
    uint64_t count;      // How many measurements
    double mean_us;      // Average latency
    double median_us;    // Middle value (50th percentile)
    double p95_us;       // 95th percentile (performance target)
    double p99_us;       // 99th percentile (worst-case analysis)
    double min_us;       // Best case
    double max_us;       // Worst case
    double std_dev_us;   // Standard deviation (volatility measure)
    PerformanceTrendData trend; // Performance trend analysis
    
    // LEARNING: Initialize all members to safe defaults
    LatencyStatistics() : count(0), mean_us(0.0), median_us(0.0), 
                         p95_us(0.0), p99_us(0.0), min_us(0.0), 
                         max_us(0.0), std_dev_us(0.0) {}
};

/**
 * High-performance time formatting utilities
 * Optimized for minimal overhead in HFT environments
 */
class TimeFormatter {
public:
    // Pre-allocated buffer for performance (no dynamic allocation)
    static constexpr size_t BUFFER_SIZE = 32;
    using TimeBuffer = std::array<char, BUFFER_SIZE>;
    
    // Format timestamp to readable string (HH:MM:SS.mmm format)
    static inline void format_time_fast(timestamp_t timestamp, TimeBuffer& buffer) noexcept {
        auto time_c = std::chrono::system_clock::to_time_t(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(timestamp));
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()) % 1000;
        
        // Fast formatting using pre-calculated lookup tables
        auto tm_info = std::localtime(&time_c);
        std::snprintf(buffer.data(), BUFFER_SIZE, "%02d:%02d:%02d.%03d",
                     tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, 
                     static_cast<int>(ms.count()));
    }
    
    // Format duration in microseconds to readable string
    static inline void format_duration_fast(double duration_us, TimeBuffer& buffer) noexcept {
        if (duration_us < 1000.0) {
            std::snprintf(buffer.data(), BUFFER_SIZE, "%.1fμs", duration_us);
        } else if (duration_us < 1000000.0) {
            std::snprintf(buffer.data(), BUFFER_SIZE, "%.2fms", duration_us / 1000.0);
        } else {
            std::snprintf(buffer.data(), BUFFER_SIZE, "%.3fs", duration_us / 1000000.0);
        }
    }
};

/**
 * High-performance latency tracking for HFT systems
 * 
 * LEARNING GOALS:
 * - Understand rolling window data structures
 * - Learn statistical calculations in C++
 * - Practice with STL containers and algorithms
 * - Implement real-time performance monitoring
 */
class LatencyTracker {
public:
    // LEARNING: constexpr for compile-time constants
    static constexpr size_t DEFAULT_WINDOW_SIZE = 1000;
    static constexpr size_t MAX_SPIKE_HISTORY = 100;
    static constexpr size_t TREND_WINDOW_SIZE = 20;  // For trend analysis
    
    // LEARNING: Thresholds as constexpr for performance
    static constexpr double MARKET_DATA_WARNING_US = 1000.0;   // 1ms
    static constexpr double MARKET_DATA_CRITICAL_US = 5000.0;  // 5ms
    static constexpr double ORDER_PLACEMENT_WARNING_US = 2000.0; // 2ms
    static constexpr double ORDER_PLACEMENT_CRITICAL_US = 10000.0; // 10ms
    static constexpr double TICK_TO_TRADE_WARNING_US = 5000.0;  // 5ms
    static constexpr double TICK_TO_TRADE_CRITICAL_US = 15000.0; // 15ms
    static constexpr double ORDER_CANCELLATION_WARNING_US = 1500.0;  // 1.5ms
    static constexpr double ORDER_CANCELLATION_CRITICAL_US = 3000.0; // 3ms
    
    // LEARNING: explicit constructor prevents accidental conversions
    explicit LatencyTracker(size_t window_size = DEFAULT_WINDOW_SIZE);
    
    // LEARNING: Destructor (automatically generated is fine for our case)
    ~LatencyTracker() = default;
    
    // LEARNING: Delete copy operations for performance/safety
    LatencyTracker(const LatencyTracker&) = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;
    
    // LEARNING: Move operations could be enabled, but not needed for singleton pattern
    LatencyTracker(LatencyTracker&&) = delete;
    LatencyTracker& operator=(LatencyTracker&&) = delete;
    
    // Primary interface - add latency measurements
    void add_latency(LatencyType type, double latency_us);
    void add_latency(LatencyType type, const duration_us_t& duration);
    
    // Convenience methods for common operations
    inline void add_market_data_latency(double latency_us) {
        add_latency(LatencyType::MARKET_DATA_PROCESSING, latency_us);
    }
    
    inline void add_order_placement_latency(double latency_us) {
        add_latency(LatencyType::ORDER_PLACEMENT, latency_us);
    }
    
    inline void add_tick_to_trade_latency(double latency_us) {
        add_latency(LatencyType::TICK_TO_TRADE, latency_us);
    }
    
    // Statistics and reporting
    LatencyStatistics get_statistics(LatencyType type) const;
    std::vector<LatencySpike> get_recent_spikes(int minutes = 5) const;
    bool should_alert() const;
    
    // Performance reporting
    void print_latency_report() const;
    void print_detailed_report() const;
    
    // System monitoring
    size_t get_total_measurements() const;
    size_t get_measurement_count(LatencyType type) const;
    double get_uptime_seconds() const;
    
    // LEARNING: Reset functionality for testing/debugging
    void reset_statistics();
    void clear_spike_history();
    
private:
    // LEARNING: std::array for fixed-size, cache-friendly storage
    // Using static_cast to convert enum to array index
    std::array<std::deque<double>, static_cast<size_t>(LatencyType::COUNT)> latency_windows_;
    
    // Performance trend tracking for each latency type
    std::array<std::deque<double>, static_cast<size_t>(LatencyType::COUNT)> trend_windows_;
    
    // LEARNING: std::deque for efficient front/back operations
    std::deque<LatencySpike> spike_history_;
    
    // Configuration
    size_t window_size_;
    timestamp_t session_start_;
    
    // LEARNING: Helper methods (private implementation details)
    double get_threshold(LatencyType type, SpikesSeverity severity) const;
    void check_and_record_spike(LatencyType type, double latency_us);
    LatencyStatistics calculate_statistics(const std::deque<double>& data) const;
    double calculate_percentile(const std::deque<double>& data, double percentile) const;
    double calculate_standard_deviation(const std::deque<double>& data, double mean) const;
    std::string latency_type_to_string(LatencyType type) const;
    std::string severity_to_string(SpikesSeverity severity) const;
    
    // Performance trend analysis
    PerformanceTrendData calculate_performance_trend(LatencyType type) const;
    std::string trend_to_string(PerformanceTrend trend) const;
    void update_trend_window(LatencyType type, double p95_latency);
    
    // LEARNING: Performance assessment helpers
    std::string assess_performance(const LatencyStatistics& stats, LatencyType type) const;
    bool is_performance_acceptable(const LatencyStatistics& stats, LatencyType type) const;
    
    // Fast time formatting helpers
    void format_spike_timestamp(const LatencySpike& spike, TimeFormatter::TimeBuffer& buffer) const;
};

/**
 * LEARNING GOAL: This class teaches RAII timing patterns
 * Automatically measures scope-based latency
 */
class ScopedLatencyMeasurement {
public:
    // LEARNING: Constructor starts timing automatically
    explicit ScopedLatencyMeasurement(LatencyTracker& tracker, LatencyType type)
        : tracker_(tracker), type_(type), start_time_(now()) {}
    
    // LEARNING: Destructor measures and records latency automatically
    ~ScopedLatencyMeasurement() {
        auto end_time = now();
        auto duration = time_diff_us(start_time_, end_time);
        tracker_.add_latency(type_, duration);
    }
    
    // LEARNING: Non-copyable for safety
    ScopedLatencyMeasurement(const ScopedLatencyMeasurement&) = delete;
    ScopedLatencyMeasurement& operator=(const ScopedLatencyMeasurement&) = delete;
    
private:
    LatencyTracker& tracker_;
    LatencyType type_;
    timestamp_t start_time_;
};

// LEARNING: Convenience macro for easy scope-based timing
#define MEASURE_LATENCY(tracker, type) \
    ScopedLatencyMeasurement _measurement(tracker, type)

// LEARNING: More specific macros for common operations
#define MEASURE_MARKET_DATA_LATENCY(tracker) \
    MEASURE_LATENCY(tracker, LatencyType::MARKET_DATA_PROCESSING)

#define MEASURE_ORDER_LATENCY(tracker) \
    MEASURE_LATENCY(tracker, LatencyType::ORDER_PLACEMENT)

#define MEASURE_TICK_TO_TRADE_LATENCY(tracker) \
    MEASURE_LATENCY(tracker, LatencyType::TICK_TO_TRADE)

} // namespace hft
