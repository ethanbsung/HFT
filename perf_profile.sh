#!/bin/bash

# HFT System Perf Profiling Script
# This script runs the HFT system with perf profiling

echo "🔍 Starting HFT System with Perf Profiling..."
echo "=============================================="

# Check if we're in the right directory
if [ ! -f "cpp/bin/hft_system" ]; then
    echo "❌ HFT system executable not found. Building..."
    cd cpp
    make hft_system
    cd ..
fi

# Load environment variables from .env file if it exists
if [ -f ".env" ]; then
    echo "📁 Loading environment variables from .env file..."
    set -a  # automatically export all variables
    source .env
    set +a  # turn off automatic export
fi

# Set default environment variables if not set
export HFT_API_KEY=${HFT_API_KEY:-$COINBASE_API_KEY}
export HFT_SECRET_KEY=${HFT_SECRET_KEY:-$COINBASE_API_SECRET}

echo "📊 Trading Symbol: BTC-USD"
echo "🔧 System Components:"
echo "   • Market Data Feed (Real-time)"
echo "   • Order Book Engine (High-performance)"
echo "   • Signal Engine (Market Making)"
echo "   • Order Manager (Risk Management)"
echo "   • Latency Tracker (Microsecond precision)"
echo "   • Memory Pool (Zero allocations)"
echo ""

# Run the HFT system with perf profiling
echo "🔄 Starting system with perf profiling... Press Ctrl+C to stop"
echo "=============================================================="

# Use the available perf version
/usr/lib/linux-tools/6.8.0-71-generic/perf record -g -F 99 ./cpp/bin/hft_system

echo ""
echo "✅ Perf profiling complete!"
echo "📊 To analyze the results, run:"
echo "   perf report -g"
echo "   or"
echo "   perf report --stdio" 