# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Development Commands

### C++ Core System

**Primary build system (Makefile):**
```bash
# Build optimized release version
make release

# Build debug version with sanitizers
make debug

# Run all tests
make test

# Run specific tests
make test_latency
make test_orderbook
make test_data_feed
make test_signal_engine
make test_order_manager

# Build main HFT system executable
make hft_system

# Performance benchmarking
make benchmark && make bench

# Code formatting and analysis
make format
make analyze

# Clean build artifacts
make clean
```

**Alternative build system (CMake):**
```bash
# Build with CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run tests with CMake
make run_tests
```

**Test execution scripts:**
```bash
# Run tests with environment loading
cpp/run_tests.sh

# Quick test runner with Google Test
make check
```

### Python System

**Main execution:**
```bash
# Run main Python HFT system
python main.py

# Run performance comparisons
python examples/run_comparison.py
python examples/run_cpp_only.py
python examples/run_hybrid.py

# Install Python dependencies
pip install -r requirements.txt
```

**Testing:**
```bash
# Run Python tests
python -m pytest tests/ -v
python test.py
```

## High-Level Architecture

This repository contains two separate HFT system implementations that do not interact with each other:

1. **Python Implementation** - Working market making system
2. **C++ Implementation** - High-performance system (currently buggy/in development)

### Core C++ Engine (High Performance)
The C++ core is designed for sub-100ns latency performance with these key components:

- **Memory Management** (`cpp/src/memory_pool.cpp`): Lock-free memory pools achieving 75M ops/sec
- **Latency Tracking** (`cpp/src/latency_tracker.cpp`): Sub-100ns performance monitoring with 12.8ns overhead
- **Order Book Engine** (`cpp/src/orderbook_engine.cpp`): Template-based order book with price-time priority
- **Order Manager** (`cpp/src/order_manager.cpp`): Complete order lifecycle with risk integration
- **Signal Engine** (`cpp/src/signal_engine.cpp`): Market making algorithms and signal processing
- **Market Data Feed** (`cpp/src/market_data_feed.cpp`): Real-time market data ingestion

### Python Trading System (Working Implementation)
The Python system provides a complete working market making implementation:

- **Market Data Streams** (`ingest/`): Real-time WebSocket feeds from Coinbase
- **Execution Simulator** (`execution_simulator.py`): Order fill simulation and PnL tracking
- **Risk Management** (`utils/risk_manager.py`): Position limits and risk monitoring
- **Engine Abstraction** (`engines/`): Modular engine system for different trading strategies

### Data Flow Architecture

**Python System (Working):**
```
Market Data (WebSocket) → OrderbookStream/TradeStream → QuoteEngine → ExecutionSimulator
                                     ↓                       ↓              ↓
                              Risk Monitoring ← Position Updates ← PnL Tracking
```

**C++ System (In Development):**
```
Market Data Feed → Order Book Engine → Signal Engine → Order Manager → Risk Checks
                        ↓                    ↓              ↓
                Latency Tracking ← Order Execution ← Position Updates
```

### Key Design Patterns

**Separate Implementations**: Two independent systems with different approaches and maturity levels

**Python System**: 
- WebSocket-based real-time data ingestion
- Simulation-based execution with realistic fill modeling
- Comprehensive risk management and PnL tracking

**C++ System**:
- Memory Pool Pattern: Custom memory pools to avoid hot-path allocations
- Lock-Free Design: Atomic operations and lock-free data structures
- Template-Based: Order book and data structures templated for performance
- RAII Resource Management: Modern C++ resource management

## Environment Configuration

**Required environment variables** (stored in `.env` or `cdp_api_key.json`):
```bash
HFT_API_KEY=your_api_key
HFT_SECRET_KEY=your_api_secret
COINBASE_PRODUCT_ID=BTC-USD
COINBASE_WEBSOCKET_URL=wss://ws-feed.exchange.coinbase.com
```

**System dependencies:**
- C++17 compiler (GCC/Clang)
- Google Test (for C++ testing)
- OpenSSL (for WebSocket connections)
- Python 3.7+ with asyncio support

## Performance Characteristics

**Python System**: 
- Functional market making with real-time data processing
- WebSocket latency dependent on network conditions
- Suitable for strategy development and testing

**C++ System**: 
- Target performance: 12.8ns per operation, 78.1M operations/second
- Sub-100ns latency goals with zero hot-path allocations
- Currently in development with bugs to be resolved

## System Status

**Python Implementation**: ✅ Working
- Complete market making system
- Real-time market data integration
- Risk management and PnL tracking
- Ready for live trading (with proper risk controls)

**C++ Implementation**: 🚧 In Development
- High-performance components implemented
- Integration and debugging needed
- 75+ unit tests available but system has bugs
- Performance benchmarks show potential but system not fully functional

## Development Workflow

**For Python System:**
1. Use `main.py` as entry point
2. Test with different symbols via `test_symbol` variable
3. Monitor via execution simulator logs
4. Validate performance with built-in analytics

**For C++ System:**
1. Build with `make debug` for development
2. Run individual component tests to isolate issues
3. Use `make test` to run full test suite
4. Debug with sanitizers enabled in debug builds