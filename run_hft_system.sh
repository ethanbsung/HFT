#!/bin/bash

# HFT System Runner Script
# This script runs the complete HFT system with real market data

echo "🚀 Starting HFT Trading System..."
echo "=================================="

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
    export $(cat .env | grep -v '^#' | xargs)
fi

# Check if API credentials are available
if [ -z "$HFT_API_KEY" ] && [ -z "$COINBASE_API_KEY" ]; then
    echo "⚠️  Warning: No API credentials found in environment variables"
    echo "   The system will run in simulation mode"
    echo "   Set HFT_API_KEY and HFT_SECRET_KEY for live trading"
else
    echo "✅ API credentials loaded successfully"
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

# Run the HFT system
echo "🔄 Starting system... Press Ctrl+C to stop"
echo "=========================================="

# Run the C++ HFT system
./cpp/bin/hft_system

echo ""
echo "✅ HFT System shutdown complete!" 