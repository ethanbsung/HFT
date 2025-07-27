#include "latency_tracker.hpp"
#include <algorithm>  // For sorting and statistical operations
#include <cmath>      // For mathematical functions (sqrt, etc.)

namespace hft {

// =============================================================================
// CONSTRUCTOR AND BASIC SETUP
// =============================================================================

LatencyTracker::LatencyTracker(size_t window_size) 
    : window_size_(window_size), session_start_(now()),
      p95_calculators_{{ApproximatePercentile(95.0), ApproximatePercentile(95.0), 
                       ApproximatePercentile(95.0), ApproximatePercentile(95.0), 
                       ApproximatePercentile(95.0), ApproximatePercentile(95.0)}},
      p99_calculators_{{ApproximatePercentile(99.0), ApproximatePercentile(99.0), 
                       ApproximatePercentile(99.0), ApproximatePercentile(99.0), 
                       ApproximatePercentile(99.0), ApproximatePercentile(99.0)}} {}

// =============================================================================
// PRIMARY INTERFACE - ADD LATENCY MEASUREMENTS (OPTIMIZED)
// =============================================================================

void LatencyTracker::add_latency(LatencyType type, double latency_us) {
    size_t index = static_cast<size_t>(type);
    
    // Add to legacy deque for compatibility and accurate window management
    latency_windows_[index].push_back(latency_us);
    
    // Maintain rolling window size for legacy deque
    while (latency_windows_[index].size() > window_size_) {
        latency_windows_[index].pop_front();
    }
    
    // Add to fast buffer only if it can maintain same size as deque
    // Fast buffer has fixed size 1024, so only use it for small windows
    if (window_size_ <= 1024) {
        fast_buffers_[index].push(latency_us);
    }
    
    // Update approximate percentile calculators (O(1) operation)
    p95_calculators_[index].update(latency_us);
    p99_calculators_[index].update(latency_us);
    
    // Check for latency spikes
    check_and_record_spike(type, latency_us);
    
    // Update trend tracking with approximate P95 (much faster)
    if (latency_windows_[index].size() >= 20) {  // Use deque size for accuracy
        double p95_estimate = p95_calculators_[index].estimate();
        update_trend_window(type, p95_estimate);
    }
}

void LatencyTracker::add_latency(LatencyType type, const duration_us_t& duration) {
    double latency_us = to_microseconds(duration);
    add_latency(type, latency_us);
}

// =============================================================================
// OPTIMIZED STATISTICS CALCULATION
// =============================================================================

LatencyStatistics LatencyTracker::get_statistics(LatencyType type) const {
    size_t index = static_cast<size_t>(type);
    const auto& data = latency_windows_[index];
    
    // Check if we have fast buffer data even when regular data is empty
    auto fast_snapshot = fast_buffers_[index].snapshot();
    
    // Use precise calculation for small datasets (tests), small window sizes, 
    // or when we need exact window size compliance
    if (!data.empty() && (data.size() <= 100 || window_size_ <= 100)) {
        std::vector<double> vec_data(data.begin(), data.end());
        auto stats = calculate_statistics(vec_data);
        stats.trend = calculate_performance_trend(type);
        return stats;
    }
    
    // Use fast path only for larger datasets where approximate calculation is acceptable
    if (p95_calculators_[index].sample_count() >= 100 && !fast_snapshot.empty() && !data.empty()) {
        auto stats = calculate_statistics_fast(type);
        // Ensure count respects the actual window size from deque
        stats.count = data.size();
        return stats;
    }
    
    // If regular data is empty but we have fast buffer data, use fast path stats
    if (data.empty()) {
        if (!fast_snapshot.empty()) {
            auto stats = calculate_statistics_fast(type);
            return stats;
        }
        return LatencyStatistics{};
    }
    
    // Fallback to precise calculation
    std::vector<double> vec_data(data.begin(), data.end());
    auto stats = calculate_statistics(vec_data);
    stats.trend = calculate_performance_trend(type);
    return stats;
}

LatencyStatistics LatencyTracker::calculate_statistics_fast(LatencyType type) const {
    LatencyStatistics stats;
    size_t index = static_cast<size_t>(type);
    
    // Get data from fast buffer
    auto snapshot = fast_buffers_[index].snapshot();
    if (snapshot.empty()) {
        return stats;
    }
    
    stats.count = snapshot.size();
    
    // Fast min/max from circular buffer
    auto min_max = fast_buffers_[index].fast_min_max();
    stats.min_us = min_max.first;
    stats.max_us = min_max.second;
    
    // Calculate mean using fast accumulation
    double sum = std::accumulate(snapshot.begin(), snapshot.end(), 0.0);
    stats.mean_us = sum / snapshot.size();
    
    // Use approximate percentiles (O(1) operation!)
    stats.p95_us = p95_calculators_[index].estimate();
    stats.p99_us = p99_calculators_[index].estimate();
    
    // For median, we can approximate as 50th percentile
    // Or use fast calculation for small datasets
    if (snapshot.size() < 100) {
        std::nth_element(snapshot.begin(), snapshot.begin() + snapshot.size()/2, snapshot.end());
        stats.median_us = snapshot[snapshot.size()/2];
    } else {
        // Approximate median - could implement another P-square calculator
        stats.median_us = stats.mean_us; // Rough approximation
    }
    
    // Calculate standard deviation
    stats.std_dev_us = calculate_standard_deviation(snapshot, stats.mean_us);
    
    // Add performance trend analysis
    stats.trend = calculate_performance_trend(type);
    
    return stats;
}

LatencyStatistics LatencyTracker::calculate_statistics(const std::vector<double>& data) const {
    LatencyStatistics stats;
    
    stats.count = data.size();
    
    // Find min/max using single pass for performance
    auto min_max = std::minmax_element(data.begin(), data.end());
    stats.min_us = *min_max.first;
    stats.max_us = *min_max.second;
    
    // Calculate mean using fast accumulation
    double sum = std::accumulate(data.begin(), data.end(), 0.0);
    stats.mean_us = sum / data.size();
    
    // Calculate percentiles efficiently
    stats.median_us = calculate_percentile_fast(data, 50.0);
    stats.p95_us = calculate_percentile_fast(data, 95.0);
    stats.p99_us = calculate_percentile_fast(data, 99.0);
    
    // Calculate standard deviation
    stats.std_dev_us = calculate_standard_deviation(data, stats.mean_us);
    
    return stats;
}

double LatencyTracker::calculate_percentile_fast(const std::vector<double>& data, double percentile) const {
    if (data.empty() || percentile < 0.0 || percentile > 100.0) {
        return 0.0;
    }
    
    // Use nth_element for O(n) performance instead of full sort
    std::vector<double> mutable_data(data);
    
    double index = (percentile / 100.0) * (mutable_data.size() - 1);
    size_t lower_index = static_cast<size_t>(index);
    
    if (lower_index >= mutable_data.size() - 1) {
        // Find the maximum element
        auto max_it = std::max_element(mutable_data.begin(), mutable_data.end());
        return *max_it;
    }
    
    // Use nth_element to partially sort the data
    std::nth_element(mutable_data.begin(), mutable_data.begin() + lower_index, mutable_data.end());
    double lower_val = mutable_data[lower_index];
    
    // Get the next element after partial sort
    auto upper_it = std::min_element(mutable_data.begin() + lower_index + 1, mutable_data.end());
    double upper_val = *upper_it;
    
    // Linear interpolation
    double weight = index - lower_index;
    return lower_val * (1.0 - weight) + upper_val * weight;
}

// Keep legacy method for backward compatibility
double LatencyTracker::calculate_percentile(const std::deque<double>& data, double percentile) const {
    if (data.empty() || percentile < 0.0 || percentile > 100.0) {
        return 0.0;
    }
    
    // Convert to vector and use fast method
    std::vector<double> vec_data(data.begin(), data.end());
    return calculate_percentile_fast(vec_data, percentile);
}

double LatencyTracker::calculate_standard_deviation(const std::vector<double>& data, double mean) const {
    double variance = std::accumulate(data.begin(), data.end(), 0.0,
        [mean](double acc, double value) {
            double diff = value - mean;
            return acc + diff * diff;
        }) / data.size();
    
    return std::sqrt(variance);
}

// =============================================================================
// PERFORMANCE TREND ANALYSIS
// =============================================================================

void LatencyTracker::update_trend_window(LatencyType type, double p95_latency) {
    size_t index = static_cast<size_t>(type);
    trend_windows_[index].push_back(p95_latency);
    
    // Maintain trend window size
    if (trend_windows_[index].size() > TREND_WINDOW_SIZE) {
        trend_windows_[index].pop_front();
    }
}

PerformanceTrendData LatencyTracker::calculate_performance_trend(LatencyType type) const {
    PerformanceTrendData trend_data;
    size_t index = static_cast<size_t>(type);
    const auto& trend_window = trend_windows_[index];
    
    if (trend_window.size() < 5) {  // Need minimum samples for trend analysis
        return trend_data;
    }
    
    trend_data.sample_count = static_cast<uint32_t>(trend_window.size());
    
    // Calculate linear regression slope for trend detection (fast implementation)
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    double n = static_cast<double>(trend_window.size());
    
    for (size_t i = 0; i < trend_window.size(); ++i) {
        double x = static_cast<double>(i);
        double y = trend_window[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    
    double slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
    double avg_latency = sum_y / n;
    
    // Convert slope to percentage change
    trend_data.trend_percentage = (slope / avg_latency) * 100.0;
    
    // Calculate volatility (standard deviation of changes)
    double mean_change = 0;
    for (size_t i = 1; i < trend_window.size(); ++i) {
        mean_change += (trend_window[i] - trend_window[i-1]);
    }
    mean_change /= (trend_window.size() - 1);
    
    double variance = 0;
    for (size_t i = 1; i < trend_window.size(); ++i) {
        double change = (trend_window[i] - trend_window[i-1]) - mean_change;
        variance += change * change;
    }
    trend_data.volatility = std::sqrt(variance / (trend_window.size() - 1));
    
    // Determine trend classification
    double abs_trend = std::abs(trend_data.trend_percentage);
    if (trend_data.volatility > avg_latency * 0.1) {  // High volatility threshold
        trend_data.trend = PerformanceTrend::VOLATILE;
    } else if (abs_trend < 2.0) {  // Less than 2% change
        trend_data.trend = PerformanceTrend::STABLE;
    } else if (trend_data.trend_percentage < 0) {  // Negative slope = improving (lower latency)
        trend_data.trend = PerformanceTrend::IMPROVING;
    } else {
        trend_data.trend = PerformanceTrend::DEGRADING;
    }
    
    return trend_data;
}

std::string LatencyTracker::trend_to_string(PerformanceTrend trend) const {
    switch (trend) {
        case PerformanceTrend::IMPROVING: return "📈 Improving";
        case PerformanceTrend::STABLE: return "📊 Stable";
        case PerformanceTrend::DEGRADING: return "📉 Degrading";
        case PerformanceTrend::VOLATILE: return "⚡ Volatile";
        default: return "❓ Unknown";
    }
}

// =============================================================================
// SPIKE DETECTION AND MANAGEMENT (OPTIMIZED)
// =============================================================================

void LatencyTracker::check_and_record_spike(LatencyType type, double latency_us) {
    double warning = get_threshold(type, SpikesSeverity::WARNING);
    double critical = get_threshold(type, SpikesSeverity::CRITICAL);
    
    if (latency_us > critical) {
        LatencySpike spike(now(), type, latency_us, SpikesSeverity::CRITICAL);
        spike_history_.push_back(spike);
        if (spike_history_.size() > MAX_SPIKE_HISTORY) {
            spike_history_.pop_front();
        }
    } else if (latency_us > warning) {
        LatencySpike spike(now(), type, latency_us, SpikesSeverity::WARNING);
        spike_history_.push_back(spike);
        if (spike_history_.size() > MAX_SPIKE_HISTORY) {
            spike_history_.pop_front();
        }
    }
}

// Note: check_and_record_spike_fast is now defined inline in the header

double LatencyTracker::get_threshold(LatencyType type, SpikesSeverity severity) const noexcept {
    switch(type) {
        case LatencyType::MARKET_DATA_PROCESSING:
            return (severity == SpikesSeverity::WARNING) ? MARKET_DATA_WARNING_US : MARKET_DATA_CRITICAL_US;
        case LatencyType::ORDER_PLACEMENT:
            return (severity == SpikesSeverity::WARNING) ? ORDER_PLACEMENT_WARNING_US : ORDER_PLACEMENT_CRITICAL_US;
        case LatencyType::ORDER_CANCELLATION:
            return (severity == SpikesSeverity::WARNING) ? ORDER_CANCELLATION_WARNING_US : ORDER_CANCELLATION_CRITICAL_US;
        case LatencyType::TICK_TO_TRADE:
            return (severity == SpikesSeverity::WARNING) ? TICK_TO_TRADE_WARNING_US : TICK_TO_TRADE_CRITICAL_US;
        default:
            return 0.0;
    }
}

std::vector<LatencySpike> LatencyTracker::get_recent_spikes(int minutes) const {
    auto cutoff_time = now() - std::chrono::minutes(minutes);
    std::vector<LatencySpike> spikes;
    
    std::copy_if(spike_history_.begin(), spike_history_.end(),
                 std::back_inserter(spikes),
                 [cutoff_time](const LatencySpike& spike) {
                    return spike.timestamp >= cutoff_time;
                 });
    
    return spikes;
}

bool LatencyTracker::should_alert() const {
    std::vector<LatencySpike> spikes = get_recent_spikes(1);
    
    size_t critical_count = std::count_if(spikes.begin(), spikes.end(),
                            [](const LatencySpike& spike) {
                                return spike.severity == SpikesSeverity::CRITICAL;
                            });
                
    size_t warning_count = std::count_if(spikes.begin(), spikes.end(),
                        [](const LatencySpike& spike) {
                            return spike.severity == SpikesSeverity::WARNING;
                        });
    
    return critical_count > 0 || warning_count > 3;
}

// =============================================================================
// FAST TIME FORMATTING
// =============================================================================

void LatencyTracker::format_spike_timestamp(const LatencySpike& spike, TimeFormatter::TimeBuffer& buffer) const {
    TimeFormatter::format_time_fast(spike.timestamp, buffer);
}

// =============================================================================
// STRING CONVERSION HELPERS
// =============================================================================

std::string LatencyTracker::latency_type_to_string(LatencyType type) const {
    switch(type) {
        case LatencyType::MARKET_DATA_PROCESSING: return "Market Data Processing";
        case LatencyType::ORDER_PLACEMENT: return "Order Placement";
        case LatencyType::ORDER_CANCELLATION: return "Order Cancellation";
        case LatencyType::TICK_TO_TRADE: return "Tick to Trade";
        case LatencyType::ORDER_BOOK_UPDATE: return "Order Book Update";
        default: return "Unknown";
    }
}

std::string LatencyTracker::severity_to_string(SpikesSeverity severity) const {
    switch(severity) {
        case SpikesSeverity::WARNING: return "⚠️ Warning";
        case SpikesSeverity::CRITICAL: return "🚨 Critical";
        default: return "❓ Unknown";
    }
}

// =============================================================================
// PERFORMANCE ASSESSMENT
// =============================================================================

std::string LatencyTracker::assess_performance(const LatencyStatistics& stats, LatencyType type) const {
    if (stats.p95_us < get_threshold(type, SpikesSeverity::WARNING) * 0.5) {
        return "🟢 Excellent";
    } else if (stats.p95_us < get_threshold(type, SpikesSeverity::WARNING)) {
        return "🟡 Good";
    } else if (stats.p95_us < get_threshold(type, SpikesSeverity::CRITICAL)) {
        return "🟠 Acceptable";
    } else {
        return "🔴 Poor";
    }
}

bool LatencyTracker::is_performance_acceptable(const LatencyStatistics& stats, LatencyType type) const {
    return stats.p95_us < get_threshold(type, SpikesSeverity::WARNING);
}

// =============================================================================
// REPORTING AND OUTPUT (OPTIMIZED WITH READABLE TIME FORMATTING)
// =============================================================================

void LatencyTracker::print_latency_report() const {
    std::cout << "\n🚀 === LATENCY SUMMARY REPORT === 🚀" << std::endl;
    
    // Set up formatting for nice aligned output
    std::cout << std::fixed << std::setprecision(2);
    
    // Print header with performance trends
    std::cout << std::setw(25) << "Metric" 
              << std::setw(8) << "Count"
              << std::setw(10) << "Mean"
              << std::setw(10) << "P95"
              << std::setw(10) << "P99"
              << std::setw(12) << "Grade"
              << std::setw(15) << "Trend" << std::endl;
    
    std::cout << std::string(90, '=') << std::endl;
    
    // Loop through each latency type with trend analysis
    for (size_t i = 0; i < static_cast<size_t>(LatencyType::COUNT); ++i) {
        LatencyType type = static_cast<LatencyType>(i);
        LatencyStatistics stats = get_statistics(type);
        
        if (stats.count > 0) {
            std::string type_name = latency_type_to_string(type);
            std::string grade = assess_performance(stats, type);
            std::string trend = trend_to_string(stats.trend.trend);
            
            std::cout << std::setw(25) << type_name
                      << std::setw(8) << stats.count
                      << std::setw(10) << stats.mean_us
                      << std::setw(10) << stats.p95_us
                      << std::setw(10) << stats.p99_us
                      << std::setw(12) << grade
                      << std::setw(15) << trend << std::endl;
            
            // Show trend percentage if significant
            if (std::abs(stats.trend.trend_percentage) > 1.0) {
                std::cout << std::setw(25) << ""
                          << std::setw(50) << ""
                          << "(" << std::showpos << stats.trend.trend_percentage 
                          << std::noshowpos << "%)" << std::endl;
            }
        }
    }
    
    std::cout << std::string(90, '=') << std::endl;
    
    // Format session uptime nicely
    TimeFormatter::TimeBuffer uptime_buffer;
    TimeFormatter::format_duration_fast(get_uptime_seconds() * 1000000, uptime_buffer);
    
    std::cout << "📊 Session uptime: " << uptime_buffer.data() << std::endl;
    std::cout << "📈 Total measurements: " << get_total_measurements() << std::endl;
    std::cout << "⚠️  Recent spikes: " << spike_history_.size() << std::endl;
    
    if (should_alert()) {
        std::cout << "🚨 ALERT: Performance degradation detected!" << std::endl;
    } else {
        std::cout << "✅ System operating within normal parameters" << std::endl;
    }
    std::cout << std::endl;
}

void LatencyTracker::print_detailed_report() const {
    std::cout << "\n🔍 === DETAILED LATENCY REPORT === 🔍" << std::endl;
    
    print_latency_report();
    
    std::cout << "\n⚡ === SPIKE ANALYSIS === ⚡" << std::endl;
    
    // Get recent spikes for analysis
    auto recent_spikes = get_recent_spikes(5); // Last 5 minutes
    
    if (recent_spikes.empty()) {
        std::cout << "✅ No latency spikes detected in the last 5 minutes." << std::endl;
    } else {
        // Group spikes by severity
        size_t warning_count = 0;
        size_t critical_count = 0;
        
        for (const auto& spike : recent_spikes) {
            if (spike.severity == SpikesSeverity::WARNING) {
                warning_count++;
            } else if (spike.severity == SpikesSeverity::CRITICAL) {
                critical_count++;
            }
        }
        
        std::cout << "📊 Recent spikes (last 5 minutes):" << std::endl;
        std::cout << "  ⚠️  Warnings: " << warning_count << std::endl;
        std::cout << "  🚨 Critical: " << critical_count << std::endl;
        std::cout << "  📊 Total: " << recent_spikes.size() << std::endl;
        
        // Print spike details table with readable timestamps
        std::cout << "\n🕒 Spike Details:" << std::endl;
        std::cout << std::setw(25) << "Type"
                  << std::setw(15) << "Severity"
                  << std::setw(15) << "Latency"
                  << std::setw(15) << "Time" << std::endl;
        
        std::cout << std::string(70, '-') << std::endl;
        
        for (const auto& spike : recent_spikes) {
            TimeFormatter::TimeBuffer time_buffer, latency_buffer;
            format_spike_timestamp(spike, time_buffer);
            TimeFormatter::format_duration_fast(spike.latency_us, latency_buffer);
            
            std::cout << std::setw(25) << latency_type_to_string(spike.type)
                      << std::setw(15) << severity_to_string(spike.severity)
                      << std::setw(15) << latency_buffer.data()
                      << std::setw(15) << time_buffer.data() << std::endl;
        }
    }
    
    // Performance trend summary
    std::cout << "\n📈 === PERFORMANCE TRENDS === 📈" << std::endl;
    for (size_t i = 0; i < static_cast<size_t>(LatencyType::COUNT); ++i) {
        LatencyType type = static_cast<LatencyType>(i);
        auto stats = get_statistics(type);
        
        if (stats.count > 0 && stats.trend.sample_count >= 5) {
            std::cout << latency_type_to_string(type) << ": " 
                      << trend_to_string(stats.trend.trend)
                      << " (" << std::showpos << stats.trend.trend_percentage 
                      << std::noshowpos << "%)" << std::endl;
        }
    }
    
    // Alert status with recommendations
    std::cout << "\n🎯 Alert Status: " << (should_alert() ? "🚨 ALERT" : "✅ Normal") << std::endl;
    
    if (should_alert()) {
        std::cout << "\n💡 Recommendations:" << std::endl;
        std::cout << "  • Review system load and CPU utilization" << std::endl;
        std::cout << "  • Check network connectivity and latency" << std::endl;
        std::cout << "  • Consider scaling resources or optimizing algorithms" << std::endl;
    }
}

// =============================================================================
// SYSTEM MONITORING
// =============================================================================

size_t LatencyTracker::get_total_measurements() const {
    // Use accurate deque counts instead of fast buffers for window compliance
    size_t total = 0;
    for (const auto& window : latency_windows_) {
        total += window.size();
    }
    return total;
}

size_t LatencyTracker::get_measurement_count(LatencyType type) const {
    size_t index = static_cast<size_t>(type);
    // Use accurate deque size instead of fast buffer for window compliance
    return latency_windows_[index].size();
}

double LatencyTracker::get_uptime_seconds() const {
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now() - session_start_);
    return static_cast<double>(uptime.count());
}

// =============================================================================
// UTILITY METHODS
// =============================================================================

void LatencyTracker::reset_statistics() {
    // Reset approximate percentile calculators
    for (size_t i = 0; i < static_cast<size_t>(LatencyType::COUNT); ++i) {
        p95_calculators_[i] = ApproximatePercentile(95.0);
        p99_calculators_[i] = ApproximatePercentile(99.0);
    }
    
    // Reset deque-based structures
    for (auto& window : latency_windows_) {
        window.clear();
    }
    for (auto& trend_window : trend_windows_) {
        trend_window.clear();
    }
    
    // Reset fast buffers using the clear method
    for (auto& buffer : fast_buffers_) {
        buffer.clear();
    }
    
    session_start_ = now();
}

void LatencyTracker::clear_spike_history() {
    spike_history_.clear();
}

} // namespace hft

