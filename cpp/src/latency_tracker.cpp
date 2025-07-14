#include "latency_tracker.hpp"
#include <algorithm>  // For sorting and statistical operations
#include <cmath>      // For mathematical functions (sqrt, etc.)

namespace hft {

// =============================================================================
// CONSTRUCTOR AND BASIC SETUP
// =============================================================================

LatencyTracker::LatencyTracker(size_t window_size) 
    : window_size_(window_size), session_start_(now()) {}

// =============================================================================
// PRIMARY INTERFACE - ADD LATENCY MEASUREMENTS
// =============================================================================

void LatencyTracker::add_latency(LatencyType type, double latency_us) {
    // TODO 3: You need to add this latency measurement to the correct deque
    // Think about how to convert the enum to an array index
    // Consider which deque operation adds to the end
    
    // TODO 4: Implement rolling window behavior
    // What happens when a deque gets too large?
    // Which deque operation removes from the beginning?
    
    size_t index = static_cast<size_t>(type);
    latency_windows_[index].push_back(latency_us);
    
    
    // TODO 4: Maintain rolling window size
    // LEARNING: If window exceeds max size, remove oldest element
    // HINT: Check if size() > window_size_, then pop_front()

    if (latency_windows_[index].size() > window_size_) {
        latency_windows_[index].pop_front();
    }
    
    // TODO 5: Check for latency spikes
    // HINT: Call check_and_record_spike(type, latency_us);


}

void LatencyTracker::add_latency(LatencyType type, const duration_us_t& duration) {
    // TODO 6: Convert the duration to a double value
    // Look for a helper function in types.hpp that converts durations
    // Then call the other add_latency method

    double latency_us = to_microseconds(duration);
    add_latency(type, latency_us);
}

// =============================================================================
// STATISTICS CALCULATION
// =============================================================================

LatencyStatistics LatencyTracker::get_statistics(LatencyType type) const {
    // TODO 7: Get the deque for this latency type
    // How do you access the correct deque from the array?

    size_t index = static_cast<size_t>(type);
    const auto& data = latency_windows_[index];
    
    // TODO 8: Handle the case where there's no data
    // What should you return if the deque is empty?
    if (data.empty()) {
        return LatencyStatistics{};
    }
    
    // TODO 9: Calculate statistics from the data
    // Which helper method processes the deque data?
    return calculate_statistics(data);
}

LatencyStatistics LatencyTracker::calculate_statistics(const std::deque<double>& data) const {
    LatencyStatistics stats;
    
    // TODO 10: Set the count of measurements
    // How many elements are in the deque?
    uint64_t count = data.size();
    
    // TODO 11: Find the minimum and maximum values
    // Look for STL algorithms that find min/max elements
    auto min_it = std::min_element(data.begin(), data.end());
    auto max_it = std::max_element(data.begin(), data.end());

    stats.min_us = *min_it;
    stats.max_us = *max_it;
    
    // TODO 12: Calculate the average (mean)
    // Use std::accumulate to sum all values, then divide
    double sum = std::accumulate(data.begin(), data.end(), 0.0);
    stats.mean_us = sum / data.size();
    
    // TODO 13: Calculate the median (50th percentile)
    // Use the percentile calculation helper

    stats.median_us = calculate_percentile(data, 50.0);
    // TODO 14: Calculate P95 and P99 percentiles
    // What percentiles are important for HFT performance?
    stats.p95_us = calculate_percentile(data, 95.0);
    stats.p99_us = calculate_percentile(data, 99.0);
    
    // TODO 15: Calculate standard deviation
    // Use the helper method for standard deviation
    stats.std_dev_us = calculate_standard_deviation(data, stats.mean_us);
    
    return stats;
}

double LatencyTracker::calculate_percentile(const std::deque<double>& data, double percentile) const {
    // TODO 16: Handle edge cases first
    // What if the data is empty or percentile is out of range?
    if (data.empty()) {
        return 0.0;
    }

    if (percentile < 0.0 || percentile > 100.0) {
        return 0.0;
    }
    
    // TODO 17: Sort the data
    // Which STL algorithm sorts a vector?
    std::sort(data.begin(), data.end());
    
    // TODO 18: Calculate the index for this percentile
    // Formula: (percentile/100) * (n-1)
    double index = (percentile/100) * (data.size()-1);
    
    // TODO 19: Handle exact vs interpolated percentiles
    // If index is whole number, return exact value
    // If not, interpolate between two values
    size_t lower_index = static_cast<size_t>(index);
    if (lower_index >= data.size() - 1) {
        return data.back();
    }

    double weight = index - lower_index;
    return data[lower_index] * (1.0 - weight) + data[lower_index + 1] * weight;
}

double LatencyTracker::calculate_standard_deviation(const std::deque<double>& data, double mean) const {
    // TODO 20: Calculate variance
    // Variance = average of squared differences from mean
    // Use std::accumulate with a lambda function
    double variance = std::accumulate(data.begin(), data.end(), 0.0,
        [mean](double acc, double value) {
            double diff = value - mean;
            return acc + diff * diff;
        }) / data.size();
    
    // TODO 21: Return square root of variance
    // Which math function calculates square root?
    
    return std::sqrt(variance); // TODO: Replace with actual calculation
}

// =============================================================================
// SPIKE DETECTION AND MANAGEMENT
// =============================================================================

void LatencyTracker::check_and_record_spike(LatencyType type, double latency_us) {
    // TODO 22: Get the thresholds for this latency type
    // What method returns the warning and critical thresholds?
    double warning = get_threshold(type, SpikesSeverity::WARNING);
    double critical = get_threshold(type, SpikesSeverity::CRITICAL);
    
    // TODO 23: Check if latency exceeds thresholds
    // Compare latency against warning and critical thresholds
    // Remember to check critical first (higher threshold)
    
    // TODO 24: Create and add spike to history
    // Create a LatencySpike object with current timestamp
    // Add it to spike_history_ deque
    if (latency_us > critical) {
        LatencySpike spike(now(), type, latency_us, SpikesSeverity::CRITICAL);
        spike_history_.push_back(spike);
        if (spike_history_.size() > MAX_SPIKE_HISTORY) {
            spike_history_.pop_front();
        }
    }
    else if (latency_us > warning) {
        LatencySpike spike(now(), type, latency_us, SpikesSeverity::WARNING);
        spike_history_.push_back(spike);
        if (spike_history_.size() > MAX_SPIKE_HISTORY) {
            spike_history_.pop_front();
        }
    }
    
    // TODO 25: Maintain spike history size
    // What happens if spike_history_ gets too large?
    // Which deque operation removes from the front?
}

double LatencyTracker::get_threshold(LatencyType type, SpikesSeverity severity) const {
    // TODO 26: Return appropriate threshold based on type and severity
    // Use switch statement on LatencyType
    // Then check severity (WARNING vs CRITICAL)
    switch(type) {
        case LatencyType::MARKET_DATA_PROCESSING:
            if (severity == SpikesSeverity::WARNING) {
                return MARKET_DATA_WARNING_US;
            }
            else if (severity == SpikesSeverity::CRITICAL) {
                return MARKET_DATA_CRITICAL_US;
            }
            else {
                return 0.0;
            }
        case LatencyType::ORDER_PLACEMENT:
            if (severity == SpikesSeverity::WARNING) {
                return ORDER_PLACEMENT_WARNING_US;
            }
            else if (severity == SpikesSeverity::CRITICAL) {
                return ORDER_PLACEMENT_CRITICAL_US;
            }
            else {
                return 0.0;
            }
        case LatencyType::ORDER_CANCELLATION:
            if (severity == SpikesSeverity::WARNING) {
                return ORDER_CANCELLATION_WARNING_US;
            }
            else if (severity == SpikesSeverity::CRITICAL) {
                return ORDER_CANCELLATION_CRITICAL_US;
            }
            else {
                return 0.0;
            }
        case LatencyType::TICK_TO_TRADE:
            if (severity == SpikesSeverity::WARNING) {
                return TICK_TO_TRADE_WARNING_US;
            }
            else if (severity == SpikesSeverity::CRITICAL) {
                return TICK_TO_TRADE_CRITICAL_US;
            }
            else {
                return 0.0;
            }
        default:
            return 0.0;
    }
}

std::vector<LatencySpike> LatencyTracker::get_recent_spikes(int minutes) const {
    // TODO 27: Calculate cutoff time
    // Current time minus specified minutes
    auto cutoff_time = now() - std::chrono::minutes(minutes);
    
    // TODO 28: Filter spikes by timestamp
    // Use std::copy_if to find spikes newer than cutoff
    // Return vector of recent spikes
    std::vector<LatencySpike> spikes;

    std::copy_if(spike_history_.begin(), spike_history_.end(),
                 std::back_inserter(spikes),
                 [cutoff_time](const LatencySpike& spike) {
                    return spike.timestamp >= cutoff_time;
                 });
    
    return spikes; // TODO: Replace with actual filtering
}

bool LatencyTracker::should_alert() const {
    // TODO 29: Get recent spikes (last 1 minute)
    // Use get_recent_spikes() method
    std::vector<LatencySpike> spikes = get_recent_spikes(1);
    
    // TODO 30: Count critical and warning spikes
    // Use std::count_if to count by severity
    size_t critical_count = std::count_if(spikes.begin(), spikes.end(),
                            [](const LatencySpike& spike) {
                                return spike.severity == SpikesSeverity::CRITICAL;
                            });
                
    size_t warning_count = std::count_if(spikes.begin(), spikes.end(),
                        [](const LatencySpike& spike) {
                            return spike.severity == SpikesSeverity::WARNING;
                        });
    
    // TODO 31: Return alert condition
    // Alert if any critical spikes OR more than 3 warning spikes
    return critical_count > 0 || warning_count > 3;
}

// =============================================================================
// STRING CONVERSION HELPERS
// =============================================================================

std::string LatencyTracker::latency_type_to_string(LatencyType type) const {
    // TODO 32: Convert enum to readable string
    // Use switch statement to return descriptive names
    
    return "Unknown"; // TODO: Replace with switch statement
}

std::string LatencyTracker::severity_to_string(SpikesSeverity severity) const {
    // TODO 33: Convert severity enum to string
    // Simple switch statement or ternary operator
    
    return "Unknown"; // TODO: Replace with actual conversion
}

// =============================================================================
// PERFORMANCE ASSESSMENT
// =============================================================================

std::string LatencyTracker::assess_performance(const LatencyStatistics& stats, LatencyType type) const {
    // TODO 34: Assess performance based on P95 latency
    // Compare P95 against warning/critical thresholds
    // Return performance grade (Excellent, Good, Poor, etc.)
    
    return "Unknown"; // TODO: Replace with assessment logic
}

bool LatencyTracker::is_performance_acceptable(const LatencyStatistics& stats, LatencyType type) const {
    // TODO 35: Return true if performance meets HFT standards
    // Compare P95 against warning threshold
    
    return true; // TODO: Replace with actual check
}

// =============================================================================
// REPORTING AND OUTPUT
// =============================================================================

void LatencyTracker::print_latency_report() const {
    // TODO 36: Print summary report
    // Use std::setw, std::setprecision for formatting
    // Loop through each LatencyType and print statistics
    
    std::cout << "\n=== LATENCY SUMMARY REPORT ===" << std::endl;
    // TODO: Implement table formatting and data display
}

void LatencyTracker::print_detailed_report() const {
    // TODO 37: Print comprehensive report with spike analysis
    // Include uptime, total measurements, recent spikes
    // Call print_latency_report() first, then add details
    
    std::cout << "\n=== DETAILED LATENCY REPORT ===" << std::endl;
    // TODO: Implement detailed reporting
}

// =============================================================================
// SYSTEM MONITORING
// =============================================================================

size_t LatencyTracker::get_total_measurements() const {
    // TODO 38: Sum measurements across all latency types
    // Use std::accumulate to sum deque sizes
    
    return 0; // TODO: Replace with actual calculation
}

size_t LatencyTracker::get_measurement_count(LatencyType type) const {
    // TODO 39: Return count for specific latency type
    // How do you get the size of a specific deque?
    
    return 0; // TODO: Replace with actual count
}

double LatencyTracker::get_uptime_seconds() const {
    // TODO 40: Calculate session uptime
    // Current time minus session start time
    // Convert to seconds
    
    return 0.0; // TODO: Replace with actual calculation
}

// =============================================================================
// UTILITY METHODS
// =============================================================================

void LatencyTracker::reset_statistics() {
    // TODO 41: Clear all latency windows and reset session start
    // Clear each deque in the array
    // Reset session_start_ to current time
}

void LatencyTracker::clear_spike_history() {
    // TODO 42: Clear spike history
    // Which deque method clears all elements?
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

