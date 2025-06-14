#!/usr/bin/env python3
"""
Latency Monitoring Utility for HFT Systems

This utility provides comprehensive latency analysis and monitoring capabilities
for high-frequency trading systems.
"""

import asyncio
import time
from datetime import datetime, timezone
from quote_engine import QuoteEngine, LatencyTracker

class LatencyMonitor:
    """Advanced latency monitoring and reporting for HFT systems"""
    
    def __init__(self, quote_engine: QuoteEngine = None):
        self.quote_engine = quote_engine
        self.latency_tracker = quote_engine.latency_tracker if quote_engine else LatencyTracker()
        
    def print_detailed_latency_report(self):
        """Print comprehensive latency analysis report"""
        print("\n" + "="*80)
        print("                    HFT LATENCY ANALYSIS REPORT")
        print("="*80)
        
        summary = self.latency_tracker.get_latency_summary()
        
        if not summary:
            print("No latency data collected yet.")
            return
            
        # Header with current time
        print(f"Report Generated: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}")
        print("-"*80)
        
        # Market Data Latency
        if 'market_data' in summary:
            md = summary['market_data']
            print(f"📊 MARKET DATA PROCESSING LATENCY:")
            print(f"   Mean:   {md['mean_ms']:>6.2f}ms  |  P95:   {md['p95_ms']:>6.2f}ms  |  P99:   {md['p99_ms']:>6.2f}ms")
            print(f"   Max:    {md['max_ms']:>6.2f}ms  |  Count: {md['count']:>6d}   |  Status: {self._get_status(md['p95_ms'], 5, 10)}")
            print()
        
        # Order Placement Latency
        if 'order_placement' in summary:
            op = summary['order_placement']
            print(f"📋 ORDER PLACEMENT LATENCY:")
            print(f"   Mean:   {op['mean_ms']:>6.2f}ms  |  P95:   {op['p95_ms']:>6.2f}ms  |  P99:   {op['p99_ms']:>6.2f}ms")
            print(f"   Max:    {op['max_ms']:>6.2f}ms  |  Count: {op['count']:>6d}   |  Status: {self._get_status(op['p95_ms'], 10, 25)}")
            print()
        
        # Order-to-Fill Latency
        if 'order_to_fill' in summary:
            of = summary['order_to_fill']
            print(f"✅ ORDER-TO-FILL LATENCY:")
            print(f"   Mean:   {of['mean_ms']:>6.2f}ms  |  P95:   {of['p95_ms']:>6.2f}ms  |  P99:   {of['p99_ms']:>6.2f}ms")
            print(f"   Max:    {of['max_ms']:>6.2f}ms  |  Count: {of['count']:>6d}   |  Status: {self._get_status(of['p95_ms'], 50, 100)}")
            print()
        
        # Tick-to-Trade Latency
        if 'tick_to_trade' in summary:
            tt = summary['tick_to_trade']
            print(f"⚡ TICK-TO-TRADE LATENCY:")
            print(f"   Mean:   {tt['mean_ms']:>6.2f}ms  |  P95:   {tt['p95_ms']:>6.2f}ms  |  P99:   {tt['p99_ms']:>6.2f}ms")
            print(f"   Max:    {tt['max_ms']:>6.2f}ms  |  Count: {tt['count']:>6d}   |  Status: {self._get_status(tt['p95_ms'], 15, 30)}")
            print()
        
        # Latency Spikes Analysis
        recent_spikes = self.latency_tracker.get_recent_spikes(minutes=5)
        critical_spikes = summary.get('critical_spikes', 0)
        total_spikes = summary.get('recent_spikes', 0)
        
        print(f"🚨 LATENCY SPIKES (Last 5 minutes):")
        print(f"   Total Spikes: {total_spikes}  |  Critical: {critical_spikes}  |  Alert Level: {self._get_alert_level(critical_spikes, total_spikes)}")
        
        if recent_spikes:
            print(f"   Recent Spike Details:")
            for spike in recent_spikes[-3:]:  # Show last 3 spikes
                spike_time = spike['timestamp'].strftime('%H:%M:%S')
                print(f"   - {spike_time}: {spike['type']} = {spike['latency_us']/1000:.1f}ms ({spike['severity']})")
        
        print("-"*80)
        
        # Overall Assessment
        self._print_overall_assessment(summary)
        
        print("="*80)
    
    def _get_status(self, value, warning_threshold, critical_threshold):
        """Get status indicator based on thresholds"""
        if value > critical_threshold:
            return "🔴 CRITICAL"
        elif value > warning_threshold:
            return "🟡 WARNING"
        else:
            return "🟢 GOOD"
    
    def _get_alert_level(self, critical_spikes, total_spikes):
        """Get alert level based on spike counts"""
        if critical_spikes > 0:
            return "🔴 HIGH"
        elif total_spikes > 5:
            return "🟡 MEDIUM"
        else:
            return "🟢 LOW"
    
    def _print_overall_assessment(self, summary):
        """Print overall latency performance assessment"""
        print("🎯 OVERALL LATENCY ASSESSMENT:")
        
        total_score = 0
        max_score = 0
        
        # Score each latency type
        latency_types = [
            ('market_data', 5, 10, 'Market Data'),
            ('order_placement', 10, 25, 'Order Placement'),
            ('order_to_fill', 50, 100, 'Order-to-Fill'),
            ('tick_to_trade', 15, 30, 'Tick-to-Trade')
        ]
        
        for lat_type, warning, critical, display_name in latency_types:
            if lat_type in summary:
                max_score += 3
                p95 = summary[lat_type]['p95_ms']
                if p95 <= warning:
                    total_score += 3
                    grade = "A"
                elif p95 <= critical:
                    total_score += 2
                    grade = "B"
                else:
                    total_score += 1
                    grade = "C"
                print(f"   {display_name:<15}: {p95:>6.1f}ms (Grade: {grade})")
        
        # Factor in spikes
        critical_spikes = summary.get('critical_spikes', 0)
        if critical_spikes > 0:
            total_score = max(0, total_score - critical_spikes)
        
        # Calculate final grade
        if max_score > 0:
            percentage = (total_score / max_score) * 100
            if percentage >= 90:
                final_grade = "A+ (Excellent)"
            elif percentage >= 80:
                final_grade = "A  (Very Good)"
            elif percentage >= 70:
                final_grade = "B  (Good)"
            elif percentage >= 60:
                final_grade = "C  (Acceptable)"
            else:
                final_grade = "D  (Needs Improvement)"
            
            print(f"\n   FINAL LATENCY GRADE: {final_grade} ({percentage:.1f}%)")
        
        # Recommendations
        self._print_recommendations(summary)
    
    def _print_recommendations(self, summary):
        """Print performance recommendations based on latency analysis"""
        recommendations = []
        
        if 'market_data' in summary and summary['market_data']['p95_ms'] > 10:
            recommendations.append("Consider optimizing market data processing pipeline")
        
        if 'order_placement' in summary and summary['order_placement']['p95_ms'] > 25:
            recommendations.append("Review order placement logic for potential optimizations")
        
        if 'order_to_fill' in summary and summary['order_to_fill']['p95_ms'] > 100:
            recommendations.append("Investigate exchange connectivity and order routing")
        
        if summary.get('critical_spikes', 0) > 0:
            recommendations.append("Investigate causes of critical latency spikes")
        
        if recommendations:
            print("\n💡 RECOMMENDATIONS:")
            for i, rec in enumerate(recommendations, 1):
                print(f"   {i}. {rec}")

def main():
    """Demo of latency monitoring capabilities"""
    print("HFT Latency Monitor - Demo Mode")
    print("This would normally be integrated with your live trading system.")
    
    # Create a demo latency tracker with some sample data
    tracker = LatencyTracker()
    
    # Simulate some latency measurements
    import random
    for _ in range(100):
        tracker.add_market_data_latency(random.uniform(1000, 8000))  # 1-8ms
        tracker.add_order_placement_latency(random.uniform(5000, 20000))  # 5-20ms
        tracker.add_order_to_fill_latency(random.uniform(20000, 80000))  # 20-80ms
        tracker.add_tick_to_trade_latency(random.uniform(8000, 25000))  # 8-25ms
    
    # Add some spikes
    tracker.add_market_data_latency(15000)  # 15ms spike
    tracker.add_order_placement_latency(35000)  # 35ms spike
    
    monitor = LatencyMonitor()
    monitor.latency_tracker = tracker
    monitor.print_detailed_latency_report()

if __name__ == "__main__":
    main() 