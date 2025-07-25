#pragma once

#include "types.hpp"  // Our foundation types
#include <deque>
#include <vector>
#include <array>
#include <atomic>     // For lock-free operations
#include <algorithm>  // LEARNING: STL algorithms (sort, nth_element)
#include <numeric>    // LEARNING: Mathematical operations (accumulate)
#include <string>
#include <iostream>   // LEARNING: Input/output operations
#include <iomanip>    // LEARNING: Output formatting
#include <chrono>     // For time formatting
#include <cstring>    // For fast string operations
#include <cstdio>     // For std::snprintf

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
    ORDER_BOOK_UPDATE = 4,      // Time to update order book
    COUNT = 5                    // Total number of types (useful for arrays)
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
 * High-performance lock-free circular buffer for latency measurements
 * Optimized for single-producer, single-consumer scenarios in HFT
 */
template<size_t SIZE>
class LockFreeCircularBuffer {
private:
    static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be a power of 2");
    static constexpr size_t MASK = SIZE - 1;
    
    alignas(64) std::atomic<size_t> head_{0};  // Producer writes here
    alignas(64) std::atomic<size_t> tail_{0};  // Consumer reads from here
    alignas(64) std::array<double, SIZE> buffer_;
    alignas(64) std::atomic<bool> full_{false};
    
public:
    LockFreeCircularBuffer() = default;
    
    // Disable copy and move operations due to atomic members
    LockFreeCircularBuffer(const LockFreeCircularBuffer&) = delete;
    LockFreeCircularBuffer& operator=(const LockFreeCircularBuffer&) = delete;
    LockFreeCircularBuffer(LockFreeCircularBuffer&&) = delete;
    LockFreeCircularBuffer& operator=(LockFreeCircularBuffer&&) = delete;
    
    // Fast O(1) insertion - lock-free for single producer
    inline bool push(double value) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & MASK;
        
        if (next_head == tail_.load(std::memory_order_acquire)) {
            // Buffer is full - overwrite oldest (ring buffer behavior)
            tail_.store((tail_.load(std::memory_order_relaxed) + 1) & MASK, 
                       std::memory_order_release);
            full_.store(true, std::memory_order_relaxed);
        }
        
        buffer_[current_head] = value;
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    // Get current size - approximate for performance
    inline size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (h >= t) {
            return h - t;
        } else {
            return SIZE - (t - h);
        }
    }
    
    // Check if buffer has ever been full (for statistics)
    inline bool has_been_full() const noexcept {
        return full_.load(std::memory_order_relaxed);
    }
    
    // Copy data for statistics calculation (thread-safe snapshot)
    std::vector<double> snapshot() const {
        std::vector<double> result;
        const size_t current_tail = tail_.load(std::memory_order_acquire);
        const size_t current_head = head_.load(std::memory_order_acquire);
        
        if (current_head == current_tail) {
            return result; // Empty
        }
        
        result.reserve(SIZE);
        size_t pos = current_tail;
        while (pos != current_head) {
            result.push_back(buffer_[pos]);
            pos = (pos + 1) & MASK;
        }
        
        return result;
    }
    
    // Clear the buffer (reset to empty state)
    inline void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        full_.store(false, std::memory_order_relaxed);
    }
    
    // Fast min/max tracking for hot path
    inline std::pair<double, double> fast_min_max() const noexcept {
        double min_val = std::numeric_limits<double>::max();
        double max_val = std::numeric_limits<double>::lowest();
        
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == current_tail) {
            return {0.0, 0.0}; // Empty
        }
        
        size_t pos = current_tail;
        while (pos != current_head) {
            const double val = buffer_[pos];
            min_val = std::min(min_val, val);
            max_val = std::max(max_val, val);
            pos = (pos + 1) & MASK;
        }
        
        return {min_val, max_val};
    }
};

/**
 * Fast approximate percentile calculator using P-Square algorithm
 * O(1) insertion, O(1) percentile estimation - perfect for HFT
 */
class ApproximatePercentile {
private:
    static constexpr size_t MARKERS = 5;
    std::array<double, MARKERS> markers_;    // P-square markers
    std::array<double, MARKERS> positions_;  // Marker positions
    std::array<double, MARKERS> desired_;    // Desired positions
    std::array<double, MARKERS> increments_; // Position increments
    size_t count_;
    double percentile_;
    
public:
    explicit ApproximatePercentile(double percentile) 
        : count_(0), percentile_(percentile) {
        // Initialize P-square algorithm
        double p = percentile / 100.0;
        desired_ = {0.0, p/2.0, p, (1.0+p)/2.0, 1.0};
        increments_ = {0.0, p/2.0, p, (1.0+p)/2.0, 1.0};
        positions_ = {0.0, 1.0, 2.0, 3.0, 4.0};
    }
    
    // O(1) update with new measurement
    inline void update(double value) noexcept {
        if (count_ < MARKERS) {
            // Initial phase - collect first 5 values
            markers_[count_] = value;
            count_++;
            if (count_ == MARKERS) {
                std::sort(markers_.begin(), markers_.end());
            }
            return;
        }
        
        // Find cell k
        size_t k = 0;
        if (value < markers_[0]) {
            markers_[0] = value;
            k = 0;
        } else if (value >= markers_[MARKERS-1]) {
            markers_[MARKERS-1] = value;
            k = MARKERS-2;
        } else {
            for (size_t i = 1; i < MARKERS; ++i) {
                if (value < markers_[i]) {
                    k = i - 1;
                    break;
                }
            }
        }
        
        // Update positions and desired positions
        for (size_t i = k+1; i < MARKERS; ++i) {
            positions_[i] += 1.0;
        }
        
        for (size_t i = 0; i < MARKERS; ++i) {
            desired_[i] += increments_[i];
        }
        
        // Adjust markers using parabolic formula
        for (size_t i = 1; i < MARKERS-1; ++i) {
            double d = desired_[i] - positions_[i];
            if ((d >= 1.0 && positions_[i+1] - positions_[i] > 1.0) ||
                (d <= -1.0 && positions_[i-1] - positions_[i] < -1.0)) {
                
                int sign = (d >= 0) ? 1 : -1;
                double new_marker = parabolic_formula(i, sign);
                
                if (markers_[i-1] < new_marker && new_marker < markers_[i+1]) {
                    markers_[i] = new_marker;
                } else {
                    markers_[i] = linear_formula(i, sign);
                }
                
                positions_[i] += sign;
            }
        }
        
        count_++;
    }
    
    // O(1) percentile estimation
    inline double estimate() const noexcept {
        if (count_ < MARKERS) {
            if (count_ == 0) return 0.0;
            
            // For small samples, use exact calculation
            std::vector<double> sorted(markers_.begin(), markers_.begin() + count_);
            std::sort(sorted.begin(), sorted.end());
            
            double index = (percentile_ / 100.0) * (count_ - 1);
            size_t lower = static_cast<size_t>(index);
            if (lower >= count_ - 1) return sorted.back();
            
            double weight = index - lower;
            return sorted[lower] * (1.0 - weight) + sorted[lower + 1] * weight;
        }
        
        return markers_[2]; // Middle marker approximates the percentile
    }
    
    inline size_t sample_count() const noexcept { return count_; }
    
private:
    double parabolic_formula(size_t i, int d) const noexcept {
        double qi_1 = markers_[i-1];
        double qi = markers_[i];
        double qi_p1 = markers_[i+1];
        double ni_1 = positions_[i-1];
        double ni = positions_[i];
        double ni_p1 = positions_[i+1];
        
        return qi + d * ((ni - ni_1 + d) * (qi_p1 - qi) / (ni_p1 - ni) +
                        (ni_p1 - ni - d) * (qi - qi_1) / (ni - ni_1)) / (ni_p1 - ni_1);
    }
    
    double linear_formula(size_t i, int d) const noexcept {
        if (d == 1) {
            return markers_[i] + (markers_[i+1] - markers_[i]) / (positions_[i+1] - positions_[i]);
        } else {
            return markers_[i] - (markers_[i-1] - markers_[i]) / (positions_[i] - positions_[i-1]);
        }
    }
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
 * OPTIMIZATIONS:
 * - Lock-free circular buffers for hot path operations
 * - Approximate percentile calculation (O(1) instead of O(n log n))
 * - Fast path for spike detection only
 * - Minimal memory allocations
 */
class LatencyTracker {
public:
    // LEARNING: constexpr for compile-time constants
    static constexpr size_t DEFAULT_WINDOW_SIZE = 1024;  // Power of 2 for lock-free buffer
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
    
    // Primary interface - add latency measurements (OPTIMIZED HOT PATH)
    void add_latency(LatencyType type, double latency_us);
    void add_latency(LatencyType type, const duration_us_t& duration);
    
    // FAST PATH: Only spike detection, no statistics
    inline void add_latency_fast_path(LatencyType type, double latency_us) noexcept {
        // Only do critical operations in hot path
        fast_buffers_[static_cast<size_t>(type)].push(latency_us);
        check_and_record_spike_fast(type, latency_us);
    }
    
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
    
    // Statistics and reporting (OPTIMIZED)
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
    // Configuration (initialized first)
    size_t window_size_;
    timestamp_t session_start_;
    
    // Approximate percentile calculators (initialized in constructor)
    mutable std::array<ApproximatePercentile, static_cast<size_t>(LatencyType::COUNT)> p95_calculators_;
    mutable std::array<ApproximatePercentile, static_cast<size_t>(LatencyType::COUNT)> p99_calculators_;
    
    // HIGH-PERFORMANCE STORAGE: Lock-free circular buffers
    using FastBuffer = LockFreeCircularBuffer<1024>;
    std::array<FastBuffer, static_cast<size_t>(LatencyType::COUNT)> fast_buffers_;
    
    // Fallback: Keep deques for compatibility and detailed analysis
    std::array<std::deque<double>, static_cast<size_t>(LatencyType::COUNT)> latency_windows_;
    
    // Performance trend tracking for each latency type
    std::array<std::deque<double>, static_cast<size_t>(LatencyType::COUNT)> trend_windows_;
    
    // LEARNING: std::deque for efficient front/back operations
    std::deque<LatencySpike> spike_history_;
    
    // LEARNING: Helper methods (private implementation details)
    double get_threshold(LatencyType type, SpikesSeverity severity) const noexcept;
    void check_and_record_spike(LatencyType type, double latency_us);
    inline void check_and_record_spike_fast(LatencyType type, double latency_us) noexcept {
        // Only check thresholds, don't record detailed spike info in hot path
        double critical = get_threshold(type, SpikesSeverity::CRITICAL);
        
        // Only record critical spikes in fast path to minimize overhead
        if (latency_us > critical) {
            // Minimize allocation and operations in hot path
            LatencySpike spike(now(), type, latency_us, SpikesSeverity::CRITICAL);
            
            // Use lock-free push if possible, or minimize lock time
            if (spike_history_.size() < MAX_SPIKE_HISTORY) {
                spike_history_.push_back(spike);
            } else {
                // Overwrite oldest in ring buffer fashion for performance
                spike_history_.pop_front();
                spike_history_.push_back(spike);
            }
        }
    }
    LatencyStatistics calculate_statistics(const std::vector<double>& data) const;
    LatencyStatistics calculate_statistics_fast(LatencyType type) const;
    double calculate_percentile(const std::deque<double>& data, double percentile) const;
    double calculate_percentile_fast(const std::vector<double>& data, double percentile) const;
    double calculate_standard_deviation(const std::vector<double>& data, double mean) const;
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

/**
 * OPTIMIZED: Fast path scoped measurement for hot trading operations
 */
class FastScopedLatencyMeasurement {
public:
    explicit FastScopedLatencyMeasurement(LatencyTracker& tracker, LatencyType type) noexcept
        : tracker_(tracker), type_(type), start_time_(now()) {}
    
    ~FastScopedLatencyMeasurement() {
        auto end_time = now();
        double latency_us = to_microseconds(time_diff_us(start_time_, end_time));
        tracker_.add_latency_fast_path(type_, latency_us);
    }
    
    FastScopedLatencyMeasurement(const FastScopedLatencyMeasurement&) = delete;
    FastScopedLatencyMeasurement& operator=(const FastScopedLatencyMeasurement&) = delete;
    
private:
    LatencyTracker& tracker_;
    LatencyType type_;
    timestamp_t start_time_;
};

// LEARNING: Convenience macro for easy scope-based timing
#define MEASURE_LATENCY(tracker, type) \
    ScopedLatencyMeasurement _measurement(tracker, type)

// OPTIMIZED: Fast path macros for hot trading operations
#define MEASURE_LATENCY_FAST(tracker, type) \
    FastScopedLatencyMeasurement _fast_measurement(tracker, type)

// LEARNING: More specific macros for common operations
#define MEASURE_MARKET_DATA_LATENCY(tracker) \
    MEASURE_LATENCY(tracker, LatencyType::MARKET_DATA_PROCESSING)

#define MEASURE_ORDER_LATENCY(tracker) \
    MEASURE_LATENCY(tracker, LatencyType::ORDER_PLACEMENT)

#define MEASURE_TICK_TO_TRADE_LATENCY(tracker) \
    MEASURE_LATENCY(tracker, LatencyType::TICK_TO_TRADE)

// OPTIMIZED: Fast path macros for hot operations
#define MEASURE_MARKET_DATA_LATENCY_FAST(tracker) \
    MEASURE_LATENCY_FAST(tracker, LatencyType::MARKET_DATA_PROCESSING)

#define MEASURE_ORDER_LATENCY_FAST(tracker) \
    MEASURE_LATENCY_FAST(tracker, LatencyType::ORDER_PLACEMENT)

#define MEASURE_ORDER_BOOK_UPDATE_FAST(tracker) \
    MEASURE_LATENCY_FAST(tracker, LatencyType::ORDER_BOOK_UPDATE)

} // namespace hft
