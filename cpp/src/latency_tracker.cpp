#include "latency_tracker.hpp"
#include <algorithm>  // For sorting and statistical operations
#include <cmath>      // For mathematical functions (sqrt, etc.)

namespace hft {

// =============================================================================
// CONSTRUCTOR AND BASIC SETUP
// =============================================================================

LatencyTracker::LatencyTracker(size_t window_size) 
    : window_size_(window_size), session_start_(now()) {
    
    // TODO 1: Initialize the latency windows
    // LEARNING: You need to set the max size for each deque in the array
    // HINT: Use a range-based for loop over latency_windows_
    // HINT: Each deque should have maxlen = window_size
    // 
    for (auto& window : latency_windows_) {
        
    }
    
    // TODO 2: Reserve space for spike history for performance
    // HINT: spike_history_.reserve(???)
}

// =============================================================================
// PRIMARY INTERFACE - ADD LATENCY MEASUREMENTS
// =============================================================================

void LatencyTracker::add_latency(LatencyType type, double latency_us) {
    // TODO 3: Add latency to the appropriate window
    // LEARNING: Convert enum to array index using static_cast
    // HINT: size_t index = static_cast<size_t>(type);
    // HINT: latency_windows_[index].push_back(latency_us);
    
    // TODO 4: Maintain rolling window size
    // LEARNING: If window exceeds max size, remove oldest element
    // HINT: Check if size() > window_size_, then pop_front()
    
    // TODO 5: Check for latency spikes
    // HINT: Call check_and_record_spike(type, latency_us);
}

void LatencyTracker::add_latency(LatencyType type, const duration_us_t& duration) {
    // TODO 6: Convert duration to microseconds and call the other overload
    // LEARNING: Use to_microseconds() helper from types.hpp
    // HINT: double latency_us = to_microseconds(duration);
    // HINT: Then call: add_latency(type, latency_us);
}

// =============================================================================
// STATISTICS CALCULATION
// =============================================================================

LatencyStatistics LatencyTracker::get_statistics(LatencyType type) const {
    // TODO 7: Get the data for the specified type
    // HINT: const auto& data = latency_windows_[static_cast<size_t>(type)];
    
    // TODO 8: Handle empty data case
    // HINT: if (data.empty()) { return LatencyStatistics{}; }
    
    // TODO 9: Call helper method to calculate statistics
    // HINT: return calculate_statistics(data);
}

LatencyStatistics LatencyTracker::calculate_statistics(const std::deque<double>& data) const {
    LatencyStatistics stats;
    
    // TODO 10: Set basic counts
    // HINT: stats.count = data.size();
    
    // TODO 11: Find min and max
    // LEARNING: Use std::min_element and std::max_element
    // HINT: auto min_it = std::min_element(data.begin(), data.end());
    // HINT: stats.min_us = *min_it;
    
    // TODO 12: Calculate mean (average)
    // LEARNING: Use std::accumulate from <numeric>
    // HINT: double sum = std::accumulate(data.begin(), data.end(), 0.0);
    // HINT: stats.mean_us = sum / data.size();
    
    // TODO 13: Calculate median (50th percentile)
    // HINT: stats.median_us = calculate_percentile(???, 50.0);
    
    // TODO 14: Calculate P95 and P99
    // HINT: stats.p95_us = calculate_percentile(???, 95.0);
    // HINT: stats.p99_us = calculate_percentile(???, 99.0);
    
    // TODO 15: Calculate standard deviation
    // HINT: stats.std_dev_us = calculate_standard_deviation(data, stats.mean_us);
    
    return stats;
}

double LatencyTracker::calculate_percentile(std::vector<double> data, double percentile) const {
    // TODO 16: Handle edge cases
    // HINT: if (data.empty()) return 0.0;
    // HINT: if (percentile <= 0.0) return data[0]; (after sorting)
    // HINT: if (percentile >= 100.0) return data.back(); (after sorting)
    
    // TODO 17: Sort the data
    // LEARNING: std::sort modifies the vector in-place
    // HINT: std::sort(data.begin(), data.end());
    
    // TODO 18: Calculate percentile index
    // LEARNING: Percentile formula: (percentile/100) * (n-1)
    // HINT: double index = (percentile / 100.0) * (data.size() - 1);
    
    // TODO 19: Handle exact vs interpolated percentiles
    // LEARNING: If index is whole number, return exact value
    //           If not, interpolate between two values
    // HINT: size_t lower_index = static_cast<size_t>(index);
    // HINT: size_t upper_index = lower_index + 1;
    // HINT: Use linear interpolation if needed
    
    return 0.0; // TODO: Replace with actual calculation
}

double LatencyTracker::calculate_standard_deviation(const std::deque<double>& data, double mean) const {
    // TODO 20: Calculate variance
    // LEARNING: Variance = average of squared differences from mean
    // HINT: Use std::accumulate with a lambda function
    // HINT: auto variance = std::accumulate(data.begin(), data.end(), 0.0,
    //           [mean](double acc, double val) {
    //               double diff = val - mean;
    //               return acc + diff * diff;
    //           }) / data.size();
    
    // TODO 21: Return square root of variance
    // HINT: return std::sqrt(variance);
    
    return 0.0; // TODO: Replace with actual calculation
}

// =============================================================================
// SPIKE DETECTION AND MANAGEMENT
// =============================================================================

void LatencyTracker::check_and_record_spike(LatencyType type, double latency_us) {
    // TODO 22: Get thresholds for this latency type
    // HINT: double warning_threshold = get_threshold(type, SpikesSeverity::WARNING);
    // HINT: double critical_threshold = get_threshold(type, SpikesSeverity::CRITICAL);
    
    // TODO 23: Check if latency exceeds thresholds
    // LEARNING: Check critical first (higher threshold), then warning
    // HINT: if (latency_us > critical_threshold) {
    //           // Create and add CRITICAL spike
    //       } else if (latency_us > warning_threshold) {
    //           // Create and add WARNING spike
    //       }
    
    // TODO 24: Create LatencySpike object and add to history
    // HINT: LatencySpike spike(now(), type, latency_us, severity);
    // HINT: spike_history_.push_back(spike);
    
    // TODO 25: Maintain spike history size
    // HINT: Check if spike_history_.size() > MAX_SPIKE_HISTORY
    // HINT: If so, remove oldest: spike_history_.pop_front();
}

double LatencyTracker::get_threshold(LatencyType type, SpikesSeverity severity) const {
    // TODO 26: Use switch statement to return appropriate threshold
    // LEARNING: Switch on LatencyType, then check severity
    // HINT: switch (type) {
    //           case LatencyType::MARKET_DATA_PROCESSING:
    //               return (severity == SpikesSeverity::WARNING) ? 
    //                      MARKET_DATA_WARNING_US : MARKET_DATA_CRITICAL_US;
    //           // ... other cases
    //       }
    
    return 0.0; // TODO: Replace with switch statement
}

std::vector<LatencySpike> LatencyTracker::get_recent_spikes(int minutes) const {
    // TODO 27: Calculate cutoff time
    // LEARNING: Current time minus specified minutes
    // HINT: auto cutoff_time = now() - std::chrono::minutes(minutes);
    
    // TODO 28: Filter spikes by timestamp
    // LEARNING: Use std::copy_if with back_inserter
    // HINT: std::vector<LatencySpike> recent_spikes;
    // HINT: std::copy_if(spike_history_.begin(), spike_history_.end(),
    //                    std::back_inserter(recent_spikes),
    //                    [cutoff_time](const LatencySpike& spike) {
    //                        return spike.timestamp > cutoff_time;
    //                    });
    
    return {}; // TODO: Replace with actual filtering
}

bool LatencyTracker::should_alert() const {
    // TODO 29: Get recent spikes (last 1 minute)
    // HINT: auto recent_spikes = get_recent_spikes(1);
    
    // TODO 30: Count critical and warning spikes
    // LEARNING: Use std::count_if to count by condition
    // HINT: auto critical_count = std::count_if(recent_spikes.begin(), recent_spikes.end(),
    //                                          [](const LatencySpike& spike) {
    //                                              return spike.severity == SpikesSeverity::CRITICAL;
    //                                          });
    
    // TODO 31: Return alert condition
    // HINT: Return true if any critical spikes OR more than 3 warning spikes
    
    return false; // TODO: Replace with actual logic
}

// =============================================================================
// STRING CONVERSION HELPERS
// =============================================================================

std::string LatencyTracker::latency_type_to_string(LatencyType type) const {
    // TODO 32: Convert enum to readable string
    // HINT: switch (type) {
    //           case LatencyType::MARKET_DATA_PROCESSING: return "Market Data Processing";
    //           case LatencyType::ORDER_PLACEMENT: return "Order Placement";
    //           // ... other cases
    //       }
    
    return "Unknown"; // TODO: Replace with switch statement
}

std::string LatencyTracker::severity_to_string(SpikesSeverity severity) const {
    // TODO 33: Convert severity enum to string
    // HINT: Simple switch statement or ternary operator
    
    return "Unknown"; // TODO: Replace with actual conversion
}

// =============================================================================
// PERFORMANCE ASSESSMENT
// =============================================================================

std::string LatencyTracker::assess_performance(const LatencyStatistics& stats, LatencyType type) const {
    // TODO 34: Assess performance based on P95 latency
    // LEARNING: Compare P95 against warning/critical thresholds
    // HINT: double warning_threshold = get_threshold(type, SpikesSeverity::WARNING);
    // HINT: if (stats.p95_us < warning_threshold * 0.5) return "Excellent";
    // HINT: else if (stats.p95_us < warning_threshold) return "Good";
    // HINT: // ... other conditions
    
    return "Unknown"; // TODO: Replace with assessment logic
}

bool LatencyTracker::is_performance_acceptable(const LatencyStatistics& stats, LatencyType type) const {
    // TODO 35: Return true if performance meets HFT standards
    // HINT: Compare P95 against warning threshold
    
    return true; // TODO: Replace with actual check
}

// =============================================================================
// REPORTING AND OUTPUT
// =============================================================================

void LatencyTracker::print_latency_report() const {
    // TODO 36: Print summary report
    // LEARNING: Format output with std::setw, std::setprecision
    // HINT: std::cout << std::setw(20) << std::left << "Metric";
    // HINT: Loop through each LatencyType and print statistics
    
    std::cout << "\n=== LATENCY SUMMARY REPORT ===" << std::endl;
    // TODO: Implement table formatting and data display
}

void LatencyTracker::print_detailed_report() const {
    // TODO 37: Print comprehensive report with spike analysis
    // HINT: Include uptime, total measurements, recent spikes
    // HINT: Call print_latency_report() first, then add details
    
    std::cout << "\n=== DETAILED LATENCY REPORT ===" << std::endl;
    // TODO: Implement detailed reporting
}

// =============================================================================
// SYSTEM MONITORING
// =============================================================================

size_t LatencyTracker::get_total_measurements() const {
    // TODO 38: Sum measurements across all latency types
    // LEARNING: Use std::accumulate to sum deque sizes
    // HINT: return std::accumulate(latency_windows_.begin(), latency_windows_.end(), 0UL,
    //                              [](size_t sum, const std::deque<double>& window) {
    //                                  return sum + window.size();
    //                              });
    
    return 0; // TODO: Replace with actual calculation
}

size_t LatencyTracker::get_measurement_count(LatencyType type) const {
    // TODO 39: Return count for specific latency type
    // HINT: return latency_windows_[static_cast<size_t>(type)].size();
    
    return 0; // TODO: Replace with actual count
}

double LatencyTracker::get_uptime_seconds() const {
    // TODO 40: Calculate session uptime
    // LEARNING: Current time minus session start time
    // HINT: auto uptime = time_diff_us(session_start_, now());
    // HINT: return to_microseconds(uptime) / 1000000.0; // Convert to seconds
    
    return 0.0; // TODO: Replace with actual calculation
}

// =============================================================================
// UTILITY METHODS
// =============================================================================

void LatencyTracker::reset_statistics() {
    // TODO 41: Clear all latency windows and reset session start
    // HINT: for (auto& window : latency_windows_) { window.clear(); }
    // HINT: session_start_ = now();
}

void LatencyTracker::clear_spike_history() {
    // TODO 42: Clear spike history
    // HINT: spike_history_.clear();
}

} // namespace hft

/*
================================================================================
LEARNING ROADMAP - SUGGESTED IMPLEMENTATION ORDER:
================================================================================

PHASE 1 - BASIC FUNCTIONALITY (Start here):
- TODO 1-2: Constructor setup
- TODO 3-6: Basic add_latency methods
- TODO 32-33: String conversion helpers
- TODO 41-42: Reset methods

PHASE 2 - STATISTICS (Core learning):
- TODO 10-15: Basic statistics (mean, min, max)
- TODO 16-19: Percentile calculation (challenging!)
- TODO 20-21: Standard deviation

PHASE 3 - SPIKE DETECTION:
- TODO 22-26: Threshold management and spike detection
- TODO 27-31: Recent spike filtering and alerting

PHASE 4 - REPORTING:
- TODO 34-35: Performance assessment
- TODO 36-37: Report formatting
- TODO 38-40: System monitoring

LEARNING TIPS:
1. Implement one TODO at a time
2. Test each method as you complete it
3. Use std::cout to debug intermediate values
4. Don't worry about perfect formatting initially
5. Focus on correctness first, optimization later

TESTING STRATEGY:
After implementing each phase, create a simple test:
  LatencyTracker tracker;
  tracker.add_market_data_latency(1500.0);  // Should trigger warning
  tracker.print_latency_report();

Good luck! Each TODO teaches important C++ and HFT concepts! 🚀
*/

